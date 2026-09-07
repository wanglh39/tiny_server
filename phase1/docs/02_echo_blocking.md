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

## 3. 阻塞 IO 的内核实现详解

前面只说了"read 没数据时进程被挂起"，但内核里到底发生了什么？
这一节从系统调用入口一路追到网卡中断，把阻塞 IO 的完整路径拆开。

### 3.1 read() 系统调用的完整路径

用户态调用 `read(fd, buf, n)` 后，CPU 执行 `int 0x80`（或 `syscall` 指令）陷入内核态，
内核根据系统调用号查表找到 `sys_read`，整个调用链如下：

```
用户态:  read(fd, buf, n)
           │
           │  syscall 指令（陷入内核）
           ▼
内核态:  sys_read(fd, buf, n)
           │
           ├─ 1. fd_install 反查：从进程的 files_struct 找到 file 结构
           │     current->files->fdt[fd]  →  struct file *
           │
           ├─ 2. VFS 层：调用 file->f_op->read（函数指针）
           │     对于 socket，f_op 是 socket_file_ops
           │     最终调用 sock_read_iter
           │
           ├─ 3. socket 层：调用 sock->ops->recvmsg
           │     对于 TCP，是 inet_stream_ops->recvmsg = tcp_recvmsg
           │
           ├─ 4. TCP 层：tcp_recvmsg 检查接收队列
           │     sk->sk_receive_queue 上有没有 skb（socket buffer）
           │
           ├─ 5. 有数据 → 拷贝到用户 buf，返回字节数
           │
           └─ 6. 没数据 → 调用 sk_wait_data 把当前进程挂起
                     │
                     ├─ 设置进程状态为 TASK_INTERRUPTIBLE
                     ├─ 把进程加入 sk->sk_wq->wait 等待队列
                     ├─ 调用 schedule() 切换到别的进程
                     │   （当前进程在这里"睡着"了）
                     │
                     └─ 被唤醒后回到 tcp_recvmsg 继续循环
                        再次检查队列，有数据就拷贝返回
```

关键数据结构之间的关系：

```c
/* 内核里每个打开的 fd 对应一个 file 结构 */
struct file {
    const struct file_operations *f_op;   /* 操作函数表 */
    void *private_data;                   /* 对 socket 指向 struct socket */
    /* ... */
};

/* socket 结构，连接 VFS 和网络栈 */
struct socket {
    struct file *file;                    /* 反向指针 */
    struct sock *sk;                      /* 真正的传输控制块 */
    const struct proto_ops *ops;          /* 协议操作（accept/bind/...） */
    /* ... */
};

/* sock 结构，TCP 状态机核心 */
struct sock {
    int sk_state;                         /* TCP 状态：ESTABLISHED 等 */
    struct sk_buff_head sk_receive_queue; /* 接收队列 */
    struct socket_wq *sk_wq;              /* 等待队列 */
    /* ... */
};
```

### 3.2 进程如何挂起：task_struct 状态变化

Linux 内核用 `task_struct` 描述一个进程（线程），其中 `state` 字段表示进程状态：

```c
#define TASK_RUNNING            0x0000   /* 正在运行或可运行 */
#define TASK_INTERRUPTIBLE      0x0001   /* 可中断睡眠（能被信号唤醒） */
#define TASK_UNINTERRUPTIBLE    0x0002   /* 不可中断睡眠（磁盘 IO 等） */
```

`read()` 阻塞时，进程状态变化过程：

```c
/* 内核源码简化版：sk_wait_data */
int sk_wait_data(struct sock *sk, long *timeo)
{
    DEFINE_WAIT(wait);                    /* 在栈上分配一个 wait_queue_t */

    /* ① 把当前进程的状态设为 TASK_INTERRUPTIBLE */
    prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);

    /* ② 设置超时定时器 */
    sk->sk_wait_pending = 1;

    /* ③ 再次检查是否有数据（防止唤醒后数据又被别人抢走） */
    if (!skb_queue_empty(&sk->sk_receive_queue))
        goto out;

    /* ④ 调用 schedule()：让出 CPU，调度别的进程 */
    *timeo = schedule_timeout(*timeo);

out:
    finish_wait(sk_sleep(sk), &wait);     /* ⑤ 从等待队列移除，状态恢复 */
    return 0;
}
```

`schedule()` 做的事：

