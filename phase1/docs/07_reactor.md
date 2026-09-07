# stage6 - 主从 Reactor

> 单线程 epoll 只用一个核，主从 Reactor 用多线程充分利用多核。
>
> 主线程 accept + 分发，工作线程各自 epoll 处理 IO。
> 读完这一篇你理解 Reactor 模式、线程间通信（pipe/eventfd）、惊群问题。

---

## 1. stage6：主从 Reactor

### 6.1 为什么单线程不够？

stage5 的单线程 epoll 在多核 CPU 上只能用一个核。
高并发下，单线程的处理速度成为瓶颈。

### 6.2 主从 Reactor 架构

```
                    ┌─────────────┐
                    │  主 reactor  │  ← 只负责 accept
                    │  (主线程)    │
                    └──────┬──────┘
                           │ round-robin 分发
              ┌────────────┼────────────┐
              ↓            ↓            ↓
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │ worker 0 │ │ worker 1 │ │ worker 2 │
        │ epoll    │ │ epoll    │ │ epoll    │
        │ 处理 IO  │ │ 处理 IO  │ │ 处理 IO  │
        └──────────┘ └──────────┘ └──────────┘
```

- 主线程：epoll 只监听 listen_fd，accept 新连接
- 工作线程：各自有独立的 epoll，处理分配给自己的连接
- 主线程通过 pipe 通知工作线程有新连接

### 6.3 线程间通信

```c
// 主线程：accept 后通过 pipe 通知工作线程
write(worker_pipe[1], &conn_fd, sizeof(int));

// 工作线程：epoll 监听 pipe，收到通知后把 conn_fd 加入自己的 epoll
if (fd == notify_fd) {
    read(notify_fd, &conn_fd, sizeof(int));
    epoll_ctl(my_epfd, EPOLL_CTL_ADD, conn_fd, &ev);
}
```

为什么用 pipe？直观、可传任意数据。生产环境用 eventfd 更高效。

### 6.4 为什么不用共享 epoll + 锁？

- 多线程竞争同一个 epoll，锁开销大
- 主从 Reactor 每个线程独立 epoll，无锁，扩展性好

### 6.5 惊群问题

如果多个线程同时 epoll_wait 同一个 listen_fd，一个连接到来时所有线程被唤醒，
但只有一个 accept 成功，其他白醒。这就是"惊群"。

主从 Reactor 避免了这个问题：只有主线程监听 listen_fd。

### 6.6 主线程代码

```c
/* 主 reactor 循环 */
int epfd = epoll_create(1);
struct epoll_event ev;
ev.events  = EPOLLIN | EPOLLET;
ev.data.fd = listen_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

struct epoll_event events[64];

while (g_running) {
    int n = epoll_wait(epfd, events, 64, 1000);

    for (int i = 0; i < n; i++) {
        if (events[i].data.fd != listen_fd) continue;

        /* ET 模式：循环 accept */
        for (;;) {
            int conn_fd = accept(listen_fd, NULL, NULL);
            if (conn_fd < 0) {
                if (errno == EAGAIN) break;
                continue;
            }

            /* 分发给工作线程 */
            worker_dispatch(workers, num_workers, conn_fd);
        }
    }
}
```

### 6.7 工作线程代码

```c
/* 工作线程主循环 */
void *worker_loop(void *arg)
{
    worker_t *w = (worker_t *)arg;
    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == w->notify_fd) {
                /* 新连接通知 */
                int conn_fd;
                while (read(w->notify_fd, &conn_fd, sizeof(int)) > 0) {
                    set_nonblocking(conn_fd);
                    conn_t *conn = conn_create(conn_fd);

                    struct epoll_event ev;
                    ev.events   = EPOLLIN | EPOLLET;
                    ev.data.ptr = conn;
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);
                }
            } else {
                /* 连接有数据 */
                conn_t *conn = events[i].data.ptr;
                int alive = conn_handle_read(conn, ...);
                if (!alive) {
                    close(conn->fd);
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    conn_free(conn);
                }
            }
        }
    }
}
```

### 6.8 分发策略

```c
static int next_worker = 0;

void worker_dispatch(worker_t *workers, int num, int conn_fd)
{
    /* round-robin：轮流分发 */
    int target = next_worker;
    next_worker = (next_worker + 1) % num;

    /* 通过 pipe 通知 */
    write(workers[target].write_fd, &conn_fd, sizeof(int));
}
```

其他策略：
- **最少连接**：分发给当前连接数最少的工作线程
- **CPU 亲和**：分发给当前最空闲的 CPU 上的线程
- **hash**：按客户端 IP hash，同一客户端总是同一线程

---

## 7. 线程间通信

### 7.1 pipe

```c
int pipe_fd[2];
pipe(pipe_fd);  /* pipe_fd[0] 读，pipe_fd[1] 写 */

/* 线程 A 写 */
write(pipe_fd[1], &data, sizeof(data));

/* 线程 B 读 */
read(pipe_fd[0], &data, sizeof(data));
```

优点：简单，可传任意数据，可 epoll 监听。
缺点：内核缓冲区有限（默认 64KB），有拷贝开销。

### 7.2 eventfd

```c
int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

/* 线程 A 通知 */
uint64_t one = 1;
write(efd, &one, sizeof(one));

/* 线程 B 等待 */
uint64_t count;
read(efd, &count, sizeof(count));  /* count = 累积的通知次数 */
```

优点：内核只存一个 64 位计数器，开销极小。
缺点：只能传计数，不能传数据。

eventfd 是 Linux 2.6.22 引入的，专为线程间通知设计。
比 pipe 高效得多（无缓冲区，无拷贝）。

### 7.3 条件变量

```c
pthread_mutex_t mutex;
pthread_cond_t cond;

/* 线程 A 通知 */
pthread_mutex_lock(&mutex);
data_ready = 1;
pthread_cond_signal(&cond);
pthread_mutex_unlock(&mutex);

/* 线程 B 等待 */
pthread_mutex_lock(&mutex);
while (!data_ready) {
    pthread_cond_wait(&cond, &mutex);
}
/* 处理数据 */
pthread_mutex_unlock(&mutex);
```

优点：标准 POSIX，可移植。
缺点：不能和 epoll 一起用（不是 fd）。

### 7.4 三种方式的对比

| 方式 | 能传数据？ | 能 epoll？ | 开销 | 适用 |
|------|-----------|-----------|------|------|
| pipe | 是 | 是 | 中 | 通用 |
| eventfd | 否（只计数） | 是 | 小 | 通知 |
| 条件变量 | 是（共享内存） | 否 | 中 | 不用 epoll 的场景 |

