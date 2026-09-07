# stage3 - select 多路复用

> 用 select 在一个进程内同时监控多个 fd，告别 fork。
>
> select 是 UNIX 最古老的 IO 多路复用，理解它才能理解 epoll 为什么更好。
> 读完这一篇你理解 fd_set 的位图本质、select 的 O(n) 遍历和 1024 上限。

---

## 1. stage3：select 多路复用

### 4.1 核心流程

```c
fd_set all_set;
FD_SET(listen_fd, &all_set);

for (;;) {
    fd_set rset = all_set;              // 拷贝（select 会修改）
    select(max_fd + 1, &rset, ...);     // 阻塞等任一 fd 就绪

    if (FD_ISSET(listen_fd, &rset)) {   // 有新连接
        conn_fd = accept(listen_fd);
        FD_SET(conn_fd, &all_set);      // 加入监控
    }

    for (int i = 0; i < MAX; i++) {     // 遍历所有客户端
        if (FD_ISSET(client[i], &rset)) {  // 这个就绪了？
            read(client[i], buf);
            write(client[i], buf);
        }
    }
}
```

### 4.2 fd_set 的本质

`fd_set` 是一个**位图**（bitmap），每位对应一个 fd：

```
fd_set（1024 bit = 128 字节）：
  bit 0:  fd=0 (stdin)
  bit 3:  fd=3 (listen_fd)  ← 关心
  bit 4:  fd=4 (conn_fd1)   ← 关心
  bit 5:  fd=5 (conn_fd2)   ← 关心
  ...
```

- `FD_SET(fd, &set)`：把第 fd 位置 1
- `FD_ISSET(fd, &set)`：检查第 fd 位是否为 1
- `FD_ZERO(&set)`：清空所有位

### 4.3 select 的三个缺点

**缺点 1：FD_SETSIZE 上限 1024**

```c
// 如果 fd > 1024，FD_SET 会内存越界！
// 因为 fd_set 只有 1024 bit
```

**缺点 2：每次都要重建 fd_set**

select 会修改传入的 fd_set（只保留就绪的 fd），所以每次调用前都要拷贝一份。

**缺点 3：O(n) 遍历**

select 返回后，你只知道"有 fd 就绪了"，不知道是哪个。必须遍历所有 fd 用 FD_ISSET 检查。

10000 个客户端，即使只有 1 个就绪，也要遍历 10000 次。

### 4.4 完整代码

```c
/* stage3_echo_select/echo_select.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>

#define BUF_SIZE 4096
#define MAX_CLIENTS 1024

int main(int argc, char *argv[])
{
    int port = argc > 1 ? atoi(argv[1]) : 8080;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 128);

    printf("echo server (select) listening on 0.0.0.0:%d\n", port);

    /* 客户端数组 */
    int clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i] = -1;

    fd_set all_set;
    FD_ZERO(&all_set);
    FD_SET(listen_fd, &all_set);
    int max_fd = listen_fd;

    char buf[BUF_SIZE];

    for (;;) {
        fd_set rset = all_set;  /* 拷贝 */
        int nready = select(max_fd + 1, &rset, NULL, NULL, NULL);
        if (nready < 0) continue;

        /* 有新连接？ */
        if (FD_ISSET(listen_fd, &rset)) {
            int conn_fd = accept(listen_fd, NULL, NULL);

            /* 加入客户端数组 */
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] < 0) {
                    clients[i] = conn_fd;
                    break;
                }
            }

            FD_SET(conn_fd, &all_set);
            if (conn_fd > max_fd) max_fd = conn_fd;

            if (--nready == 0) continue;  /* 没有更多就绪的 */
        }

        /* 检查所有客户端 */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = clients[i];
            if (fd < 0) continue;

            if (FD_ISSET(fd, &rset)) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0) {
                    /* 关闭 */
                    close(fd);
                    FD_CLR(fd, &all_set);
                    clients[i] = -1;
                } else {
                    write(fd, buf, n);
                }

                if (--nready == 0) break;  /* 没有更多就绪的 */
            }
        }
    }

    return 0;
}
```

### 4.5 select 的参数详解

```c
int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);
```

- `nfds`：最大 fd + 1（内核只扫描前 nfds 个 bit）
- `readfds`：关心可读的 fd 集合
- `writefds`：关心可写的 fd 集合
- `exceptfds`：关心异常的 fd 集合（如带外数据）
- `timeout`：超时（NULL = 永久阻塞）

返回值：就绪 fd 的总数。

### 4.6 poll：select 的改进版

```c
struct pollfd {
    int fd;         /* 关心的 fd */
    short events;   /* 关心的事件（POLLIN/POLLOUT） */
    short revents;  /* 实际发生的事件 */
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout);
```

poll 的改进：
- 没有 1024 上限（用数组代替位图）
- 不用每次重建（revents 由内核填，events 不变）

poll 的缺点：
- 还是 O(n) 遍历
- 内核每次都要遍历整个数组检查就绪

---

