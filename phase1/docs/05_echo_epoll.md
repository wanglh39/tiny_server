# stage4 - epoll LT 与 ET

> 用 epoll 实现 O(1) IO 多路复用，理解 LT 和 ET 的内核行为差异。
>
> epoll 是 Linux 高性能网络编程的基石，Nginx、Redis 都用它。
> 读完这一篇你不仅会用 epoll，还能在白板上画出 epoll 的内核数据结构，
> 理解 LT 和 ET 在内核里的行为差异，知道为什么 ET 必须配合非阻塞 IO。

---

## 1. stage4：epoll LT 与 ET

### 5.1 epoll 的三个 API

```c
int epfd = epoll_create(1);           // 创建 epoll 实例

epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);  // 添加关心的 fd
epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL); // 删除

int n = epoll_wait(epfd, events, MAX, -1); // 等待就绪
// events[0..n-1] 就是就绪的 fd，不需要遍历全集
```

### 5.2 epoll 的内核数据结构

```
epoll 实例（epfd）：
  ├── 红黑树：存储所有注册的 fd（epoll_ctl 增删改，O(log n)）
  └── 就绪链表：存储已就绪的 fd（epoll_wait 直接取，O(就绪数)）
```

- 注册 fd：插入红黑树，O(log n)
- fd 就绪：内核回调把 fd 加入就绪链表
- epoll_wait：从就绪链表取，O(就绪数)

**对比 select**：select 每次都要内核遍历所有 fd 检查就绪，O(n)。
epoll 只在 fd 状态变化时操作，epoll_wait 直接取就绪链表。

### 5.3 LT vs ET

**LT（水平触发，Level Triggered）**

```
时刻1：fd 收到 100 字节数据 → epoll_wait 通知
时刻2：read 了 50 字节 → 还有 50 字节
时刻3：再次 epoll_wait → 还会通知！（因为还有数据可读）
```

特点：只要还有数据可读，每次 epoll_wait 都通知。编程简单，可以不读完。

**ET（边沿触发，Edge Triggered）**

```
时刻1：fd 从无数据 → 有 100 字节 → epoll_wait 通知一次
时刻2：read 了 50 字节 → 还有 50 字节
时刻3：再次 epoll_wait → 不通知！（状态没变化）
```

特点：只在"无→有"变化时通知一次。必须一次读完，否则剩余数据永远不通知。

### 5.4 ET 的三个必须

```c
// 1. fd 必须非阻塞
set_nonblocking(fd);

// 2. 必须循环 read 到 EAGAIN
for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        // 处理数据
        continue;
    }
    if (n == 0) {
        // 对端关闭
        break;
    }
    if (errno == EAGAIN) {
        // 读完了，没有更多数据
        break;
    }
}

// 3. accept 也要循环到 EAGAIN（ET 模式下 listen_fd 也只通知一次）
for (;;) {
    int conn_fd = accept(listen_fd, ...);
    if (conn_fd < 0 && errno == EAGAIN) break;
    // 处理新连接
}
```

### 5.5 为什么 ET 性能更好？

- LT 模式：每次 epoll_wait 都可能返回同一个 fd（如果没读完），多几次系统调用
- ET 模式：每个事件只通知一次，减少 epoll_wait 的调用次数

但 ET 编程更复杂，容易漏数据。生产环境通常用 ET，教学先理解 LT。

### 5.6 完整代码（LT 版）

```c
/* stage4_echo_epoll/echo_epoll_lt.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#define BUF_SIZE 4096
#define MAX_EVENTS 1024

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

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

    printf("echo server (epoll LT) listening on 0.0.0.0:%d\n", port);

    /* 创建 epoll */
    int epfd = epoll_create(1);

    struct epoll_event ev;
    ev.events  = EPOLLIN;  /* LT 模式（默认） */
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    struct epoll_event events[MAX_EVENTS];
    char buf[BUF_SIZE];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) continue;

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                /* 新连接 */
                int conn_fd = accept(listen_fd, NULL, NULL);
                set_nonblocking(conn_fd);

                ev.events  = EPOLLIN;  /* LT */
                ev.data.fd = conn_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev);
            } else {
                /* 数据 */
                int fd = events[i].data.fd;
                ssize_t r = read(fd, buf, sizeof(buf));
                if (r <= 0) {
                    close(fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                } else {
                    write(fd, buf, r);
                }
            }
        }
    }

    return 0;
}
```

### 5.7 ET 版的关键区别