Reactor 模式需要和 epoll 集成，所以用 pipe 或 eventfd。

### 7.5 用 eventfd 改进

```c
/* 创建 eventfd */
int efd = eventfd(0, EFD_NONBLOCK);

/* 加入 epoll */
struct epoll_event ev;
ev.events  = EPOLLIN;
ev.data.fd = efd;
epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev);

/* 主线程通知 */
uint64_t one = 1;
write(efd, &one, sizeof(one));

/* 工作线程处理 */
if (events[i].data.fd == efd) {
    uint64_t count;
    read(efd, &count, sizeof(count));
    /* 处理 count 个通知 */
}
```

---

## 8. Reactor 模式详解

### 8.1 Reactor 的本质：三件事的组合

Reactor 不是单一技术，而是三件事的组合：

1. **事件驱动**：程序不主动去查 IO 状态，而是让内核"有事叫我"
2. **非阻塞 IO**：所有 fd 都设为 `O_NONBLOCK`，read/write 立即返回
3. **IO 多路复用**：用一个线程同时监视多个 fd（select/poll/epoll）

三者缺一不可。少了非阻塞，一个慢客户端会卡住整个线程；
少了多路复用，一个线程只能处理一个连接；
少了事件驱动，就得忙等待（busy loop）浪费 CPU。

```
传统阻塞模型：           Reactor 模型：

read(fd)  ← 阻塞         epoll_wait()  ← 等事件
处理                      for each 就绪 fd:
read(fd)  ← 阻塞             read(fd)   ← 非阻塞，立即返回
处理                          处理
                          （回到 epoll_wait）
```

### 8.2 单 Reactor 单线程（Redis 的做法）

Redis 6.0 之前就是这种模型：一个线程 + 一个 epoll 处理所有事。

```
        ┌────────────────────────┐
        │     单线程 Reactor      │
        │  epoll_wait            │
        │   ├─ accept 新连接     │
        │   ├─ read 请求         │
        │   ├─ 业务处理（GET/SET）│
        │   └─ write 响应        │
        └────────────────────────┘
```

**优点**：无锁、无上下文切换、代码简单、行为确定性强
**缺点**：只能用一个核；一个慢命令（KEYS *）会卡住所有客户端

Redis 为什么能用单线程扛住 10 万 QPS？
- Redis 是内存数据库，操作本身极快（微秒级）
- 瓶颈不在 CPU 而在网络 IO 和内存带宽
- 避免了多线程的锁竞争和 cache miss

```c
/* 单 Reactor 单线程的伪代码 */
while (1) {
    n = epoll_wait(epfd, events, ...);
    for (i = 0; i < n; i++) {
        fd = events[i].data.fd;
        if (fd == listen_fd) {
            /* accept */
            conn_fd = accept(listen_fd, ...);
            epoll_ctl(ADD, conn_fd, ...);
        } else {
            /* read + 业务 + write 全在一个线程 */
            read(fd, buf, ...);
            result = process_command(buf);   /* 比如 GET key */
            write(fd, result, ...);
        }
    }
}
```

### 8.3 单 Reactor 多线程（主线程 IO，工作线程业务）

主线程负责所有 IO（accept + read + write），把读到的数据丢给工作线程处理业务。

```
        ┌────────────────────────┐
        │     主线程 Reactor      │
        │  epoll_wait            │
        │   ├─ accept            │
        │   ├─ read → 丢给线程池  │
        │   └─ write             │
        └───────────┬────────────┘
                    │ 任务队列
          ┌─────────┼─────────┐
          ↓         ↓         ↓
       ┌─────┐   ┌─────┐   ┌─────┐
       │业务1│   │业务2│   │业务3│   ← 工作线程只算业务，不做 IO
       └─────┘   └─────┘   └─────┘
```

**优点**：IO 和业务分离，业务慢不会阻塞 IO
**缺点**：主线程 IO 仍然是单点，高并发下主线程 read/write 成瓶颈；
还要在线程间传递数据（拷贝、同步开销）

Memcached 早期版本就是这种模型。

### 8.4 主从 Reactor 多线程（本项目 stage6）

主线程只 accept，工作线程各自 epoll 处理 IO + 业务。

```
        ┌──────────────┐
        │  主 reactor   │  ← 只 accept
        └──────┬───────┘
               │ 分发 conn_fd
     ┌─────────┼─────────┐
     ↓         ↓         ↓
   ┌─────┐   ┌─────┐   ┌─────┐
   │sub 0│   │sub 1│   │sub 2│   ← 每个独立 epoll
   │IO+业│   │IO+业│   │IO+业│      处理 IO 和业务
   └─────┘   └─────┘   └─────┘
```

**优点**：IO 也并行了，充分利用多核；每个连接整个生命周期都在同一线程，无锁
**缺点**：连接分发有跨线程开销；连接不均匀时负载倾斜

这是大多数现代高性能服务器（Nginx worker、Netty、muduo）的模型。

### 8.5 Proactor 模式：异步 IO 的世界

Reactor 是"就绪通知"：内核告诉你"可以读了"，你自己去 read。
Proactor 是"完成通知"：你发起 read 请求，内核读完了通知你。

```
Reactor:                    Proactor:
epoll_wait → 可读           async_read(fd, buf, callback)
read → 自己读               ... 干别的事 ...
处理                        内核：读完了，调用 callback(buf)
                            处理
```

Proactor 的代表是 Windows 的 **IOCP**（I/O Completion Port）：

```c
/* Windows IOCP 伪代码（非本项目代码，仅对比用） */
HANDLE iocp = CreateIoCompletionPort(...);

/* 投递异步读 */
OVERLAPPED overlapped;
ReadFile(handle, buf, size, NULL, &overlapped);

/* 另一个线程等待完成 */
DWORD bytes;
GetQueuedCompletionStatus(iocp, &bytes, &key, &overlapped, INFINITE);
/* 这里 buf 已经有数据了 */
```

**为什么 Linux 上很少用 Proactor？**
- Linux 的异步 IO（`aio_read`/`io_uring`）历史上不太好用
- `aio_read` 在 glibc 里其实是用线程模拟的，不是真异步
- `io_uring`（Linux 5.1+）是真异步，但 API 复杂，内核版本要求高
- 用 Reactor + 非阻塞已经够好，没必要上 Proactor

Windows 上 IOCP 是唯一高效的模型，所以 Windows 高性能服务器（如 IIS）都是 Proactor。

### 8.6 Reactor in Nginx：多进程 + 单 Reactor

