# stage2 - 多进程并发（fork）

> 用 fork 为每个客户端创建子进程，实现并发。
>
> 这是 UNIX 最古老的并发模型，简单直接但有进程开销。
> 读完这一篇你理解 fork 的内核实现、写时复制（COW）、僵尸进程回收。

---

## 1. stage2：多进程并发

### 3.1 核心流程

```c
for (;;) {
    conn_fd = accept(listen_fd);
    pid = fork();
    if (pid == 0) {           // 子进程
        close(listen_fd);     // 不需要门口
        while (read(conn_fd, buf) > 0)
            write(conn_fd, buf);
        close(conn_fd);
        exit(0);
    } else {                  // 父进程
        close(conn_fd);       // 不需要通道，继续接客
    }
}
```

### 3.2 fork 后的 fd 引用计数

```
fork 前：
  父进程 fd 表：[0, 1, 2, listen_fd=3, conn_fd=4]
  内核：conn_fd=4 的引用计数 = 1

fork 后：
  父进程 fd 表：[0, 1, 2, listen_fd=3, conn_fd=4]
  子进程 fd 表：[0, 1, 2, listen_fd=3, conn_fd=4]  ← 复制
  内核：conn_fd=4 的引用计数 = 2  ← 两个进程都指向它
```

**父进程必须 close(conn_fd)**：
- 否则引用计数一直 = 2
- 子进程 close 时引用计数变 1，不真正关闭
- fd 泄漏：每来一个客户端泄漏一个 fd

**子进程必须 close(listen_fd)**：
- 否则子进程也持有 listen_fd 引用
- 父进程退出时端口不会释放

### 3.3 SIGCHLD 回收僵尸

子进程退出后不会立即消失，变成僵尸（Z 状态），等父进程 wait。
如果不回收，PID 和 task_struct 一直占用。

```c
void sigchld_handler(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;  // 循环回收所有已退出的子进程
}
```

为什么用 while 循环？信号会合并——3 个子进程同时退出可能只触发 1 次 SIGCHLD。

### 3.4 完整代码

```c
/* stage2_echo_fork/echo_fork.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>

#define BUF_SIZE 4096

static void sigchld_handler(int sig)
{
    (void)sig;
    /* 循环回收所有僵尸 */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

int main(int argc, char *argv[])
{
    int port = argc > 1 ? atoi(argv[1]) : 8080;

    /* 设置 SIGCHLD 处理 */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;  /* 被信号打断的系统调用自动重启 */
    sigaction(SIGCHLD, &sa, NULL);

    /* 创建监听 socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 128);

    printf("echo server (fork) listening on 0.0.0.0:%d\n", port);

    char buf[BUF_SIZE];

    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) continue;

        pid_t pid = fork();
        if (pid < 0) {
            /* fork 失败 */
            close(conn_fd);
            continue;
        }

        if (pid == 0) {
            /* 子进程 */
            close(listen_fd);  /* 不需要门口 */

            ssize_t n;
            while ((n = read(conn_fd, buf, sizeof(buf))) > 0) {
                write(conn_fd, buf, n);
            }

            close(conn_fd);
            exit(0);
        } else {
            /* 父进程 */
            close(conn_fd);  /* 不需要通道 */
        }
    }

    return 0;
}
```

### 3.5 缺点

- fork 开销大：复制父进程的内存空间（虽然 COW）
- 进程是重资源：每个进程几 MB 内存
- 1000 并发 = 1000 进程 = 几 GB 内存
- 进程间通信（IPC）复杂：pipe、shm、msgqueue

### 3.6 用 pthread 替代 fork

```c
/* 多线程版本 */
void *handle_client(void *arg) {
    int conn_fd = *(int *)arg;
    free(arg);

    char buf[BUF_SIZE];
    ssize_t n;
    while ((n = read(conn_fd, buf, sizeof(buf))) > 0) {
        write(conn_fd, buf, n);
    }
    close(conn_fd);
    return NULL;
}

for (;;) {
    int conn_fd = accept(listen_fd, NULL, NULL);

    int *fd_ptr = malloc(sizeof(int));
    *fd_ptr = conn_fd;

    pthread_t tid;
    pthread_create(&tid, NULL, handle_client, fd_ptr);
    pthread_detach(tid);  /* 自动回收，不需要 join */
}
```

线程比进程轻量（共享内存），但要注意：
- 线程安全（共享全局变量要加锁）
- 线程崩溃整个进程崩溃（vs 进程隔离）

---