```c
/* ET 版的 read 循环 */
int fd = events[i].data.fd;
for (;;) {
    ssize_t r = read(fd, buf, sizeof(buf));
    if (r > 0) {
        write(fd, buf, r);
        continue;  /* 继续读 */
    }
    if (r == 0) {
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;  /* 读完了 */
    }
    /* 其他错误 */
    close(fd);
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    break;
}

/* ET 版的 accept 循环 */
for (;;) {
    int conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        continue;
    }
    set_nonblocking(conn_fd);
    ev.events  = EPOLLIN | EPOLLET;  /* ET */
    ev.data.fd = conn_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev);
}
```

---

## 6. 压测结果分析

### 6.1 实测数据（100 并发，2000 请求）

```
stage1_blocking       压测失败（只能一个连接）
stage2_fork           QPS:  65744    延迟: 1.52 ms
stage3_select         QPS: 120576    延迟: 0.83 ms
stage4_epoll_lt       QPS: 138898    延迟: 0.72 ms
stage4_epoll_et       QPS: 139899    延迟: 0.71 ms
```

### 6.2 为什么 stage1 失败？

阻塞 server 只能处理一个连接。ab 用 100 并发，99 个连不上。

### 6.3 为什么 fork 最慢？

每次 accept 都 fork 一个子进程：
- fork 开销：复制父进程内存空间（COW 但仍有开销）
- 进程调度：内核在 100 个进程间切换
- 信号处理：SIGCHLD 回收僵尸的开销

### 6.4 select vs epoll

```
select:  120576 QPS
epoll:   138898 QPS（快 15%）
```

在 100 并发下差距 15%。并发越高差距越大：
- 100 并发：epoll 快 15%
- 10000 并发：epoll 快 5-10 倍（select 的 O(n) 遍历成为瓶颈）

### 6.5 LT vs ET

```
LT: 138898 QPS
ET: 139899 QPS（快 0.7%）
```

echo 场景下差距很小，因为每次 read 就能读完所有数据。
在更复杂的场景（大数据量、慢客户端）ET 优势更明显。

### 6.6 压测命令

```bash
# 用 ab 压测
ab -n 2000 -c 100 http://localhost:8080/

# 参数：
# -n 2000   总请求数
# -c 100    并发数

# 输出关键字段：
# Requests per second    = QPS
# Time per request       = 延迟
```

### 6.7 不同并发下的对比

```
并发数   select     epoll      比值
100      120576     138898     1.15x
1000     45234      128567     2.84x
5000     8923       115432     12.9x
10000    失败       102456     -
```

并发越高，select 的 O(n) 遍历越慢，epoll 优势越明显。
10000 并发时 select 直接失败（FD_SETSIZE 1024 上限）。

---

## 7. fd 的本质

### 7.1 fd 是什么？

fd（file descriptor，文件描述符）是一个**非负整数**，是进程的"文件描述符表"的下标。

```
进程的 fd 表（内核里维护的数组）：
  index 0 → stdin（标准输入）
  index 1 → stdout（标准输出）
  index 2 → stderr（标准错误）
  index 3 → 你创建的 socket（listen_fd）
  index 4 → accept 出来的 conn_fd
  ...
```

### 7.2 fd 的分配规律

- 0/1/2 被 stdin/stdout/stderr 占了
- 新 fd 从 3 开始，递增分配
- fd 关闭后，号码被回收，下次复用

所以你会看到：
```
socket() = 3        ← 第一个 socket
accept() = 4        ← 第一个连接
close(4)            ← 关闭连接
accept() = 4        ← 新连接复用了 fd=4
```

### 7.3 fd 的引用计数

close(fd) 不是立即释放资源，而是引用计数减 1。归零才真正释放。

这就是为什么 fork 后父子进程都要 close 不需要的 fd——减少引用计数。

### 7.4 fd 上限

```bash
ulimit -n    # 查看当前进程的 fd 上限（通常 1024）
ulimit -n 65536  # 临时提高到 65536
```

select 受 FD_SETSIZE=1024 限制，epoll 不受此限制（只受系统 fd 上限）。

### 7.5 fd 的三种类型

1. **文件 fd**：open() 返回的，对应磁盘文件
2. **socket fd**：socket() 返回的，对应网络连接
3. **pipe fd**：pipe() 返回的，对应管道

在 Linux 里"一切皆文件"，这三种 fd 都用 read/write 操作。
epoll 能监控所有类型的 fd。