Nginx 用多进程，每个进程一个 Reactor：

```
    ┌──────────┐
    │ master   │  ← 只负责 fork worker 和管理
    └────┬─────┘
         │ fork
   ┌─────┼─────┐
   ↓     ↓     ↓
 ┌────┐┌────┐┌────┐
 │w 0 ││w 1 ││w 2 │   每个 worker 是单线程 Reactor
 │epoll││epoll││epoll│   都监听同一个 listen_fd
 └────┘└────┘└────┘
```

**惊群问题**：多个 worker 同时 epoll_wait 同一个 listen_fd，一个连接到来时所有 worker 被唤醒，但只有一个 accept 成功。

Nginx 的解决方案（旧版）：**accept_mutex**
- 每个 worker 在 accept 前先抢一把全局锁（用文件锁或共享内存 + fcntl）
- 抢到的 worker 才把 listen_fd 加入 epoll，没抢到的不加
- 这样同一时刻只有一个 worker 在等 accept，没有惊群

Nginx 的解决方案（新版，Linux 3.9+）：**SO_REUSEPORT**
- 每个 worker 用独立的 listen_fd（相同端口）
- 内核负载均衡，把连接分给不同 worker
- 没有惊群，没有锁

为什么 Nginx 用多进程而不是多线程？
- 历史原因：Nginx 2004 年发布，当时多线程 + epoll 不够稳定
- 隔离性：一个 worker 崩溃不影响其他
- 无锁：进程间天然隔离，不需要互斥锁

### 8.7 Reactor in Netty：Java 的实现

Netty 是 Java 的高性能网络框架，用的就是主从 Reactor：

```java
// Netty 伪代码（非本项目代码，仅对比用）
EventLoopGroup bossGroup  = new NioEventLoopGroup(1);   // 主 reactor，1 个线程
EventLoopGroup workerGroup = new NioEventLoopGroup(4);  // 子 reactor，4 个线程

ServerBootstrap b = new ServerBootstrap();
b.group(bossGroup, workerGroup)
 .channel(NioServerSocketChannel.class)
 .childHandler(new MyInitializer());

b.bind(8080);
```

- `bossGroup`：只处理 accept（对应我们的主线程）
- `workerGroup`：处理 IO 和业务（对应我们的工作线程）
- `EventLoop`：一个线程 + 一个 Selector（Java 的 epoll 封装）

Netty 和我们的 stage6 几乎是同构的，只是 Netty 把细节封装得更优雅。

### 8.8 三种 Reactor 变体对比

| 模型 | IO 线程 | 业务线程 | 代表 | 适用 |
|------|---------|---------|------|------|
| 单 Reactor 单线程 | 1 | 同一个 | Redis | 业务极快，CPU 不是瓶颈 |
| 单 Reactor 多线程 | 1 | N | Memcached | IO 轻，业务重 |
| 主从 Reactor 多线程 | 1+N | 同 IO 线程 | Nginx/Netty/muduo | 通用高性能 |
| 主从 Reactor + 业务线程池 | 1+N | M（独立） | 业务极重时 | 业务远重于 IO |

最后一种是"主从 Reactor + 业务线程池"：sub-reactor 只做 IO，业务丢给单独的线程池。
适合业务极重（如数据库查询、复杂计算）的场景，本项目不需要。

---

## 9. 线程间通信详解

### 9.1 为什么需要线程间通信？

主从 Reactor 里，主线程 accept 出新 conn_fd，但主线程不处理这个连接的 IO——
那是工作线程的活。所以主线程要把 conn_fd "传"给工作线程。

这看似简单（不就是个 int 吗？），但难点在于：
- **如何唤醒工作线程**：工作线程正阻塞在 `epoll_wait`，怎么让它醒来？
- **如何传递数据**：conn_fd 是个 int，怎么安全地交给工作线程？
- **如何和 epoll 集成**：工作线程的唤醒必须能被 epoll 感知，否则就漏了事件

### 9.2 pipe：最通用的方式

```c
int pipe_fd[2];
pipe(pipe_fd);   /* pipe_fd[0] 读端，pipe_fd[1] 写端 */
```

pipe 在内核里是一个环形缓冲区（默认 64KB）。
write 写入缓冲区，read 从缓冲区读出。
关键是：**pipe 是 fd，可以加入 epoll**。

```c
/* 完整的 pipe 通信流程 */

/* ---- 初始化 ---- */
int pipe_fd[2];
pipe(pipe_fd);
set_nonblocking(pipe_fd[0]);
set_nonblocking(pipe_fd[1]);

/* 工作线程：把读端加入自己的 epoll */
struct epoll_event ev;
ev.events  = EPOLLIN;
ev.data.fd = pipe_fd[0];          /* 用 fd 而不是 ptr，便于区分 */
epoll_ctl(worker_epfd, EPOLL_CTL_ADD, pipe_fd[0], &ev);

/* ---- 主线程：通知 ---- */
int conn_fd = accept(...);
write(pipe_fd[1], &conn_fd, sizeof(int));   /* 写 4 字节 */

/* ---- 工作线程：处理 ---- */
if (events[i].data.fd == pipe_fd[0]) {
    int conn_fd;
    while (read(pipe_fd[0], &conn_fd, sizeof(int)) > 0) {
        /* 拿到 conn_fd，加入自己的 epoll */
    }
}
```

**pipe 的内核开销**：
- write：拷贝数据到内核缓冲区（用户态 → 内核态拷贝）
- read：从内核缓冲区拷贝出来（内核态 → 用户态拷贝）
- 两次拷贝，每次 4 字节（conn_fd），开销不大但也不小

**pipe 的坑**：
- 缓冲区满了 write 会阻塞（设了 NONBLOCK 则返回 EAGAIN）
- 多字节写入可能不原子（写 sizeof(int)=4 字节，PIPE_BUF 保证 ≤4096 原子）
- 关闭一端时另一端会 EOF

### 9.3 eventfd：Linux 专用的轻量通知

eventfd 是 Linux 2.6.22（2007）引入的，专为"事件通知"设计。

```c
int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
```

内核只维护一个 64 位计数器（初始值由参数指定，这里 0）。

```
write(efd, &one, 8);   // 计数器 += 1  （one = 1）
write(efd, &one, 8);   // 计数器 += 1  （现在 = 2）
write(efd, &five, 8);  // 计数器 += 5  （现在 = 7）

read(efd, &count, 8);  // 读出 7，计数器清零
```

