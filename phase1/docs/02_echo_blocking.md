# stage1 - 阻塞 echo server

> 最简单的 echo 服务器：一个连接、一个进程、阻塞 IO。
>
> 虽然只能服务一个客户端，但它揭示了 IO 模型的本质——阻塞 IO 在内核里做了什么。
> 读完这一篇你理解 read() 为什么会"卡住"，以及四种 IO 模型的核心区别。

## 目录

1. [四种 IO 模型对比](#1-四种-io-模型对比)
2. [stage1：阻塞 echo server](#2-stage1阻塞-echo-server)
3. [stage2：多进程并发](#3-stage2多进程并发)
4. [stage3：select 多路复用](#4-stage3select-多路复用)
5. [stage4：epoll LT 与 ET](#5-stage4epoll-lt-与-et)
6. [压测结果分析](#6-压测结果分析)
7. [fd 的本质](#7-fd-的本质)
8. [阻塞 IO 的内核实现](#8-阻塞-io-的内核实现)
9. [fork 的内核实现](#9-fork-的内核实现)
10. [僵尸进程和孤儿进程](#10-僵尸进程和孤儿进程)
11. [select 的内核实现](#11-select-的内核实现)
12. [epoll 的内核实现](#12-epoll-的内核实现)
13. [LT vs ET 的内核行为](#13-lt-vs-et-的内核行为)
14. [性能调优建议](#14-性能调优建议)
15. [常见错误和调试方法](#15-常见错误和调试方法)
16. [思考题和面试题](#16-思考题和面试题)

---

## 1. 四种 IO 模型对比

| 模型 | 并发方式 | 优势 | 劣势 | 适用场景 |
|------|---------|------|------|---------|
| 阻塞 IO | 单连接 | 最简单 | 只能一个客户端 | 学习、调试 |
| 多进程 | fork | 隔离好、简单 | 进程开销大 | 连接数少 |
| select | 单进程多路复用 | 无进程开销 | O(n) 遍历、1024 上限 | 中等并发 |
| epoll | 单进程多路复用 | O(1) 取就绪、无上限 | 仅 Linux | 高并发 |

### 1.1 阻塞 IO 的本质

```
应用调用 read()
    ↓
内核：有数据吗？
    ├── 有 → 拷贝到用户空间，返回
    └── 没有 → 把进程挂起（睡眠），等数据到达
                    ↓
              网卡收到数据 → 唤醒进程 → 拷贝 → 返回
```

**关键**：read 没数据时，进程被挂起，CPU 去跑别的。不是忙等待。

**问题**：一个进程只能等一个 fd。如果想同时等 1000 个 fd，需要 1000 个进程。

### 1.2 多路复用的本质

```
应用调用 select(fd1, fd2, fd3, ...)
    ↓
内核：把这些 fd 都加入等待队列
    ↓
任一 fd 有数据 → 唤醒进程 → 返回就绪的 fd 列表
    ↓
应用遍历 fd 列表，处理就绪的
```

**关键**：一个进程能同时等多个 fd。不需要每个 fd 一个进程。

### 1.3 五种 IO 模型的完整对比

POSIX 定义了 5 种 IO 模型：

| 模型 | 等待数据时 | 数据拷贝时 | 特点 |
|------|-----------|-----------|------|
| 阻塞 IO | 阻塞 | 阻塞 | 最简单 |
| 非阻塞 IO | 立即返回（轮询） | 阻塞 | 忙等待浪费 CPU |
| IO 多路复用 | 阻塞（等多个 fd） | 阻塞 | select/poll/epoll |
| 信号驱动 IO | 异步通知 | 阻塞 | SIGIO，少用 |
| 异步 IO | 内核完成 | 内核完成 | POSIX aio，Linux 支持差 |

前 4 种在"数据拷贝"阶段都是阻塞的，只有第 5 种（异步 IO）真正完全不阻塞。
但 Linux 的异步 IO (aio) 支持不好，实际中用 IO 多路复用 + 非阻塞 fd 居多。

---

## 2. stage1：阻塞 echo server

### 2.1 核心流程

```c
listen_fd = socket();        // fd=3，"门口"
bind(listen_fd, 8080);
listen(listen_fd, 128);

for (;;) {
    conn_fd = accept(listen_fd);   // fd=4，"通道"
    while (read(conn_fd, buf) > 0)  // 阻塞等数据
        write(conn_fd, buf);        // echo 回去
    close(conn_fd);
}
```

### 2.2 为什么只能服务一个客户端？

```
时刻1：客户端A连接 → accept返回conn_fd=4 → 进入read循环
时刻2：客户端B连接 → 三次握手完成（内核做的）→ 进入已完成队列
时刻3：客户端A发数据 → read返回 → write回去
时刻4：客户端B等着 → accept不会被调用（卡在read循环里）
```

客户端B的连接在内核里已经 ESTABLISHED，但应用层不知道。
直到客户端A断开，read 返回 0，回到 accept，才能处理B。

### 2.3 验证

```bash
build/bin/echo_server 8080
# 终端1: nc localhost 8080  → 能连，能 echo
# 终端2: nc localhost 8080  → 能连（三次握手完成），但输入没反应！
```

用 `ss -tnp` 看，两个连接都是 ESTABLISHED，但只有一个在收数据。

### 2.4 完整代码

```c
/* stage1_echo_blocking/echo_blocking.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUF_SIZE 4096

int main(int argc, char *argv[])
{
    int port = argc > 1 ? atoi(argv[1]) : 8080;

    /* 1. 创建监听 socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    /* 2. 设置地址重用 */
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* 3. bind */
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));

    /* 4. listen */
    listen(listen_fd, 128);

    printf("echo server listening on 0.0.0.0:%d\n", port);

    char buf[BUF_SIZE];

    /* 5. 主循环 */
    for (;;) {
        /* 6. accept：阻塞等新连接 */
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int conn_fd = accept(listen_fd, (struct sockaddr *)&client, &client_len);
        if (conn_fd < 0) continue;

        printf("client connected: %s:%d\n",
               inet_ntoa(client.sin_addr), ntohs(client.sin_port));

        /* 7. echo 循环 */
        ssize_t n;
        while ((n = read(conn_fd, buf, sizeof(buf))) > 0) {
            write(conn_fd, buf, n);  /* echo 回去 */
        }

        /* 8. 对端关闭 */
        close(conn_fd);
        printf("client disconnected\n");
    }

    return 0;
}
```

### 2.5 代码逐行讲解

1. `socket(AF_INET, SOCK_STREAM, 0)`：创建 IPv4 TCP socket
   - `AF_INET`：IPv4
   - `SOCK_STREAM`：面向连接的 TCP（vs `SOCK_DGRAM` UDP）
   - `0`：协议自动选择（TCP 对应 IPPROTO_TCP）

2. `setsockopt(..., SO_REUSEADDR, ...)`：允许地址重用
   - 服务器重启时，旧连接可能还在 TIME_WAIT，端口被占用
   - `SO_REUSEADDR` 允许绑定到 TIME_WAIT 的端口

3. `bind(listen_fd, ...)`：把 socket 绑定到地址
   - `INADDR_ANY`：监听所有网卡（0.0.0.0）
   - `htons(port)`：端口转网络字节序

4. `listen(listen_fd, 128)`：开始监听
   - `128`：backlog，等待队列长度
   - 内核维护两个队列：未完成握手队列 + 已完成握手队列
   - backlog 是两个队列总和的上限

5. `accept(listen_fd, ...)`：取出一个已完成握手的连接
   - 阻塞直到有新连接
   - 返回新的 conn_fd，专门和这个客户端通信

6. `read(conn_fd, buf, ...)`：读数据
   - 阻塞直到有数据或对端关闭
   - 返回 > 0：读到的字节数
   - 返回 0：对端关闭
   - 返回 < 0：出错

7. `write(conn_fd, buf, n)`：写数据（echo）
   - 阻塞直到内核缓冲区有空间
   - 返回写入的字节数

8. `close(conn_fd)`：关闭连接
   - 触发四次挥手（如果没设置 SO_LINGER）

---