```
1. 从运行队列（runqueue）中选一个状态为 TASK_RUNNING 的进程
2. 上下文切换：保存当前进程的寄存器到 task_struct
3. 加载新进程的寄存器
4. 新进程继续执行，当前进程"冻结"在 schedule() 调用处
```

被挂起的进程不在运行队列里，调度器看不到它，所以不会分配 CPU——这就是"阻塞"的本质。

### 3.3 进程如何唤醒：从网卡中断到 wait_queue

数据到达网卡后，硬件中断触发，内核最终唤醒等待的进程：

```
网卡收到数据帧
    │
    ▼
硬中断：网卡驱动调用 napi_schedule
    │
    ▼
软中断 NET_RX_SOFTIRQ：net_rx_action
    │
    ├─ 1. napi_poll → 从网卡接收环取数据
    ├─ 2. netif_receive_skb → 交给协议栈
    ├─ 3. ip_rcv → ip_local_deliver → tcp_v4_rcv
    ├─ 4. tcp_v4_rcv → 找到对应的 sock → tcp_queue_rcv
    └─ 5. tcp_queue_rcv → 把 skb 挂到 sk->sk_receive_queue
            │
            ▼
        调用 sk->sk_data_ready(sk)
            │
            ▼
        sock_def_readable → wake_up_interruptible(sk_sleep(sk))
            │
            ├─ 遍历 wait_queue 上的每个 wait_queue_t
            ├─ 对每个挂起的进程调用 try_to_wake_up
            │   ├─ 把进程状态设为 TASK_RUNNING
            ├─ 把进程加回运行队列
            └─ 如果是别的 CPU 在跑这个进程，发 IPI 强制重新调度
```

被唤醒的进程下次被调度时，从 `schedule_timeout()` 返回处继续执行，
然后 `tcp_recvmsg` 循环检查发现队列非空，拷贝数据到用户 buf，返回。

### 3.4 进程状态转换图

```
                ┌──────────────────────────────────────┐
                │                                      │
                ▼                                      │
        ┌──────────────┐  read()无数据           ┌─────┴───────┐
        │ TASK_RUNNING │ ──────────────────►  │ TASK_       │
        │  (运行/就绪)  │                      │ INTERRUPTIBLE│
        └──────────────┘                      │  (可中断睡眠) │
                ▲                              └─────────────┘
                │                                      │
                │ schedule() 选中                       │ 数据到达
                │                                      │ → wake_up
                │                                      ▼
                └──────────────────────────────────────┘
```

关键点：
- **睡眠是主动的**：进程自己调用 `schedule()` 让出 CPU，不是被别人强制挂起。
- **唤醒是被动的**：中断处理代码把进程状态改回 RUNNING 并加入运行队列。
- **唤醒 ≠ 立即运行**：只是变成"就绪"，何时真正上 CPU 由调度器决定。
- **可能有多个进程等同一个 fd**：wake_up 会唤醒所有等待者（惊群效应）。

### 3.5 非阻塞 IO：O_NONBLOCK 与 EAGAIN

如果 socket 设置了 `O_NONBLOCK`，`read()` 行为完全不同：

```c
/* 非阻塞 read 的内核逻辑（简化） */
ssize_t tcp_recvmsg_nonblock(...)
{
    if (skb_queue_empty(&sk->sk_receive_queue)) {
        /* 不挂起，直接返回错误码 */
        return -EAGAIN;     /* 资源暂不可用，请重试 */
    }
    /* 有数据，正常拷贝 */
    return 拷贝字节数;
}
```

用户态用法：

```c
/* 创建非阻塞 socket */
int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

/* 或者把已存在的 fd 设为非阻塞 */
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);

/* read 立即返回 */
ssize_t n = read(fd, buf, sizeof(buf));
if (n == -1 && errno == EAGAIN) {
    /* 没数据，但不是错误，过会儿再读 */
} else if (n == -1) {
    /* 真出错了 */
} else {
    /* 读到 n 字节 */
}
```

非阻塞 IO 单独用没意义（要不停 read，浪费 CPU），它必须配合 **IO 多路复用**：
让 `epoll` 阻塞等"哪个 fd 就绪了"，就绪后再用非阻塞 read 读写，避免在 read/write 里卡住。

---

## 4. socket API 详解