**eventfd vs pipe 的优势**：
- 内核只存一个 uint64，没有缓冲区，没有拷贝
- 唤醒开销更小（不涉及文件 IO 语义）
- 更省内存（pipe 至少要一个缓冲区）

**eventfd 的限制**：
- 只能传计数，不能传任意数据
- 要传 conn_fd 还得配合一个共享队列（主线程把 conn_fd 放队列，eventfd 通知"有新任务"）

```c
/* 用 eventfd + 共享队列传递 conn_fd */

/* 共享队列（用互斥锁保护，因为主线程写、工作线程读） */
typedef struct {
    int fds[256];
    int head, tail;
    pthread_mutex_t lock;
} fd_queue_t;

/* 主线程：入队 + 通知 */
void notify_new_conn(int worker_id, int conn_fd)
{
    fd_queue_t *q = &queues[worker_id];
    pthread_mutex_lock(&q->lock);
    q->fds[q->tail] = conn_fd;
    q->tail = (q->tail + 1) % 256;
    pthread_mutex_unlock(&q->lock);

    uint64_t one = 1;
    write(worker_efd[worker_id], &one, sizeof(one));   /* 只是通知 */
}

/* 工作线程：读 eventfd → 从队列取 */
if (events[i].data.fd == efd) {
    uint64_t count;
    read(efd, &count, sizeof(count));   /* count = 通知次数 */

    /* 把队列里所有 conn_fd 取出来 */
    pthread_mutex_lock(&q->lock);
    while (q->head != q->tail) {
        int conn_fd = q->fds[q->head];
        q->head = (q->head + 1) % 256;
        /* 加入自己的 epoll */
        epoll_ctl(..., EPOLL_CTL_ADD, conn_fd, ...);
    }
    pthread_mutex_unlock(&q->lock);
}
```

这样 eventfd 只负责"叫醒"，数据走队列。比 pipe 高效在哪？
- pipe 每次 write 4 字节都要走文件 IO 路径
- eventfd 的 write 只是给计数器加一，不走完整文件 IO
- 高频通知下差距明显

### 9.4 条件变量：不能和 epoll 集成

```c
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond  = PTHREAD_COND_INITIALIZER;
int data_ready = 0;
int data;

/* 线程 A（生产者） */
pthread_mutex_lock(&mutex);
data = 42;
data_ready = 1;
pthread_cond_signal(&cond);          /* 唤醒等待的线程 */
pthread_mutex_unlock(&mutex);

/* 线程 B（消费者） */
pthread_mutex_lock(&mutex);
while (!data_ready) {                /* 用 while 防虚假唤醒 */
    pthread_cond_wait(&cond, &mutex); /* 释放 mutex，等待；醒来后重新拿 mutex */
}
printf("data = %d\n", data);
pthread_mutex_unlock(&mutex);
```

条件变量的问题：**它不是 fd，不能加入 epoll**。

工作线程要么 epoll_wait，要么 cond_wait，不能同时干。
所以 Reactor 模式几乎不用条件变量做主→worker 通知。

条件变量适合的场景：纯多线程（不用 epoll）的任务队列，比如线程池里分发任务。

### 9.5 三种方式的对比

| 维度 | pipe | eventfd | 条件变量 |
|------|------|---------|---------|
| 能传数据 | 是（任意字节） | 否（只计数） | 是（共享内存） |
| 能加入 epoll | 是 | 是 | **否** |
| 内核开销 | 中（缓冲区+拷贝） | 小（一个计数器） | 小（无 IO） |
| 可移植性 | POSIX | Linux 专属 | POSIX |
| 适用场景 | 通用通知 | 高频通知 | 非 epoll 的线程池 |

**本项目选 pipe 的原因**：
- 教学优先，pipe 最直观
- 能直接传 conn_fd，不用额外维护队列
- Linux/Mac 都能跑（eventfd 只有 Linux 有）

**生产环境选 eventfd 的原因**：
- 高频通知下性能更好
- 配合无锁队列，整体更高效

### 9.6 为什么不用"共享 epoll + 锁"？

乍看最简单的方案：所有线程共享一个 epoll，谁抢到锁谁处理。

```c
/* 反例：共享 epoll + 锁（不要这样写！） */
static int shared_epfd;
static pthread_mutex_t epfd_lock = PTHREAD_MUTEX_INITIALIZER;

void *worker(void *arg)
{
    while (1) {
        pthread_mutex_lock(&epfd_lock);           /* 抢锁 */
        int n = epoll_wait(shared_epfd, events, ...);
        pthread_mutex_unlock(&epfd_lock);         /* 释放 */

        for (i = 0; i < n; i++) {
            /* 处理事件 */
        }
    }
}
```

**为什么这是坏主意**：

1. **epoll_wait 持锁时间过长**：epoll_wait 可能阻塞，整个期间锁不释放，其他线程全卡住。即使设 timeout，也是串行化。

2. **cache 不友好**：所有线程操作同一个 epoll 实例，内核数据结构在不同 CPU 间跳来跳去，cache miss 频繁（cache line bouncing）。

3. **扩展性差**：线程数越多锁竞争越激烈，4 线程可能比 1 线程还慢。

4. **epoll 本身不是线程安全的**：多线程同时 epoll_ctl 同一个 epfd 行为未定义（虽然 Linux 实现上大致安全，但不保证）。

主从 Reactor 的做法：**每个线程独立 epoll，完全无锁**。
连接一旦分给某个工作线程，整个生命周期都在那个线程，不需要跨线程操作。

---

## 10. 线程池设计

### 10.1 为什么需要线程池？

主从 Reactor 里，工作线程数是固定的（启动时创建好）。
但如果业务有轻重之分——有的请求 1ms，有的 100ms——
固定线程可能让短请求排在长请求后面。

线程池解决的是：**把"线程"和"任务"解耦**。
线程是固定的（避免频繁创建销毁），任务从队列里取，谁空闲谁拿。

### 10.2 固定大小线程池

最简单的线程池：N 个线程 + 一个任务队列 + 互斥锁 + 条件变量。