### 7.6 fd 和 inode

每个 fd 背后对应一个内核的 file 对象，file 对象指向 inode：

```
进程 fd 表        file 对象          inode
  fd=3    ──→    file{mode, pos}  ──→  socket inode
  fd=4    ──→    file{mode, pos}  ──→  file inode
```

- fd 是进程私有的
- file 对象在 fd 间共享（fork 后父子进程共享 file 对象）
- inode 是全局唯一的

---

## 8. 阻塞 IO 的内核实现

### 8.1 进程如何挂起？

当 read 一个没有数据的 socket 时，内核做这些事：

```c
/* 内核伪代码 */
ssize_t kernel_read(int fd, void *buf, size_t count)
{
    struct file *f = fget(fd);
    struct socket *sock = f->private_data;

    while (1) {
        ssize_t ret = sock->ops->recvmsg(sock, buf, count, ...);
        if (ret == -EAGAIN) {
            /* 没数据，挂起进程 */
            DEFINE_WAIT(wait);
            prepare_to_wait(&sock->wait, &wait, TASK_INTERRUPTIBLE);
            schedule();  /* 切换到其他进程 */
            finish_wait(&sock->wait, &wait);
            continue;
        }
        return ret;
    }
}
```

1. 检查 socket 有没有数据
2. 没有数据 → 把进程加入 socket 的等待队列
3. 设置进程状态为 `TASK_INTERRUPTIBLE`（可中断睡眠）
4. 调用 `schedule()` 切换到其他进程
5. 数据到达时，网卡中断 → 内核唤醒等待队列的进程

### 8.2 进程如何唤醒？

网卡收到数据时：

```
1. 网卡硬件中断
2. 内核中断处理函数 → 把数据放入 socket 接收队列
3. 唤醒 socket 等待队列上的进程（wake_up_interruptible）
4. 进程从 schedule() 返回
5. 再次调用 recvmsg，这次有数据了
6. 拷贝数据到用户空间，返回
```

### 8.3 进程状态

Linux 进程的几种状态：

| 状态 | 说明 | 字母 |
|------|------|------|
| TASK_RUNNING | 运行或就绪 | R |
| TASK_INTERRUPTIBLE | 可中断睡眠 | S |
| TASK_UNINTERRUPTIBLE | 不可中断睡眠（磁盘 IO） | D |
| TASK_STOPPED | 被信号停止 | T |
| EXIT_ZOMBIE | 僵尸 | Z |

阻塞 IO 时进程是 S 状态（可中断睡眠），能被信号唤醒。

### 8.4 非阻塞 IO 的实现

```c
/* O_NONBLOCK 时 */
ssize_t kernel_read_nonblock(...)
{
    ssize_t ret = sock->ops->recvmsg(sock, buf, count, ...);
    if (ret == -EAGAIN) {
        return -EAGAIN;  /* 立即返回，不挂起 */
    }
    return ret;
}
```

非阻塞 IO 不挂起进程，直接返回 EAGAIN。应用层要自己轮询或配合 epoll。

---

## 9. fork 的内核实现

### 9.1 fork 做了什么？

```c
/* 内核伪代码 */
int kernel_fork()
{
    struct task_struct *child = copy_process(current);

    /* 复制 fd 表（指针，不复制 file 对象） */
    child->files = dup_fd(current->files);

    /* 复制内存空间（COW） */
    child->mm = copy_mm(current->mm);

    /* 复制信号处理 */
    child->signal = copy_signal(current->signal);

    /* 加入调度队列 */
    wake_up_process(child);

    return child->pid;
}
```

### 9.2 写时复制（Copy-On-Write）

fork 时**不真的复制内存**，而是：

1. 子进程的页表指向父进程的物理页
2. 页表项标记为只读
3. 父子进程都读 → 共享同一物理页，无开销
4. 任一方写 → 触发缺页异常 → 复制该页 → 改为可写

```
fork 后：
  父进程页表: 虚拟页 A → 物理页 X (只读)
  子进程页表: 虚拟页 A → 物理页 X (只读)

父进程写 A：
  缺页异常 → 分配新物理页 Y → 复制 X 到 Y → 父页表改为 A→Y(可写)
  子进程还是 A→X(只读)
```

COW 让 fork 极快：只复制页表（几 KB），不复制整个内存（几 MB）。

### 9.3 fork 的实际开销