stage1 用到了 `socket / bind / listen / accept / read / write / close` 七个 API。
这一节逐个拆解它们在内核里做了什么。

### 4.1 socket()：创建文件描述符

```c
int socket(int domain, int type, int protocol);
/* 返回：新的文件描述符（>=0），或 -1（出错） */
```

内核执行步骤：

```
1. 在 current->files->fdt 中找一个空闲的 fd 槽位（通常是 3）
2. 分配 struct socket（VFS 层）
3. 分配 struct sock（传输层，TCP 状态机）
4. 初始化 sk->sk_receive_queue、sk->sk_wq 等字段
5. 把 socket 和 sock 关联起来
6. 创建 struct file，f_op = socket_file_ops
7. fd_install(fd, file)：把 file 挂到进程的文件描述符表
8. 返回 fd
```

参数含义：
- `domain`：地址族。`AF_INET`（IPv4）、`AF_INET6`（IPv6）、`AF_UNIX`（本地套接字）
- `type`：套接字类型。`SOCK_STREAM`（TCP）、`SOCK_DGRAM`（UDP）、`SOCK_RAW`（原始）
- `protocol`：通常填 0，内核根据 domain+type 自动选

返回的 fd 是一个**整数**，它是进程文件描述符表的下标。fd=3 表示 `current->files->fdt[3]`。

### 4.2 bind()：绑定地址和端口

```c
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

`sockaddr_in` 结构体详解（IPv4 专用）：

```c
struct sockaddr_in {
    sa_family_t    sin_family;   /* 地址族：AF_INET */
    in_port_t      sin_port;     /* 端口号，网络字节序 */
    struct in_addr sin_addr;     /* IP 地址，网络字节序 */
    /* 后面有填充字节，使总长度等于 struct sockaddr */
};

struct in_addr {
    uint32_t s_addr;             /* 32 位 IPv4 地址 */
};
```

**网络字节序**问题：x86 是小端，网络是大端，所以端口和 IP 必须转换：

```c
htons(8080);              /* host to network short，端口转换 */
htonl(INADDR_ANY);        /* host to network long，IP 转换 */
inet_addr("127.0.0.1");   /* 点分十进制 → 网络字节序整数 */
inet_ntoa(addr.sin_addr); /* 网络字节序 → 点分十进制字符串 */
```

`INADDR_ANY`（即 `0.0.0.0`）表示监听所有网卡。如果机器有多个 IP，
用具体 IP 则只接受发往该 IP 的连接。

内核执行步骤：
```
1. 通过 fd 找到 struct socket
2. 检查端口是否被占用（除非设了 SO_REUSEADDR 且原占用处于 TIME_WAIT）
3. 把地址拷贝到 sk->sk_rcv_saddr、sk->sk_num 等字段
4. 把 sock 加入全局的 listening_hash 哈希表（便于后续查找）
```

### 4.3 listen()：进入 LISTEN 状态

```c
int listen(int sockfd, int backlog);
```

调用后 socket 从 `CLOSED` 变为 `LISTEN`，内核开始接受 SYN 包。

**backlog 的含义**：内核为每个 listening socket 维护两个队列：

```
            ┌─────────────────────────────────────────┐
            │       listening socket (LISTEN)          │
            └─────────────────────────────────────────┘
                          │
            ┌─────────────┴─────────────┐
            ▼                           ▼
    ┌───────────────┐           ┌───────────────┐
    │  SYN 队列     │           │  ACCEPT 队列  │
    │ (半连接队列)  │  ──握手完成──►  │ (已完成队列)  │
    │ SYN_RCVD 状态 │           │ ESTABLISHED   │
    └───────────────┘           └───────────────┘
            │                           │
        收到 SYN 放这里            accept() 从这里取
```

- **SYN 队列**：收到客户端 SYN，回 SYN+ACK，等待客户端 ACK。状态 `SYN_RCVD`。
- **ACCEPT 队列**：三次握手完成，等待应用调用 `accept()` 取走。状态 `ESTABLISHED`。
- **backlog**：两个队列长度之和的上限（不同内核实现略有差异）。

如果 ACCEPT 队列满了，新完成的连接会被丢弃，客户端会重传 ACK，最终超时。

### 4.4 accept()：取出已完成连接

```c
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
/* 返回：新的 conn_fd（非负），或 -1 */
```

内核执行步骤：
```
1. 检查 sockfd 是不是 listening socket
2. 检查 ACCEPT 队列是否为空
   ├─ 非空 → 取出一个已完成的连接，创建新的 struct file 和 fd，返回
   └─ 空 ──
        ├─ 阻塞模式：挂起在 sk->sk_wq 上，直到有新连接
        └─ 非阻塞模式：返回 -EAGAIN