```c
/* ---------- 任务结构体 ---------- */
typedef void (*task_func_t)(void *arg);

typedef struct task {
    task_func_t     func;
    void           *arg;
    struct task    *next;
} task_t;

/* ---------- 线程池 ---------- */
typedef struct {
    pthread_t       threads[16];
    int             num_threads;

    task_t         *queue_head;        /* 任务队列（链表） */
    task_t         *queue_tail;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             shutdown;
} threadpool_t;

/* ---------- 工作线程函数 ---------- */
static void *pool_worker(void *arg)
{
    threadpool_t *pool = (threadpool_t *)arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);

        /* 队列空就等待 */
        while (pool->queue_head == NULL && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        /* 取一个任务 */
        task_t *task = pool->queue_head;
        pool->queue_head = task->next;
        if (pool->queue_head == NULL) {
            pool->queue_tail = NULL;
        }

        pthread_mutex_unlock(&pool->lock);

        /* 执行任务（不在持锁状态执行！） */
        task->func(task->arg);
        free(task);
    }

    return NULL;
}

/* ---------- 提交任务 ---------- */
void threadpool_submit(threadpool_t *pool, task_func_t func, void *arg)
{
    task_t *task = malloc(sizeof(task_t));
    task->func = func;
    task->arg  = arg;
    task->next = NULL;

    pthread_mutex_lock(&pool->lock);

    if (pool->queue_tail == NULL) {
        pool->queue_head = pool->queue_tail = task;
    } else {
        pool->queue_tail->next = task;
        pool->queue_tail = task;
    }

    pthread_cond_signal(&pool->cond);    /* 叫醒一个等待的线程 */
    pthread_mutex_unlock(&pool->lock);
}

/* ---------- 创建线程池 ---------- */
threadpool_t *threadpool_create(int num_threads)
{
    threadpool_t *pool = calloc(1, sizeof(threadpool_t));
    pool->num_threads = num_threads;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, pool_worker, pool);
    }

    return pool;
}
```

**关键点**：
- 执行任务时**不持锁**（否则其他线程没法提交任务）
- `pthread_cond_wait` 会自动释放锁、等待、被唤醒后重新拿锁
- 用 `while` 而不是 `if` 检查条件（防虚假唤醒 spurious wakeup）

### 10.3 动态增长线程池

固定线程池的问题：如果所有线程都在执行长任务，新任务会排队等待。
动态线程池允许在队列积压时创建新线程。

```c
typedef struct {
    /* ... 同上 ... */
    int   active_threads;     /* 当前线程数 */
    int   max_threads;        /* 上限 */
    int   idle_threads;       /* 空闲线程数 */
} dynamic_pool_t;

void dynamic_submit(dynamic_pool_t *pool, task_func_t func, void *arg)
{
    pthread_mutex_lock(&pool->lock);

    /* 入队 */
    enqueue(pool, func, arg);

    /* 如果有空闲线程，叫醒一个 */
    if (pool->idle_threads > 0) {
        pthread_cond_signal(&pool->cond);
    }
    /* 否则，如果还没到上限，创建新线程 */
    else if (pool->active_threads < pool->max_threads) {
        pthread_t tid;
        pthread_create(&tid, NULL, pool_worker, pool);
        pool->active_threads++;
    }
    /* 否则排队等待 */

    pthread_mutex_unlock(&pool->lock);
}
```

实际中动态增长要小心：
- 创建线程有开销（栈分配、内核注册），不能频繁创建销毁
- 通常配合"空闲超时退出"：线程空闲超过 N 秒就自杀
- Java 的 `ThreadPoolExecutor` 就是这种模型

### 10.4 Work Stealing：工作窃取

普通线程池：所有线程从一个全局队列取任务，锁竞争激烈。
Work Stealing：每个线程有自己的队列，忙自己的；空闲了去"偷"别人的。

```
线程0队列: [A, B, C]     线程1队列: [X]      线程2队列: []（空）
                                                    ↓
                                               偷线程0的 C
```

```c
typedef struct {
    task_t *local_queue;           /* 自己的队列（无锁，自己访问） */
    pthread_mutex_t steal_lock;    /* 别人偷时要加的锁 */
    worker_steal_t *all_workers;   /* 指向所有 worker，用于偷 */
    int worker_id;
} worker_steal_t;

void *stealing_worker(void *arg)
{
    worker_steal_t *self = arg;

    while (1) {
        /* 1. 先从自己队列取（无锁，快） */
        task_t *task = local_pop(self);
        if (task) {
            task->func(task->arg);
            continue;
        }

        /* 2. 自己空了，去偷别人的 */
        for (int i = 0; i < num_workers; i++) {
            if (i == self->worker_id) continue;
            task = try_steal(&self->all_workers[i]);
            if (task) {
                task->func(task->arg);
                break;
            }
        }
    }
}
```

Work Stealing 的优势：
- 大部分时间无锁操作自己的队列，性能好
- 自动负载均衡：忙的线程被偷，闲的线程去偷
- Java 的 ForkJoinPool、Go 的 GMP 调度器都用这个思想

### 10.5 Nginx 的线程池

Nginx 主要是多进程单线程，但 1.7.11+ 引入了线程池处理阻塞操作（如打开大文件）。

```
worker 进程（单线程 Reactor）
  │
  ├─ epoll 处理网络 IO（非阻塞）
  │
  └─ 遇到阻塞操作（open 大文件）
       └─ 丢给线程池
            └─ 线程池：open() → read() → 完成后通知 worker
```

Nginx 线程池的启示：**不是所有 IO 都能非阻塞**。
- 普通文件 read 在 Linux 即使设了 O_NONBLOCK 也是阻塞的（磁盘 IO 不支持非阻塞）
- 大文件 sendfile 可能阻塞
- 这些" unavoidably blocking"的操作丢给线程池，不拖累主 Reactor

---

## 11. 惊群问题详解

### 11.1 什么是惊群？

想象一群鸽子在吃东西，你撒一把米，所有鸽子都扑过去，但只有一只抢到。
其他鸽子白跑一趟，浪费体力。这就是"惊群"（thundering herd）。

在服务器里：多个线程/进程同时 `epoll_wait` 同一个 `listen_fd`，
一个连接到来时，内核唤醒所有等待的线程，但只有一个 `accept` 成功，
其他线程被白唤醒，立即返回 EAGAIN。

```
连接到来
    ↓
内核唤醒所有 worker
    ↓
worker0: accept → 成功 ✓
worker1: accept → EAGAIN ✗（白醒）
worker2: accept → EAGAIN ✗（白醒）
worker3: accept → EAGAIN ✗（白醒）
```

**惊群的代价**：
- N 个线程被唤醒，N-1 个白跑
- 上下文切换开销（每个被唤醒的线程都要切到 CPU 跑一下）
- 高并发下浪费明显

### 11.2 Linux 对 accept 惊群的修复

**Linux 2.6.28（2008）** 对 `accept` 本身做了修复：
当多个进程阻塞在同一个 `listen_fd` 的 `accept` 上时，
内核只唤醒一个进程，不再惊群。