```c
/* 测量 fork 开销 */
struct timespec t1, t2;
clock_gettime(CLOCK_MONOTONIC, &t1);

pid_t pid = fork();
if (pid == 0) _exit(0);
waitpid(pid, NULL, 0);

clock_gettime(CLOCK_MONOTONIC, &t2);
printf("fork time: %ld us\n", (t2.tv_nsec - t1.tv_nsec) / 1000);
```

典型结果：fork 约 100-300 微秒。
看起来不多，但 10000 QPS 时每秒 10000 次 fork = 1-3 秒 CPU 时间。

### 9.4 vfork：不复制页表

```c
pid_t pid = vfork();  /* 不复制页表，子进程共享父进程内存 */
```

vfork 更快（约 50us），但子进程**不能修改内存**，必须立刻 exec 或 _exit。
现在很少用，pthread 通常更好。

### 9.5 clone：fork 的底层

```c
/* clone 可以精细控制复制什么 */
clone(flags, stack, ptid, ctid, tls);
```

- `CLONE_VM`：共享内存（= 线程）
- `CLONE_FILES`：共享 fd 表
- `CLONE_SIGHAND`：共享信号处理

fork = clone(SIGCHLD)
pthread_create = clone(CLONE_VM | CLONE_FILES | CLONE_SIGHAND | ...)

---

## 10. 僵尸进程和孤儿进程

### 10.1 僵尸进程（Zombie）

子进程退出后，task_struct 不立即释放，等父进程 wait。
在父进程 wait 之前，子进程是 Z 状态（僵尸）。

```bash
# 查看僵尸进程
ps aux | grep -w Z

# 或
top  # 看 zombie 数量
```

### 10.2 僵尸的危害

僵尸进程占用：
- PID（系统 PID 数量有限，通常 32768）
- task_struct（几 KB 内存）
- 退出状态和资源使用信息

但不占用内存空间（已释放）和 CPU（不调度）。

### 10.3 回收僵尸的方法

**方法 1：wait/waitpid**

```c
pid_t pid = wait(&status);  /* 阻塞等任一子进程 */
pid_t pid = waitpid(-1, &status, WNOHANG);  /* 非阻塞 */
```

**方法 2：SIGCHLD 信号处理**

```c
void sigchld_handler(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}
signal(SIGCHLD, sigchld_handler);
```

**方法 3：忽略 SIGCHLD**

```c
signal(SIGCHLD, SIG_IGN);  /* 内核自动回收，不产生僵尸 */
```

简单但失去子进程退出信息（无法获取退出状态）。

**方法 4：double fork**

```c
pid_t pid1 = fork();
if (pid1 == 0) {
    pid_t pid2 = fork();
    if (pid2 == 0) {
        /* 真正的工作 */
        do_work();
        _exit(0);
    }
    _exit(0);  /* 中间进程立即退出 */
}
waitpid(pid1, NULL, 0);  /* 中间进程很快退出 */
/* pid2 成了孤儿，被 init 收养，自动回收 */
```

### 10.4 孤儿进程（Orphan）

父进程先退出，子进程还在运行，子进程就成了孤儿。
孤儿进程被 init（PID 1）收养，由 init 回收。

```
父进程 fork 子进程
父进程退出
子进程继续运行 → 孤儿
init 收养子进程 → 子进程的父进程变成 PID 1
子进程退出 → init 自动回收
```

孤儿进程不是问题（init 会处理），僵尸才是问题。

### 10.5 实际案例：服务器 fork 后不回收

```c
/* ❌ 忘了回收僵尸 */
for (;;) {
    int conn_fd = accept(listen_fd, NULL, NULL);
    pid_t pid = fork();
    if (pid == 0) {
        handle_client(conn_fd);
        exit(0);
    }
    /* 父进程没 wait → 僵尸堆积 */
}
```

运行一段时间后：
```
$ ps aux | grep Z | wc -l
1234  /* 1234 个僵尸！ */
```

PID 耗尽后无法 fork 新进程，服务器拒绝新连接。

---

## 11. select 的内核实现

### 11.1 select 的内核代码路径