3. 把客户端地址填入 addr（如果非 NULL）
```

**返回的 conn_fd 是一个新的文件描述符**，它和 listen_fd 不同：
- `listen_fd`：只用来接受连接，不收发数据
- `conn_fd`：专门和这一个客户端通信，read/write 都用它

一个服务器通常有 1 个 listen_fd 和 N 个 conn_fd（N = 当前连接数）。

### 4.5 connect()：三次握手

```c
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

客户端调用 `connect()` 触发三次握手：

```
1. 内核发送 SYN，状态 CLOSED → SYN_SENT
2. 阻塞等待服务器的 SYN+ACK
3. 收到 SYN+ACK，发送 ACK，状态 SYN_SENT → ESTABLISHED
4. connect() 返回 0
```

如果服务器没响应，connect 会阻塞到超时（默认约 75 秒）才返回 -1，errno=ETIMEDOUT。

非阻塞 socket 的 connect 会立即返回 -EINPROGRESS，握手在后台进行，
应用需要用 `select/epoll` 等待 fd 可写才表示连接建立。

### 4.6 read() / write()：数据收发

对 socket 来说，`read/write` 和 `recv/send` 等价（不传 flags）：

```c
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);

ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t send(int fd, const void *buf, size_t len, int flags);
```

`flags` 常用值：
- `MSG_DONTWAIT`：本次操作非阻塞（不影响 fd 本身的阻塞属性）
- `MSG_NOSIGNAL`：对端关闭时写不会触发 SIGPIPE
- `MSG_WAITALL`：read 等到读满 count 字节才返回（除非出错/对端关闭）

**read 返回值的含义**（必须牢记）：
- `> 0`：实际读到的字节数，可能小于请求的 count（短读）
- `= 0`：对端正常关闭（FIN 已收到），这是 EOF
- `< 0`：出错，看 errno
  - `EAGAIN`：非阻塞模式无数据
  - `EINTR`：被信号中断，应该重试
  - `ECONNRESET`：对端 RST，连接被重置

**write 返回值的含义**：
- `> 0`：实际写入字节数，可能小于请求的 count（短写）
- `< 0`：出错
  - `EPIPE`：对端已关闭，且没设 MSG_NOSIGNAL 会触发 SIGPIPE（默认杀进程）
  - `EAGAIN`：非阻塞模式内核缓冲区满

短读短写必须处理：循环写直到写完，或记录偏移下次继续。

### 4.7 close()：四次挥手

```c
int close(int fd);
```

调用 close 触发四次挥手：

```
1. 内核发送 FIN，状态 ESTABLISHED → FIN_WAIT_1
2. 收到对端 ACK，状态 FIN_WAIT_1 → FIN_WAIT_2
3. 收到对端 FIN，回 ACK，状态 FIN_WAIT_2 → TIME_WAIT
4. 等 2*MSL 后，状态 TIME_WAIT → CLOSED，端口可重用
```

**close 的语义**：把 fd 的引用计数减 1，减到 0 才真正关闭并发 FIN。
如果 fork 了子进程，父子共享同一个 fd，要父子都 close 才真正挥挥手。

想立即关连接不管缓冲区，用 `setsockopt(SO_LINGER)` 设 linger 结构：
```c
struct linger l = {.l_onoff = 1, .l_linger = 0};
setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
close(fd);   /* 发 RST 而不是 FIN，跳过 TIME_WAIT */
```

---

## 5. TCP 连接建立和关闭详解

### 5.1 三次握手：为什么是三次

```
客户端                              服务器
CLOSED                              LISTEN
  │                                   │
  │ ───── SYN, seq=x ──────────────►  │   收到 SYN，进入 SYN_RCVD
  │                                   │   创建半连接，回 SYN+ACK
  │ ◄──── SYN+ACK, seq=y, ack=x+1 ──  │
  │                                   │
  │ 进入 ESTABLISHED                  │
  │ ───── ACK, ack=y+1 ─────────────► │   收到 ACK，进入 ESTABLISHED
  │                                   │   连接移入 ACCEPT 队列
  │                                   │
  │          可以开始收发数据          │
```

