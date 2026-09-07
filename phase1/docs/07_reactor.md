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