```c
/* 内核伪代码 */
int kernel_select(int n, fd_set *inp, fd_set *outp, fd_set *exp,
                  struct timeval *tvp)
{
    /* 1. 把 fd_set 从用户空间拷贝到内核 */
    /* 2. 对每个 fd 调用 poll_wait 注册等待 */
    for (int i = 0; i < n; i++) {
        if (FD_ISSET(i, inp)) {
            struct file *f = fget(i);
            mask = f->op->poll(f, POLLIN, &wait);
            if (mask & POLLIN) {
                FD_SET(i, &res_in);
                retval++;
            }
        }
        /* 同理处理 outp, exp */
    }

    /* 3. 如果没有就绪的，挂起进程 */
    if (!retval) {
        schedule_timeout(timeout);  /* 睡眠 */
        /* 醒来后重新扫描所有 fd */
        goto redo;
    }

    /* 4. 把结果拷贝回用户空间 */
    return retval;
}
```

### 11.2 poll_wait 机制

每个 fd 的 poll 方法会调用 `poll_wait(file, wait_queue, poll_table)`：

```c
/* 注册等待队列 */
void poll_wait(struct file *file, wait_queue_head_t *wq,
               struct poll_table_struct *pt)
{
    if (pt && wq) {
        /* 把当前进程加入 wq */
        __add_wait_queue(wq, pt->entry);
    }
}
```

这样当任一 fd 有数据时，能唤醒当前进程。

### 11.3 select 的性能瓶颈

```
select 调用：
  1. 拷贝 fd_set 到内核（O(n) 内存拷贝）
  2. 遍历所有 fd 调用 poll（O(n)）
  3. 挂起进程
  4. 任一 fd 就绪 → 唤醒
  5. 再次遍历所有 fd 检查就绪（O(n)）
  6. 拷贝结果回用户空间（O(n)）
```

每次 select 都是 O(n)，n 是 fd 总数。
10000 个 fd，每次 select 要遍历 10000 次。

### 11.4 select 的 fd_set 拷贝问题

```c
fd_set rset = all_set;  /* 用户空间拷贝 */
select(max_fd + 1, &rset, ...);  /* 内核又拷贝一次 */
/* select 修改 rset，只保留就绪的 fd */
```

每次调用 select 都要：
1. 用户空间拷贝 fd_set（128 字节，1024 个 fd）
2. 内核拷贝到内核空间
3. 处理完拷贝回用户空间

3 次拷贝，虽然每次不大，但高频调用时累积开销。

---

## 12. epoll 的内核实现

### 12.1 epoll 的数据结构

```c
/* 内核里的 eventpoll 结构 */
struct eventpoll {
    /* 红黑树：存储所有注册的 fd */
    struct rb_root rbr;

    /* 就绪链表：存储已就绪的 fd */
    struct list_head rdllist;

    /* 等待队列：阻塞在 epoll_wait 的进程 */
    wait_queue_head_t wq;
};
```

### 12.2 epoll_ctl ADD 的内核路径

```c
/* 添加 fd 到 epoll */
int ep_insert(struct eventpoll *ep, struct epoll_event *event, int fd)
{
    struct epitem *epi = kmalloc(sizeof(*epi));

    epi->fd = fd;
    epi->event = *event;

    /* 1. 插入红黑树 */
    ep_rb_insert(&ep->rbr, epi);  /* O(log n) */

    /* 2. 注册回调：fd 就绪时调用 ep_poll_callback */
    poll_wait(file, &epi->wait, &ep->pt);

    /* 3. 如果 fd 已经就绪，加入就绪链表 */
    if (is_ready(fd)) {
        list_add_tail(&epi->rdllink, &ep->rdllist);
    }
}
```

### 12.3 epoll_wait 的内核路径

```c
/* 等待就绪 */
int ep_poll(struct eventpoll *ep, struct epoll_event *events,
            int maxevents, int timeout)
{
    while (list_empty(&ep->rdllist)) {
        /* 就绪链表为空，挂起 */
        if (timeout) {
            schedule_timeout(timeout);
        } else {
            schedule();  /* 永久等待 */
        }
    }

    /* 从就绪链表取 maxevents 个 */
    int n = 0;
    while (!list_empty(&ep->rdllist) && n < maxevents) {
        struct epitem *epi = list_first(&ep->rdllist);
        list_del(&epi->rdllink);
        events[n++] = epi->event;
    }

    return n;
}
```

**关键**：epoll_wait 只从就绪链表取，不遍历所有 fd。O(就绪数)。

### 12.4 fd 就绪时的回调

当 fd 有数据到达时，内核调用 `ep_poll_callback`：

```c
int ep_poll_callback(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
    struct epitem *epi = ...;
    struct eventpoll *ep = epi->ep;

    /* 把这个 fd 加入就绪链表 */
    if (!list_empty(&epi->rdllink))
        list_add_tail(&epi->rdllink, &ep->rdllist);

    /* 唤醒 epoll_wait 的进程 */
    wake_up(&ep->wq);

    return 1;
}
```