**为什么两次不行？** 假设只有两次：
- 客户端发 SYN，服务器回 SYN+ACK 就算建立。
- 如果旧的 SYN 包延迟到达，服务器回 ACK 建立连接，但客户端没想连——服务器白等。
- 第三次 ACK 让服务器确认客户端确实想连，且收到了自己的 SYN+ACK。

**为什么四次不行（即不能合并 SYN+ACK）？** 实际上三次已经够，四次是浪费。
四次挥手中 FIN 和 ACK 不能合并，因为 FIN 表示"我没数据了"，
但服务器可能还有数据要发，所以先 ACK 客户端的 FIN，等自己数据发完再发 FIN。

### 5.2 四次挥手：为什么是四次

```
主动关闭方                          被动关闭方
ESTABLISHED                         ESTABLISHED
  │                                   │
  │ ───── FIN, seq=u ──────────────►  │   收到 FIN，进入 CLOSE_WAIT
  │   进入 FIN_WAIT_1                 │   （应用还没 close，可能还在发数据）
  │ ◄──── ACK, ack=u+1 ─────────────  │
  │   进入 FIN_WAIT_2                 │
  │                                   │   应用调用 close
  │ ◄──── FIN, seq=v ─────────────    │   进入 LAST_ACK
  │                                   │
  │ ───── ACK, ack=v+1 ─────────────► │   进入 CLOSED（彻底消失）
  │   进入 TIME_WAIT                  │
  │   等 2*MSL                        │
  │   进入 CLOSED                     │
```

**为什么 FIN 和 ACK 不能合并**：被动方收到 FIN 后，自己可能还有数据没发完，
所以先 ACK 对方的 FIN，等数据发完、应用 close 后再发自己的 FIN。

### 5.3 TIME_WAIT 状态：为什么存在 2MSL

主动关闭方最后会进入 TIME_WAIT，持续 2*MSL（通常 60 秒）。

**MSL（Maximum Segment Lifetime）**：一个 TCP 段在网络中存活的最长时间，RFC 建议 2 分钟，Linux 默认 30 秒。

**TIME_WAIT 存在的两个原因**：

1. **防止旧连接的延迟包干扰新连接**
   ```
   假设没有 TIME_WAIT，端口立刻重用：
   旧连接的 FIN-ACK 迷路了，延迟到达
   新连接正在用同样的端口，收到旧包 → 数据错乱
   ```
   等 2*MSL 保证旧连接的所有包都从网络上消失。

2. **确保被动方能正常关闭**
   ```
   主动方最后发的 ACK 可能丢，被动方会重传 FIN。
   如果主动方已经 CLOSED，无法回 ACK，被动方永远停在 LAST_ACK。
   在 TIME_WAIT 期间收到重传的 FIN，能再回 ACK，帮被动方正常关闭。
   ```

**2MSL 而非 1MSL 的原因**：
- ACK 最长存活 1MSL 才到被动方
- 被动方重传的 FIN 最长再存活 1MSL 才到主动方
- 总共最多 2MSL，之后肯定不会再有旧包了

**TIME_WAIT 的代价**：每个短连接服务器都会留下一个 TIME_WAIT，持续 60 秒。
高 QPS 短连接服务会堆积大量 TIME_WAIT，耗尽端口（约 28000 个可用端口）。

### 5.4 SO_REUSEADDR：解决地址占用

服务器重启时，旧连接的 TIME_WAIT 还占着端口，bind 会失败：
```
bind: Address already in use
```

`SO_REUSEADDR` 允许绑定到处于 TIME_WAIT 的端口：

```c
int reuse = 1;
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
bind(listen_fd, ...);   /* 现在能成功了 */
```

注意：
- `SO_REUSEADDR` 只对 TIME_WAIT 有效，不能让两个进程同时 listen 同一端口。
- 想两个进程同时 listen（多进程热升级），要用 `SO_REUSEPORT`（Linux 3.9+）。

### 5.5 用 ss 观察连接状态

```bash
# 看所有 TCP 连接的状态
ss -tn state TIME-WAIT
ss -tn state ESTABLISHED
ss -tn state CLOSE-WAIT

# 统计各状态数量
ss -tan | awk 'NR>1{print $1}' | sort | uniq -c
```