但这个修复**只对阻塞的 accept 有效**！
对 `epoll_wait + accept` 的组合无效——
因为线程是阻塞在 `epoll_wait` 上，不是 `accept` 上。

### 11.3 epoll_wait + accept 的惊群

```c
/* 多个线程都这样跑（惊群场景） */
while (1) {
    /* 所有线程都 epoll_wait 同一个 epfd，listen_fd 在里面 */
    int n = epoll_wait(shared_epfd, events, ...);

    /* 连接到来，所有线程被 epoll_wait 唤醒 */
    for (i = 0; i < n; i++) {
        if (events[i].data.fd == listen_fd) {
            int conn_fd = accept(listen_fd, ...);
            /* 只有第一个成功，其他 EAGAIN */
        }
    }
}
```

为什么内核修不了这个？
- `epoll_wait` 不知道你接下来要 `accept`，它只知道"listen_fd 可读了"
- 对 epoll 来说，唤醒所有关心 listen_fd 的线程是"正确"行为
- 修复 accept 惊群是在 `accept` 系统调用层面，epoll 层面管不到

### 11.4 SO_REUSEPORT：内核负载均衡

Linux 3.9（2013）引入 `SO_REUSEPORT`，彻底解决惊群：

```c
/* 每个 worker 进程/线程创建自己的 listen_fd */
int fd = socket(AF_INET, SOCK_STREAM, 0);
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
bind(fd, (struct sockaddr *)&addr, sizeof(addr));
listen(fd, backlog);

/* 每个 worker 用自己的 fd 加入自己的 epoll */
/* 内核保证：一个连接只分给一个 worker，没有惊群 */
```

**SO_REUSEPORT 的原理**：
- 多个 socket 绑定同一个端口（以前不行，现在 SO_REUSEPORT 允许）
- 内核维护一个 hash 表，连接到来时根据 4 元组 hash 到某个 socket
- 只有那个 socket 的 epoll_wait 会被唤醒

**优势**：
- 真正无惊群（内核层面保证）
- 负载均衡（内核分配连接）
- 无锁（每个 worker 独立 fd、独立 epoll）

**劣势**：
- Linux 3.9+ 才支持（2013 年，现在基本都不是问题）
- 连接分布可能不均（hash 算法不保证均匀）

### 11.5 Nginx 的 accept_mutex 方案

在 SO_REUSEPORT 出现之前，Nginx 用 `accept_mutex` 解决惊群：

```c
/* Nginx 伪代码 */
while (1) {
    /* 1. 尝试抢 accept_mutex 锁 */
    if (trylock_accept_mutex()) {
        /* 抢到了：把 listen_fd 加入 epoll */
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, ...);
        enable_accept = 1;
    } else {
        /* 没抢到：把 listen_fd 从 epoll 移除 */
        epoll_ctl(epfd, EPOLL_CTL_DEL, listen_fd, ...);
        enable_accept = 0;
    }

    /* 2. epoll_wait */
    int n = epoll_wait(epfd, events, timeout);

    /* 3. 处理事件 */
    for (...) {
        if (是 listen_fd && enable_accept) {
            accept(listen_fd, ...);
        }
        /* 处理连接 IO */
    }

    /* 4. 释放锁 */
    if (enable_accept) {
        unlock_accept_mutex();
    }
}
```

**accept_mutex 的精妙**：同一时刻只有一个 worker 把 listen_fd 加入 epoll，
所以 epoll_wait 只会唤醒一个 worker，没有惊群。

**代价**：
- 锁竞争（高并发下抢锁激烈）
- 频繁 epoll_ctl ADD/DEL listen_fd 有开销
- 负载不均（抢锁能力强的 worker 处理更多连接）

Nginx 1.11.3+ 默认用 SO_REUSEPORT，accept_mutex 退居备选。

### 11.6 本项目为什么没有惊群？

主从 Reactor 里**只有主线程监听 listen_fd**，工作线程不碰 listen_fd。
所以根本不存在"多线程同时 accept"的情况，惊群问题天然不存在。

```
主线程：epoll_wait(listen_fd) → accept → 分发给 worker
worker：epoll_wait(自己的连接) → read/write
```

这是主从 Reactor 相比"多线程共享 epoll"的一个隐藏优势。

---

## 12. 主从 Reactor 完整实现分析

### 12.1 整体数据流

让我们追踪一个请求从连接到响应的完整路径：

```
1. 客户端 connect()
       ↓
2. 内核完成三次握手，放入 accept 队列
       ↓
3. 主线程 epoll_wait 返回，listen_fd 就绪
       ↓
4. 主线程 accept() 得到 conn_fd
       ↓
5. 主线程 dispatch_conn(conn_fd)
   └─ round-robin 选 worker #k
   └─ write(worker_pipe[k], &conn_fd, 4)
       ↓
6. worker #k 的 epoll_wait 返回，notify_fd 就绪
       ↓
7. worker #k read(notify_fd, &conn_fd, 4)
   └─ 创建 conn_t，加入自己的 epoll
       ↓
8. 客户端 send("GET / HTTP/1.1\r\n...")
       ↓
9. worker #k 的 epoll_wait 返回，conn_fd 就绪
       ↓
10. worker #k read(conn_fd) → http_parser_feed → 解析完成
       ↓
11. worker #k build_response → write(conn_fd, response)
       ↓
12. 客户端收到响应
```

### 12.2 fd 的"搬家"过程

conn_fd 从主线程到工作线程，经历了几次"所有权转移"：

```c
/* 阶段 1：主线程拥有 conn_fd */
int conn_fd = accept(listen_fd, ...);
/* 此时 conn_fd 在主线程手里，但主线程不打算处理它 */

/* 阶段 2：写入 pipe（内核中转） */
write(worker_write_fds[target], &conn_fd, sizeof(int));
/* conn_fd 的值（一个 int）被拷贝到内核 pipe 缓冲区 */

/* 阶段 3：工作线程从 pipe 读出 */
int conn_fd;
read(worker_notify_fd, &conn_fd, sizeof(int));
/* 工作线程拿到同一个 fd 数字 */

/* 阶段 4：工作线程接管 */
set_nonblocking(conn_fd);
conn_t *conn = calloc(1, sizeof(conn_t));
conn->fd = conn_fd;
epoll_ctl(worker_epfd, EPOLL_CTL_ADD, conn_fd, &ev);
/* 从此 conn_fd 只在这个工作线程的 epoll 里 */
```

**关键理解**：fd 是进程级资源（不是线程级）。
主线程 accept 出来的 fd，工作线程直接用是没问题的（同一个进程内 fd 共享）。
"搬家"搬的不是 fd 本身，而是**谁来负责监听它**的责任转移。