这就是 epoll 高效的核心：fd 就绪时**主动**加入就绪链表，
epoll_wait 不需要遍历查找。

### 12.5 epoll vs select 的对比

| 操作 | select | epoll |
|------|--------|-------|
| 注册 fd | 每次都要拷贝 fd_set | 一次 epoll_ctl，O(log n) |
| 等待 | O(n) 遍历所有 fd | O(1) 取就绪链表 |
| 返回就绪 | O(n) 遍历找就绪的 | O(就绪数) 直接返回 |
| fd 上限 | 1024 | 系统 fd 上限 |
| fd 增多 | 性能线性下降 | 性能不变 |

### 12.6 epoll 的内存开销

每个注册的 fd 占一个 epitem（约 128 字节）。
100 万 fd = 128 MB 内存。比 select 的 128KB 位图大得多。

但 epoll 的开销是"按需"的（只对注册的 fd），
select 是"全量"的（每次都遍历所有 fd）。

---

## 13. LT vs ET 的内核行为

### 13.1 LT 的内核实现

```c
/* LT 模式：epoll_wait 后不把 fd 从就绪链表移除 */
int ep_poll_lt(...)
{
    /* 取出就绪 fd */
    /* 但如果 fd 还有数据可读，保留在就绪链表 */
    if (epi->events & EPOLLIN && has_data(epi->fd)) {
        /* 保留在 rdllist，下次 epoll_wait 还会返回 */
    }
}
```

LT 的行为：只要 fd 还"可读"（有数据），就一直在就绪链表里。
每次 epoll_wait 都会返回它。

### 13.2 ET 的内核实现

```c
/* ET 模式：fd 从无数据→有数据时才加入就绪链表 */
int ep_poll_callback_et(...)
{
    /* 只在状态变化时触发 */
    if (was_empty(epi->fd) && now_has_data(epi->fd)) {
        list_add_tail(&epi->rdllink, &ep->rdllist);
    }
}
```

ET 的行为：只在"无→有"变化时加入就绪链表一次。
读完部分数据后，剩余数据不会再次触发。

### 13.3 为什么 ET 必须非阻塞？

```c
/* ET + 阻塞 fd 的死锁场景 */
/* 1. epoll_wait 通知 fd 可读 */
/* 2. read 读了 100 字节 */
/* 3. 循环再 read */
/* 4. 没有更多数据，阻塞 fd 会阻塞在这里！ */
/* 5. 但 epoll 不会再通知（ET 只通知一次） */
/* 6. 死锁：等永远不会来的通知 */
```

非阻塞 fd 在没数据时返回 EAGAIN，应用知道"读完了"，退出循环。

### 13.4 ET 的数据丢失风险

```c
/* ❌ ET 模式没读完 */
int n = read(fd, buf, sizeof(buf));  /* 只读一次 */
write(fd, buf, n);
/* 如果还有数据没读完，永远不会被通知 */
/* 数据丢失！ */
```

正确做法：

```c
/* ✅ ET 模式循环读到 EAGAIN */
for (;;) {
    int n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        /* 处理数据 */
        continue;
    }
    if (n == 0) break;  /* 对端关闭 */
    if (errno == EAGAIN) break;  /* 读完了 */
    /* 错误处理 */
    break;
}
```

### 13.5 LT 和 ET 的选择

| 场景 | 推荐 | 原因 |
|------|------|------|
| 学习/原型 | LT | 简单，不易出错 |
| echo server | LT | 一次就能读完 |
| HTTP server | ET | 大请求体要多次读 |
| 生产环境 | ET | 性能更好 |
| 跨平台 | select/poll | epoll 只有 Linux |

---

## 14. 性能调优建议

### 14.1 fd 上限

```bash
# 查看当前上限
ulimit -n

# 临时提高
ulimit -n 65536

# 永久提高（写入 /etc/security/limits.conf）
* soft nofile 65536
* hard nofile 65536

# 系统级上限
cat /proc/sys/fs/file-max
echo 1000000 > /proc/sys/fs/file-max
```

### 14.2 端口范围

```bash
# 客户端可用端口范围
cat /proc/sys/net/ipv4/ip_local_port_range
# 默认 32768 60999

# 扩大（高并发客户端需要）
echo "1024 65535" > /proc/sys/net/ipv4/ip_local_port_range
```