压测短连接时，能看到大量 TIME_WAIT 堆积在服务器上：
```
$ ss -tan | awk 'NR>1{print $1}' | sort | uniq -c
   5234 TIME-WAIT
     12 ESTAB
      1 LISTEN
```

---

## 6. 阻塞 IO 模型的深入分析

### 6.1 为什么一个进程只能服务一个连接

回顾 stage1 的主循环：

```c
for (;;) {
    conn_fd = accept(listen_fd);          /* ① 阻塞等连接 */
    while (read(conn_fd, buf) > 0)        /* ② 阻塞等数据 */
        write(conn_fd, buf, n);
    close(conn_fd);
}
```

问题出在 ②：一旦进入某个连接的 read 循环，进程就**绑死**在这个 conn_fd 上。
别的客户端连接进来，三次握手在内核里完成了，但应用层的 `accept` 永远不会被调用——
因为进程卡在 ② 的 `read` 里。

```
时间线：
t=0  accept 返回 conn_fd=4（客户端A）
t=1  read(4) 阻塞，等A发数据
t=2  客户端B连接 → 内核完成握手 → 进 ACCEPT 队列
t=3  客户端B发数据 → 数据在内核缓冲区，但应用不读
t=4  客户端A发数据 → read(4) 返回 → write 回去
t=5  客户端B还在等 → 永远等不到回声（直到A断开）
```

**根本原因**：阻塞 IO 的 `read` 一次只能等一个 fd。进程在等 fd=4 时，无法同时等 fd=5。

### 6.2 方案一：多进程

每个连接 fork 一个子进程，子进程独立处理自己的 conn_fd：

```c
for (;;) {
    conn_fd = accept(listen_fd);
    if (fork() == 0) {
        close(listen_fd);                 /* 子进程不需要 listen_fd */
        while (read(conn_fd, buf) > 0)
            write(conn_fd, buf, n);
        close(conn_fd);
        exit(0);
    }
    close(conn_fd);                       /* 父进程不需要 conn_fd */
}
```

优点：简单，进程隔离好（一个崩了不影响别的）。
缺点：
- fork 开销大（复制父进程地址空间，虽然写时复制 COW 缓解）
- 每个进程独立地址空间，进程间通信麻烦
- 1 万连接 = 1 万进程，内存和调度开销惊人

### 6.3 方案二：多线程

每个连接一个线程，比进程轻量：

```c
for (;;) {
    conn_fd = accept(listen_fd);
    pthread_create(&tid, NULL, handle_client, (void*)(long)conn_fd);
}

void *handle_client(void *arg) {
    int conn_fd = (int)(long)arg;
    while (read(conn_fd, buf) > 0)
        write(conn_fd, buf, n);
    close(conn_fd);
    return NULL;
}
```

优点：线程比进程轻量，共享地址空间，通信方便。
缺点：
- 1 万连接 = 1 万线程，每个线程默认栈 8MB，内存爆炸
- 线程切换有内核开销
- 共享地址空间，锁竞争严重

### 6.4 方案三：非阻塞 IO + 忙轮询

```c
/* 把所有 fd 设为非阻塞 */
fcntl(fd, F_SETFL, O_NONBLOCK);

for (;;) {
    for (int i = 0; i < n_fds; i++) {
        n = read(fds[i], buf, sizeof(buf));
        if (n > 0) write(fds[i], buf, n);
        else if (n == 0) /* 关闭，移除 */;
        /* n == -1 && errno == EAGAIN：没数据，跳过 */
    }
}
```

优点：单进程单线程能处理多连接。
缺点：
- **CPU 100%**：没数据也一直 read，纯空转
- 连接多了，每次循环遍历所有 fd，大部分 read 都返回 EAGAIN，浪费

### 6.5 引出 IO 多路复用

理想的模型：进程告诉内核"我关心这些 fd，有任何一个可读了叫我"，
然后睡觉；内核发现有 fd 就绪了，唤醒进程，告诉它是哪些 fd。

这正是 **IO 多路复用** 做的事：

```c
/* select 版本 */
fd_set rfds;
FD_ZERO(&rfds);
FD_SET(fd1, &rfds); FD_SET(fd2, &rfds); FD_SET(fd3, &rfds);

select(maxfd+1, &rfds, NULL, NULL, NULL);   /* 阻塞，直到有 fd 就绪 */

if (FD_ISSET(fd1, &rfds)) { read(fd1, ...); }
if (FD_ISSET(fd2, &rfds)) { read(fd2, ...); }
/* ... */
```