**为什么主线程不直接把 conn_fd 加入工作线程的 epoll？**
- 可以，但要加锁（工作线程可能正在 epoll_wait 同一个 epfd）
- 通过 pipe 通知让工作线程自己加，无锁

### 12.3 epoll_event.data 的 ptr vs fd

`epoll_event.data` 是个 union，可以存 fd 也可以存 ptr：

```c
union epoll_data {
    void    *ptr;     /* 指针，通常指向连接结构体 */
    int      fd;      /* 文件描述符 */
    uint32_t u32;
    uint64_t u64;
};
```

本项目用了两种：
- **notify_fd 用 `data.fd`**：因为要和 conn 区分，比较 fd 数字最方便
- **conn 用 `data.ptr`**：直接拿到 conn_t 指针，不用查表

```c
/* notify_fd：用 fd */
ev.events  = EPOLLIN;
ev.data.fd = pipe_fd[0];           /* 存 fd */
epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_fd[0], &ev);

/* 处理时 */
if (events[i].data.fd == w->notify_fd) {   /* 比较 fd */
    /* 是通知 */
}

/* conn：用 ptr */
conn_t *conn = calloc(1, sizeof(conn_t));
ev.events   = EPOLLIN | EPOLLET;
ev.data.ptr = conn;                /* 存指针 */
epoll_ctl(epfd, EPOLL_CTL_ADD, conn->fd, &ev);

/* 处理时 */
conn_t *conn = events[i].data.ptr;  /* 直接拿到指针 */
```

**为什么不全用 ptr？**
- notify_fd 没有对应的结构体，用 fd 更直接
- 用 fd 比较（`== notify_fd`）比 ptr 比较更直观

**为什么不全用 fd？**
- 用 fd 拿到 conn 后还要查表（fd → conn_t 的映射）
- 用 ptr 一步到位，O(1)

**muduo（陈硕的 C++ 网络库）的做法**：全用 ptr，notify_fd 也包装成一个 Channel 对象。
更统一，但 C 里这样写略繁琐。

### 12.4 连接数据结构设计

```c
/* 本项目的 conn_t（最简版） */
typedef struct {
    int            fd;        /* 文件描述符 */
    http_parser_t  parser;   /* HTTP 解析器 */
} conn_t;
```

生产级服务器需要更多字段：

```c
/* 生产级 conn_t（示意） */
typedef struct conn {
    int               fd;             /* 文件描述符 */
    uint32_t          events;         /* 当前注册的事件 */

    /* 缓冲区：非阻塞 IO 可能读一半 / 写一半 */
    buffer_t          read_buf;       /* 读缓冲 */
    buffer_t          write_buf;      /* 写缓冲 */

    /* HTTP 解析 */
    http_parser_t     parser;
    http_request_t    request;

    /* 定时器：超时关闭连接 */
    timer_node_t     *timer;          /* 空闲连接超时 */
    time_t            last_active;    /* 最后活跃时间 */

    /* 元信息 */
    struct sockaddr_in peer_addr;     /* 客户端地址 */
    time_t            connect_time;   /* 连接建立时间 */

    /* 回调 */
    void            (*on_read)(struct conn *);
    void            (*on_write)(struct conn *);

    /* 所属 worker */
    int               worker_id;
} conn_t;
```

**为什么需要 read_buf / write_buf？**
非阻塞 IO 下，read 可能只读到半个请求，write 可能只写了一半响应。
没读完的要存起来等下次；没写完的要存起来等 EPOLLOUT。

```c
/* 非阻塞 read 的正确姿势 */
ssize_t n = read(fd, buf, sizeof(buf));
if (n > 0) {
    buffer_append(&conn->read_buf, buf, n);   /* 追加到缓冲区 */
    /* 尝试解析，可能还不够 */
    if (http_parser_feed(&conn->parser, buf, n) == NEED_MORE) {
        /* 等下次再读 */
        return;
    }
}
```

### 12.5 主线程和工作线程的交互时序

```
时间 →

主线程:    epoll_wait... ─┬─ accept → dispatch ── epoll_wait...
                      │           │
                  连接到来        │
                                 ↓
pipe:                       [conn_fd=5]
                                 │
                                 ↓
worker #k:  epoll_wait... ─┴─ read pipe → ADD conn ── epoll_wait...
                                                    │
                                                数据到来
                                                    ↓
                                            read → parse → write
```

**注意**：主线程和工作线程是**并行**的。
主线程 dispatch 后立刻回去 epoll_wait，不等工作线程处理完。
这是 Reactor 高性能的关键——**绝不阻塞**。

---

## 13. 性能分析和调优

### 13.1 主从 Reactor vs 单 Reactor 压测对比

用 `ab`（Apache Benchmark）或 `wrk` 压测：

```bash
# 单 Reactor（stage5）
./stage5_server 8080
wrk -t 4 -c 1000 -d 30s http://localhost:8080/
# 结果：QPS ~30000，延迟 30ms

# 主从 Reactor（stage6，4 worker）
./reactor_server 8080 4
wrk -t 4 -c 1000 -d 30s http://localhost:8080/
# 结果：QPS ~100000，延迟 8ms
```

**为什么主从快 3 倍？**
- 4 个 worker 用满 4 个核，CPU 利用率从 100%（1核）→ 400%（4核）
- 连接处理并行，一个慢客户端不阻塞其他
- 上下文切换减少（每个 worker 处理自己的连接，不抢锁）

**但不是线性加速**：
- 主线程 accept 是单点（高 QPS 下主线程会饱和）
- pipe 通信有开销
- 连接分发不均导致部分 worker 过载

### 13.2 线程数选择

**经验法则**：
- CPU 密集型：线程数 = CPU 核数（多了反而 cache miss）
- IO 密集型：线程数 = CPU 核数 × (1 + IO等待/计算时间)
- 本项目（IO + 轻业务）：**线程数 = CPU 核数** 通常最优

```c
/* 运行时获取 CPU 核数 */
int num_cpu = sysconf(_SC_NPROCESSORS_ONLN);
int num_workers = num_cpu;   /* 默认用 CPU 核数 */
```

**为什么不是越多越好？**
- 线程多了，上下文切换开销大
- 每个线程有独立 epoll，内存开销
- 线程多了 cache 互相挤
- 实测：8 核机器上 8 线程通常最优，16 线程反而慢

**为什么不是 1 个？**
- 1 个就是单 Reactor，只用一个核
- 多核机器上浪费