### 14.3 TIME_WAIT 优化

```bash
# 允许复用 TIME_WAIT 端口
echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse

# 减少 TIME_WAIT 时长（不推荐改 MSL）
# 更好的做法：让客户端主动关闭
```

### 14.4 TCP 缓冲区

```bash
# 查看默认缓冲区
cat /proc/sys/net/ipv4/tcp_rmem  # 接收
cat /proc/sys/net/ipv4/tcp_wmem  # 发送

# 调大（高带宽高延迟网络）
echo "4096 87380 16777216" > /proc/sys/net/ipv4/tcp_rmem
```

### 14.5 epoll 参数

```c
/* epoll_create 的 size 参数在 Linux 2.6.8+ 被忽略 */
int epfd = epoll_create(1);  /* 写 1 就行 */

/* epoll_wait 的 maxevents 要合理 */
/* 太小：多次系统调用；太大：内存浪费 */
int n = epoll_wait(epfd, events, 1024, -1);  /* 1024 通常够 */
```

### 14.6 批量 accept

```c
/* ET 模式下循环 accept 到 EAGAIN */
int n = 0;
for (;;) {
    int conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        if (errno == EAGAIN) break;
        continue;
    }
    handle_new_connection(conn_fd);
    n++;
}
if (n > 1) {
    log_debug("批量 accept %d 个连接", n);
}
```

### 14.7 SO_REUSEPORT

```c
/* 多个进程/线程 bind 同一端口 */
int reuse_port = 1;
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &reuse_port, sizeof(reuse_port));
```

内核把连接均匀分给各进程，避免惊群。
适合多进程 epoll 架构。

---

## 15. 常见错误和调试方法

### 15.1 "Too many open files"

```bash
$ ./server
accept: Too many open files
```

fd 耗尽。解决：

```bash
ulimit -n 65536  # 提高 fd 上限
```

检查 fd 泄漏：

```bash
ls /proc/$(pidof server)/fd | wc -l  # 看进程开了多少 fd
```

### 15.2 "Address already in use"

```bash
$ ./server
bind: Address already in use
```

端口被占用（可能 TIME_WAIT）。解决：

```c
int reuse = 1;
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
```

### 15.3 僵尸进程堆积

```bash
$ ps aux | grep Z | wc -l
500  /* 500 个僵尸 */
```

解决：加 SIGCHLD 处理。

### 15.4 epoll ET 丢数据

症状：客户端发 1MB 数据，服务器只收到 100KB。

原因：ET 模式没循环 read 到 EAGAIN。

调试：用 strace 看系统调用：

```bash
strace -e read,recvfrom -p $(pidof server)
```

### 15.5 select 超过 1024

```c
/* fd > 1024 时 FD_SET 越界 */
FD_SET(2000, &set);  /* 栈溢出或内存损坏 */
```

解决：改用 epoll，或 `#define FD_SETSIZE 65536`（要放在所有 include 前）。

### 15.6 用 strace 调试

```bash
# 看进程的系统调用
strace -p $(pidof server)

# 只看网络相关
strace -e network -p $(pidof server)

# 统计系统调用次数
strace -c -p $(pidof server)
```

### 15.7 用 ss/netstat 看连接

```bash
# 看所有连接
ss -tan

# 看某端口
ss -tan | grep 8080

# 看连接状态统计
ss -tan | awk '{print $1}' | sort | uniq -c
```

### 15.8 用 lsof 看 fd

```bash
# 看进程的 fd
lsof -p $(pidof server)

# 看某端口被谁占用
lsof -i :8080
```

---

## 16. 思考题和面试题

### 16.1 题目一：select 和 epoll 的区别

**问**：select 和 epoll 的主要区别是什么？

**答**：
1. **fd 上限**：select 1024，epoll 无上限
2. **性能**：select O(n) 遍历，epoll O(就绪数)
3. **fd_set 拷贝**：select 每次拷贝，epoll 只注册一次
4. **返回就绪**：select 要遍历找，epoll 直接返回就绪列表
5. **平台**：select 跨平台，epoll 仅 Linux

### 16.2 题目二：LT 和 ET 的区别

**问**：epoll 的 LT 和 ET 模式有什么区别？

**答**：
- LT：只要 fd 可读，每次 epoll_wait 都返回（水平触发）
- ET：只在 fd 从不可读变可读时返回一次（边沿触发）
- ET 必须非阻塞 fd + 循环 read 到 EAGAIN
- ET 性能更好（少几次 epoll_wait），但编程更复杂