```c
/* epoll 版本（更高效） */
int epfd = epoll_create1(0);
epoll_ctl(epfd, EPOLL_CTL_ADD, fd1, &ev);
epoll_ctl(epfd, EPOLL_CTL_ADD, fd2, &ev);

struct epoll_event events[64];
int n = epoll_wait(epfd, events, 64, -1);    /* 阻塞，返回就绪 fd 数 */
for (int i = 0; i < n; i++) {
    read(events[i].data.fd, ...);            /* 只处理真正就绪的 */
}
```

对比：
- **select**：每次要把全部 fd 集合传给内核，内核 O(n) 遍历；有 1024 上限。
- **epoll**：fd 只在 `epoll_ctl` 时注册一次；`epoll_wait` 直接返回就绪列表，O(1)。

这就是后面 stage3（select）和 stage4（epoll）要解决的问题。

---

## 7. 代码逐行讲解和调试方法

### 7.1 用 strace 观察系统调用

`strace` 跟踪进程的系统调用，是理解阻塞 IO 的利器：

```bash
# 启动服务器并跟踪
strace -f -e trace=network,read,write ./build/bin/echo_server 8080

# 输出示例：
socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) = 3
setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
bind(3, {sa_family=AF_INET, sin_port=htons(8080), sin_addr=inet_addr("0.0.0.0")}, 16) = 0
listen(3, 128)                          = 0
accept(3, {sa_family=AF_INET, sin_port=htons(54321), sin_addr=inet_addr("127.0.0.1")}, [16]) = 4
read(4, "hello\n", 4096)               = 6
write(4, "hello\n", 6)                 = 6
read(4, ^C)                             # 卡在这里等下一个数据
```

可以清楚看到：`accept` 阻塞等连接 → 返回 fd=4 → `read` 阻塞等数据 → `write` 回写 → `read` 又阻塞。

### 7.2 用 ss / netstat 观察连接状态

```bash
# 启动服务器
./build/bin/echo_server 8080 &

# 客户端连接但故意不发数据
nc localhost 8080 &

# 看连接状态
ss -tnp
# State   Recv-Q  Send-Q  Local Address:Port  Peer Address:Port  Process
# ESTAB   0       0       127.0.0.1:8080     127.0.0.1:54321    ...

# Recv-Q 是内核接收缓冲区字节数，Send-Q 是发送缓冲区
# 客户端发了数据但服务器没 read，能看到 Recv-Q > 0
```

`netstat` 是老命令，`ss` 是它的现代替代，速度更快：
```bash
netstat -tnp           # 等价于 ss -tnp
ss -tn state ESTAB     # 只看 ESTABLISHED
ss -tn state TIME-WAIT # 只看 TIME_WAIT
ss -s                  # 连接数统计
```

### 7.3 用 gdb 调试

```bash
# 启动 gdb
gdb ./build/bin/echo_server

# 在 accept 后下断点
(gdb) break echo_blocking.c:168
(gdb) run 8080

# 连一个客户端后命中断点
(gdb) print conn_fd
$1 = 4
(gdb) print client
$2 = {sin_family = 2, sin_port = 47120, sin_addr = {s_addr = 16777343}, ...}

# 单步执行
(gdb) next
# 进入 read 循环
(gdb) step
```

调试 fork 出的子进程：
```bash
(gdb) set follow-fork-mode child
(gdb) set detach-on-fork off
(gdb) run
# fork 后会切到子进程
(gdb) info inferiors
(gdb) inferior 2    # 切到子进程
```

### 7.4 用 lsof 看 fd

```bash
# 找到服务器进程 PID
PID=$(pgrep echo_server)

# 看它打开了哪些 fd
lsof -p $PID
# COMMAND    PID  USER   FD  TYPE  DEVICE  SIZE/OFF  NODE NAME
# echo_serv  1234  user  cwd   DIR   8,1      4096     2 /home/user/webserve
# echo_serv  1234  user  txt   REG   8,1     12345  12345 /home/user/.../echo_server
# echo_serv  1234  user    0u  CHR   1,3       0t0     6 /dev/null
# echo_serv  1234  user    1u  CHR   1,3       0t0     6 /dev/null
# echo_serv  1234  user    2u  CHR   1,3       0t0     6 /dev/null
# echo_serv  1234  user    3u  IPv4  ...       0t0   TCP *:8080 (LISTEN)
# echo_serv  1234  user    4u  IPv4  ...       0t0   TCP 127.0.0.1:8080->127.0.0.1:54321 (ESTABLISHED)
```