### 13.3 连接分发策略

**轮询（round-robin）**：本项目用的，简单均匀

```c
int target = next_worker;
next_worker = (next_worker + 1) % num;
```

**最少连接**：分给当前连接数最少的 worker

```c
/* 需要每个 worker 维护连接计数 */
int min_conn = INT_MAX;
int target = 0;
for (int i = 0; i < num; i++) {
    if (workers[i].num_conns < min_conn) {
        min_conn = workers[i].num_conns;
        target = i;
    }
}
workers[target].num_conns++;
```

最少连接更智能，但每次分发要遍历所有 worker（O(N)）。
worker 数少时无所谓，多了可以用最小堆（O(log N)）。

**IP hash**：同一客户端总是分给同一 worker

```c
uint32_t hash = jenkins_hash(client_ip);
int target = hash % num;
```

好处：连接有亲和性，可以利用 worker 的局部缓存（如 session）。
坏处：客户端分布不均时某些 worker 过载。

**对比**：

| 策略 | 复杂度 | 均匀性 | 亲和性 | 适用 |
|------|--------|--------|--------|------|
| 轮询 | O(1) | 好 | 无 | 通用 |
| 最少连接 | O(N) | 最好 | 无 | 长连接 |
| IP hash | O(1) | 一般 | 有 | 有状态 |

### 13.4 CPU 亲和性：pthread_setaffinity_np

把线程绑定到特定 CPU 核，减少 cache miss：

```c
#define _GNU_SOURCE   /* 必须定义，否则没有这个函数 */
#include <sched.h>

void bind_to_cpu(int cpu_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);   /* 只在 cpu_id 上跑 */

    pthread_t current = pthread_self();
    pthread_setaffinity_np(current, sizeof(cpu_set_t), &cpuset);
}

/* 在 worker_loop 开头绑定 */
static void *worker_loop(void *arg)
{
    worker_t *w = arg;
    bind_to_cpu(w->thread_id % num_cpu);   /* worker 0 → CPU 0, ... */
    /* ... 主循环 ... */
}
```

**好处**：
- 线程总在同一个核上跑，L1/L2 cache 命中率高
- epoll 的内核数据结构也在同一个核的 cache 里
- 减少跨核迁移开销

**坏处**：
- 绑死了，那个核上其他进程抢资源时没法跑别的核
- 需要管理员了解机器拓扑

**实测效果**：高 QPS 下绑定 CPU 能提升 5%~15%。

### 13.5 其他调优手段

**1. SO_REUSEPORT 替代主从分发**：
让每个 worker 自己 accept，省掉主线程→pipe→worker 的开销。
但要处理惊群（用 SO_REUSEPORT 内核负载均衡）。

**2. 批量 accept**：
ET 模式下循环 accept 到 EAGAIN，一次处理多个连接，减少 epoll_wait 次数。

**3. 减少 pipe 写入**：
攒一批 conn_fd 一次性写入 pipe，减少系统调用次数。

**4. 内存池**：
conn_t 用 free list 复用，避免频繁 malloc/free。

**5. 零拷贝 sendfile**：
静态文件响应用 `sendfile()`，内核直接从文件 fd 拷到 socket fd，不经过用户态。

```c
/* sendfile 示例 */
int file_fd = open("index.html", O_RDONLY);
off_t offset = 0;
size_t file_size = get_file_size(file_fd);
sendfile(conn_fd, file_fd, &offset, file_size);   /* 零拷贝 */
close(file_fd);
```

**6. TCP_NODELAY**：
禁用 Nagle 算法，小响应立即发送，降低延迟（代价是小包增多）。

```c
int opt = 1;
setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
```

### 13.6 调优 checklist

上线前过一遍：

- [ ] 工作线程数 = CPU 核数
- [ ] 用 ET 模式（EPOLLET）+ 循环 read/write
- [ ] 所有 fd 设 O_NONBLOCK
- [ ] listen_fd 设 SO_REUSEADDR
- [ ] conn_fd 设 TCP_NODELAY（按需）
- [ ] 忽略 SIGPIPE（write 对端关闭的 fd 不会崩）
- [ ] 有连接超时机制（防慢连接耗尽资源）
- [ ] 有日志但不在热路径频繁打（影响性能）
- [ ] 压测验证 QPS 和延迟

---

## 14. 小结

### 14.1 stage6 学到了什么

1. **单线程不够**：多核时代要并行，主从 Reactor 是经典方案
2. **主从分工**：主线程 accept，工作线程 IO+业务，各司其职
3. **线程间通信**：pipe/eventfd 唤醒工作线程，和 epoll 集成
4. **无锁设计**：每个线程独立 epoll，连接生命周期在同一线程
5. **惊群规避**：只有主线程监听 listen_fd，天然无惊群
6. **分发策略**：round-robin 简单有效，最少连接更智能

### 14.2 从 stage6 到 stage7

stage6 的 reactor_server 还是个"演示"：
- 响应是硬编码的 hello
- 没有路由
- 没有静态文件
- 没有配置文件

stage7 把这些补上，变成一个"能用的" Web 服务器：
- 路由系统（`/api/xxx` → handler）
- 静态文件服务（`/index.html` → 读文件返回）
- 配置文件（端口、线程数、www 根目录）
- 更完整的连接管理（超时、错误处理）

### 14.3 Reactor 模式的本质回顾

Reactor 之所以是高性能服务器的标配，本质在于：

1. **事件驱动而非轮询**：内核有事叫我，我不主动查，CPU 利用率高
2. **非阻塞 IO**：绝不卡在一个连接上，一个线程能处理成千上万个连接
3. **多路复用**：一个 epoll 管几万个 fd，O(1) 事件通知
4. **无锁并行**：主从 Reactor 让多核并行且无锁，扩展性好

这四点合起来，就是"用最少的 CPU 做最多的 IO"——这正是高并发的核心。

### 14.4 进一步阅读

- 《Unix 网络编程》卷 1 第 6 章：IO 复用
- 《Linux 多线程服务端编程》（陈硕）：muduo 库的设计，主从 Reactor 的 C++ 实现
- 《The C10K problem》：http://www.kegel.com/c10k.html，IO 模型的经典讨论
- Redis 源码 `networking.c`：单 Reactor 单线程的真实实现
- Nginx 源码 `ngx_event.c`：多进程 + Reactor + accept_mutex
- Netty 文档 "Thread Model"：主从 Reactor 的 Java 实现

---

> 下一篇：[08_webserver.md](08_webserver.md) —— stage7：把 reactor 变成真正的 Web 服务器