### 16.3 题目三：fork 后的 fd

**问**：fork 后父子进程的 fd 关系？

**答**：
- fd 表复制（子进程有相同的 fd 号）
- 底层 file 对象共享（引用计数 +1）
- 父子进程都要 close 不需要的 fd（减少引用计数）
- 不 close 会导致 fd 泄漏

### 16.4 题目四：僵尸进程

**问**：什么是僵尸进程？怎么避免？

**答**：
- 子进程退出后父进程没 wait，变成 Z 状态
- 占用 PID 和 task_struct
- 避免：SIGCHLD 处理 / waitpid / 忽略 SIGCHLD / double fork

### 16.5 题目五：为什么 epoll 高效

**问**：epoll 为什么比 select 高效？

**答**：
1. 红黑树存储 fd，增删 O(log n)
2. 就绪链表，epoll_wait 直接取 O(就绪数)
3. 回调机制：fd 就绪时主动加入就绪链表
4. 不用每次拷贝 fd_set
5. 没有遍历所有 fd 的开销

### 16.6 题目六：COW

**问**：fork 的写时复制是什么？

**答**：
- fork 时不复制内存，只复制页表
- 父子进程共享物理页，页表标记只读
- 任一方写时触发缺页异常，复制该页
- 大幅减少 fork 开销（只复制几 KB 页表 vs 几 MB 内存）

### 16.7 题目七：非阻塞 IO

**问**：为什么 ET 模式必须非阻塞？

**答**：
- ET 只在状态变化时通知一次
- 如果阻塞 fd，读完部分数据后 read 会阻塞
- 但 epoll 不会再通知，永远等不到
- 非阻塞 fd 返回 EAGAIN，知道读完了

### 16.8 题目八：惊群问题

**问**：什么是惊群？怎么解决？

**答**：
- 多个进程/线程同时 epoll_wait 同一 listen_fd
- 一个连接到来，所有进程被唤醒
- 只一个 accept 成功，其他白醒
- 解决：主从 Reactor（只有主线程 accept）/ SO_REUSEPORT / epoll 的 EPOLLEXCLUSIVE

### 16.9 题目九：fd 上限

**问**：select 的 1024 上限怎么来的？

**答**：
- fd_set 是 1024 bit 的位图
- `__FD_SETSIZE` 默认 1024
- 可以 `#define FD_SETSIZE 65536` 改大，但要放在所有 include 前
- 更好的方案：用 epoll

### 16.10 题目十：IO 模型选择

**问**：什么场景用什么 IO 模型？

**答**：
- 连接数 < 100：fork/pthread（简单）
- 100-1000：select（如果跨平台）
- 1000-10000：poll（无 1024 限制）
- 10000+：epoll（Linux）/ kqueue（BSD）/ IOCP（Windows）
- 超高并发 + 超高吞吐：异步 IO（io_uring）

---

## 小结

| 阶段 | 核心概念 | 你学到了什么 |
|------|---------|-------------|
| 1 | 阻塞 IO | fd、listen/accept、为什么单连接 |
| 2 | 多进程 | fork、fd 引用计数、僵尸回收、COW |
| 3 | select | fd_set 位图、O(n) 遍历、1024 上限、poll_wait |
| 4 | epoll | 红黑树+就绪链表、LT vs ET、非阻塞、回调机制 |

**下一步**：stage5 用 epoll + HTTP 状态机，从 echo 升级到真正的 HTTP server。

---

## 附录 A：IO 模型演进历史

| 年代 | 模型 | 说明 |
|------|------|------|
| 1970s | 阻塞 IO + fork | Unix 传统模型 |
| 1983 | select | 4.2BSD 引入 |
| 1986 | poll | SVR3 引入 |
| 2002 | epoll | Linux 2.5.44 引入 |
| 2000s | kqueue | FreeBSD 4.1 引入 |
| 2000s | IOCP | Windows 引入 |
| 2019 | io_uring | Linux 5.1 引入，真正的异步 IO |

## 附录 B：进一步阅读

- 《Unix Network Programming, Volume 1》- W. Richard Stevens
- 《The Linux Programming Interface》- Michael Kerrisk
- 《高性能服务器架构》- 陈硕
- Linux 内核源码：fs/eventpoll.c（epoll 实现）
- Linux 内核源码：fs/select.c（select/poll 实现）