fd=3 是 listen socket（LISTEN 状态），fd=4 是已接受的连接（ESTABLISHED）。
连第二个客户端后，能看到 fd=5 也是 ESTABLISHED，但应用层没在 read 它。

### 7.5 常见错误排查

**bind 失败：Address already in use**
```bash
# 原因：上次服务器退出后端口还在 TIME_WAIT
# 解决：加 SO_REUSEADDR（代码里已经加了）
# 或者等 60 秒
# 或者用 ss 确认：
ss -tln | grep 8080
```

**accept 返回 EMFILE**
```
原因：进程打开的 fd 数达到上限（默认 1024）
解决：ulimit -n 65536 提高上限
```

**read 返回 0 但以为是错误**
```c
/* 错误写法 */
n = read(fd, buf, sizeof(buf));
if (n < 0) { perror("read"); }   /* 漏了 n==0 的情况 */

/* 正确写法 */
n = read(fd, buf, sizeof(buf));
if (n == 0) { /* 对端关闭 */ close(fd); break; }
if (n < 0) { perror("read"); close(fd); break; }
/* n > 0：处理数据 */
```

**write 触发 SIGPIPE 杀死进程**
```
对端已经 close，本端还 write → 内核发 SIGPIPE → 默认终止进程
解决：
  1. 忽略信号：signal(SIGPIPE, SIG_IGN);
  2. 用 send 带 MSG_NOSIGNAL 标志
  3. 接受 EPIPE 错误码
```

### 7.6 性能观察

```bash
# 用 time 看服务器处理一个连接的耗时
time sh -c 'echo hello | nc localhost 8080'

# 用 top 看服务器 CPU 占用（阻塞 IO 空闲时应该接近 0）
top -p $(pgrep echo_server)

# 用 perf 看内核热点
sudo perf record -p $(pgrep echo_server) -g -- sleep 5
sudo perf report
```

阻塞 IO 空闲时 CPU 应该是 0%（进程在 schedule 里睡着）。
如果看到 CPU 100%，说明哪里在忙轮询，可能是非阻塞 fd 没配多路复用。

---

## 8. 小结

stage1 只用 60 行 C 代码就实现了一个能跑的 echo 服务器，但它暴露了阻塞 IO 的根本矛盾：
**一次 read 只能等一个 fd，进程在等数据时无法接受新连接。**

理解了这一点，就理解了为什么需要后面的方案：
- stage2 用 fork/线程让每个连接独立，代价是进程/线程开销
- stage3 用 select 让一个进程等多个 fd，代价是 O(n) 遍历
- stage4 用 epoll 让一个进程高效等多个 fd，这是高并发的最终答案

下一篇我们实现 stage2，用多进程让阻塞 IO 也能并发。

---

## 9. 思考题

1. 如果把 `read` 换成 `recv` 并加 `MSG_DONTWAIT` 标志，stage1 还能正常工作吗？为什么？
2. `listen_fd` 和 `conn_fd` 都是文件描述符，它们在内核里的 `file_operations` 一样吗？
3. 服务器调用 `accept` 时 ACCEPT 队列为空，进程挂起在哪个 wait_queue 上？
4. 客户端发数据后立刻 `close`，服务器 `read` 还能读到数据吗？（提示：FIN 和数据谁先到）
5. 为什么 `SO_REUSEADDR` 只对 TIME_WAIT 有效，对 LISTEN 状态的端口无效？
6. stage1 服务器运行时，用 `strace` 能看到 `accept` 后紧跟 `read`，但 `read` 没有超时。
   如果想让 read 30 秒没数据就断开，该怎么做？（提示：`setsockopt(SO_RCVTIMEO)`）
7. 假设有 1000 个客户端同时连 stage1，`ss` 能看到多少个 ESTABLISHED？应用层能处理几个？
8. `close` 一个还有未读数据的 socket，内核会怎么处理这些数据？（提示：RST vs 丢弃）

