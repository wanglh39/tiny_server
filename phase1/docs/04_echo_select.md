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

## 2. select 的内核实现详解

> 前面讲了 select 的用户态用法，这一节我们钻进内核，看看 `select()` 这个系统调用到底做了什么。
> 理解内核实现，你才能明白 select 的性能瓶颈从何而来，也才能理解 epoll 为什么快。

### 2.1 系统调用路径总览

当用户进程调用 `select()` 时，会经历如下调用栈（以 Linux 5.x 为例）：

```
用户态:  select()
   |  glibc 包装
   v
系统调用入口:  sys_select()          // 入口，处理参数
   v
核心逻辑:    core_sys_select()      // 拷贝 fd_set 到内核
   v
真正干活:    do_select()            // 轮询所有 fd，等待就绪
   v
驱动层:      f_op->poll()           // 每个 fd 的设备驱动 poll 方法
   v
等待队列:    poll_wait()            // 把当前进程加入等待队列
```

整个流程可以概括为三步：
1. **拷贝**：把用户空间的 `fd_set` 拷贝到内核栈（或内核堆）。
2. **轮询**：遍历所有 fd，调用每个 fd 对应驱动的 `poll` 方法，检查是否就绪。
3. **等待**：如果没有任何 fd 就绪，当前进程把自己挂到每个 fd 的等待队列上，然后睡眠。
   当某个 fd 有数据到达时，驱动会唤醒等待队列上的进程，`do_select` 再次轮询一遍，返回就绪的 fd 数量。

### 2.2 sys_select：入口与参数处理

```c
/* linux/fs/select.c（简化版，便于理解结构） */
SYSCALL_DEFINE5(select, int, n, fd_set __user *, inp,
                fd_set __user *, outp, fd_set __user *, exp,
                struct timeval __user *, tvp)
{
    struct timespec64 end_time, *to = NULL;
    if (tvp) {
        /* 把 timeval 转成内核的 timespec64，并计算绝对超时时刻 */
        to = &end_time;
        /* ... 时间换算 ... */
    }
    /* 转发到核心函数 */
    return core_sys_select(n, inp, outp, exp, to);
}
```

`sys_select` 主要做两件事：
- 如果用户传了 `timeout`，把它换算成内核内部的**绝对截止时刻**（`end_time`）。
  这样后续判断超时只需比较当前时间和 `end_time`。
- 转发到 `core_sys_select`。

### 2.3 core_sys_select：fd_set 的内核拷贝

```c
int core_sys_select(int n, fd_set __user *inp, fd_set __user *outp,
                    fd_set __user *exp, struct timespec64 *end_time)
{
    fd_set_bits fds;
    void *bits;
    /* 在栈上分配 6 个 fd_set 的空间（rin/win/ein × 2 份） */
    unsigned long stack_fds[256 / sizeof(unsigned long)];

    /* ... 分配 bits 缓冲区，栈不够就用 kmalloc 堆分配 ... */

    /* 把用户空间的 fd_set 拷贝到内核 */
    if (get_user_fd_set(inp, &fds.in, ...))  goto fail;
    if (get_user_fd_set(outp, &fds.out, ...)) goto fail;
    if (get_user_fd_set(exp, &fds.ex, ...))  goto fail;

    /* 真正的轮询 + 等待 */
    int ret = do_select(n, &fds, end_time);

    /* 把就绪结果拷贝回用户空间 */
    if (set_user_fd_set(inp, &fds.in, ...))  goto fail;
    /* ... */

    return ret;
}
```

关键点：
- **三次拷贝**：用户空间的 `readfds/writefds/exceptfds` 都要 `copy_from_user` 进内核。
  返回时还要 `copy_to_user` 拷贝回去。这是 select 的一个开销来源。
- **栈上分配优先**：内核优先在栈上分配 `fd_set` 缓冲区（快），如果位图太大（fd 很多）才退到 `kmalloc`。
  这就是为什么 `FD_SETSIZE` 默认 1024——位图 128 字节，6 份也就 768 字节，栈上放得下。

### 2.4 do_select：轮询与等待的核心

`do_select` 是 select 的心脏。简化后的逻辑：

```c
int do_select(int n, fd_set_bits *fds, struct timespec64 *end_time)
{
    for (;;) {
        /* 第一阶段：遍历所有 fd，检查就绪 */
        for (int i = 0; i < n; i++) {
            /* 检查第 i 位是否被关心（在 in/out/ex 中） */
            if (bit_not_set(fds, i)) continue;

            /* 拿到 fd 对应的 file 结构 */
            struct file *f = fget(i);
            /* 调用驱动提供的 poll 方法 */
            mask = f->f_op->poll(f, &pwq);

            if (mask & POLLIN)   set_bit(fds->res_in, i);
            if (mask & POLLOUT)  set_bit(fds->res_out, i);
            if (mask & POLLEX)   set_bit(fds->res_ex, i);
        }

        /* 如果有就绪的 fd，跳出循环返回 */
        if (any_ready(fds)) break;

        /* 如果超时了，跳出返回 0 */
        if (timed_out) break;

        /* 第二阶段：没有 fd 就绪，睡眠等待 */
        /* 把当前进程加入所有关心 fd 的等待队列 */
        for (int i = 0; i < n; i++) {
            poll_wait(f, &pwq);  /* 加入等待队列 */
        }

        /* 调度出去，让出 CPU */
        schedule();
        /* 被唤醒后回到循环开头，重新轮询一遍 */
    }
    return ready_count;
}
```

这就是 select 的 **O(n) 遍历** 的根源：
- 每次循环开头都要**遍历所有 n 个 fd**，调用驱动的 `poll` 方法。
- 即使只有 1 个 fd 就绪，也要把所有 fd 都问一遍。
- 10000 个客户端 → 每次循环 10000 次驱动调用。

### 2.5 poll_wait：把进程加入等待队列

每个 fd 背后都关联一个**等待队列**（`wait_queue_head_t`）。例如一个 socket 的接收缓冲区就有一个等待队列，存放"想读这个 socket 的进程"。

```c
void poll_wait(struct file *file, poll_table *pt)
{
    if (pt->_qproc) {
        /* 调用注册的回调，把当前进程加入 file 的等待队列 */
        pt->_qproc(file, &file->f_wait, pt);
    }
}
```

`do_select` 第一次循环时，`pt->_qproc` 是 `__pollwait`，它的作用是把当前进程**登记**到每个 fd 的等待队列上。

登记之后，`do_select` 调用 `schedule()` 让出 CPU，进程进入睡眠状态（`TASK_INTERRUPTIBLE`）。

### 2.6 就绪回调：fd 有数据时唤醒进程

当网卡收到数据，硬中断 → 软中断（NET_RX）→ 协议栈把数据放进 socket 的接收队列。这时协议栈会调用：

```c
wake_up_interruptible(&sock->wait);  /* 唤醒等待队列上的进程 */
```

被唤醒的进程从 `schedule()` 返回，回到 `do_select` 循环开头，**再次遍历所有 fd**，这次发现有 fd 就绪了，把就绪位写进结果位图，返回就绪数量。

注意：唤醒后**还要再遍历一遍所有 fd**。这就是为什么 select 在高并发下慢——每次有事件都要全量扫描。

### 2.7 select 的性能瓶颈总结

| 瓶颈                | 原因                                          | 影响                     |
|---------------------|-----------------------------------------------|--------------------------|
| O(n) 遍历           | `do_select` 每轮都要遍历所有 n 个 fd          | n=10000 时每次 10000 次调用 |
| 1024 fd 上限        | `FD_SETSIZE = 1024`，位图固定大小             | 无法支撑高并发           |
| 内存拷贝            | 用户↔内核 3 次拷贝 fd_set                    | 每次调用都有开销         |
| 重建 fd_set         | select 会清空未就绪的位，用户每次都要重置     | 用户态也要遍历           |
| 等待队列登记开销    | 每次都要把自己加入所有 fd 的等待队列          | fd 多时登记本身就很慢     |

这五条瓶颈，epoll 一个一个地解决：
- O(n) → O(1)（就绪列表）
- 1024 上限 → 无上限（红黑树管理 fd）
- 内存拷贝 → 一次拷贝（共享内存）
- 重建 → 不重建（epoll_ctl 增量维护）
- 等待队列登记 → 一次登记（fd 注册时登记一次）

这就是为什么 epoll 是高并发的终极方案。但 epoll 是后话，本篇先把 select 吃透。

---

## 3. fd_set 详解：位图的每一个细节

### 3.1 fd_set 的定义

在 `<sys/select.h>`（Linux 上实际是 `<bits/types/sigset_t.h>` 附近）能看到：

```c
/* fd_set 就是一个固定大小的 long 数组 */
typedef struct {
    unsigned long fds_bits[__FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_SETSIZE 1024
```

展开看：
- `sizeof(unsigned long)` 在 64 位系统上是 8 字节 = 64 bit。
- `__FD_SETSIZE / (8 * sizeof(long)) = 1024 / 64 = 16`。
- 所以 `fd_set` 是 `unsigned long fds_bits[16]`，共 16 × 8 = 128 字节 = 1024 bit。

```
fd_set 在 64 位系统上的内存布局：

  fds_bits[0]   fds_bits[1]   ...   fds_bits[15]
┌─────────────┬─────────────┬─...─┬─────────────┐
│  bit 0..63  │ bit 64..127 │ ... │ bit 960..1023│
└─────────────┴─────────────┴─...─┴─────────────┘
   fd=0..63      fd=64..127    ...   fd=960..1023
```

每一位对应一个 fd 编号。位 = 1 表示"我关心这个 fd"，位 = 0 表示"不关心"。

### 3.2 FD_SET / FD_CLR / FD_ISSET / FD_ZERO 的实现

这四个操作都是宏，展开后就是位运算：

```c
/* 把 fd 对应的位置 1（加入集合） */
#define FD_SET(fd, set) \
    do { \
        unsigned long __fd = (fd); \
        ((set)->fds_bits[__fd / (8 * sizeof(long))] \
         |= (1UL << (__fd % (8 * sizeof(long))))); \
    } while (0)

/* 把 fd 对应的位清 0（从集合移除） */
#define FD_CLR(fd, set) \
    do { \
        unsigned long __fd = (fd); \
        ((set)->fds_bits[__fd / (8 * sizeof(long))] \
         &= ~(1UL << (__fd % (8 * sizeof(long))))); \
    } while (0)

/* 检查 fd 对应的位是否为 1 */
#define FD_ISSET(fd, set) \
    ({ \
        unsigned long __fd = (fd); \
        (((set)->fds_bits[__fd / (8 * sizeof(long))] \
          >> (__fd % (8 * sizeof(long)))) & 1); \
    })

/* 清空整个集合（所有位置 0） */
#define FD_ZERO(set) \
    memset((set)->fds_bits, 0, sizeof(fd_set))
```

举例：`FD_SET(5, &set)` 在 64 位系统上：
- `5 / 64 = 0`，所以操作 `fds_bits[0]`。
- `5 % 64 = 5`，所以置第 5 位：`fds_bits[0] |= (1UL << 5)`。

```
fds_bits[0]:  0 0 1 0 0 0 0 0 ...   (bit 5 = 1, 表示关心 fd=5)
              bit7 ... bit5 ... bit0
```

### 3.3 为什么上限是 1024

`FD_SETSIZE = 1024` 是 POSIX 规定的最小值，glibc 默认就把它定义成 1024。

如果 fd ≥ 1024，`FD_SET(fd, &set)` 会访问 `fds_bits[fd/64]`，当 fd=1024 时就是 `fds_bits[16]`——越界！数组只有 16 个元素（下标 0..15）。

这就是 select 的硬上限：**fd 编号必须 < 1024**。

注意：是 **fd 编号** < 1024，不是"客户端数量" < 1024。fd 编号是进程级递增的，stdin=0、stdout=1、stderr=2、listen_fd=3、第一个 conn_fd=4……每打开一个文件/socket，fd 编号就 +1。即使你只保留 100 个客户端，如果中间频繁开关 fd，编号也可能涨到 1024 以上。

### 3.4 如何突破 1024 限制

**方法一：重定义 FD_SETSIZE（不推荐）**

```c
/* 必须在 include 任何系统头文件之前定义 */
#define FD_SETSIZE 65535
#include <sys/select.h>
```

这会让 `fd_set` 变成 8192 字节，能容纳 65535 个 fd。

**为什么不推荐**：
- 内核的 `sys_select` 也要支持这么大的位图。Linux 内核里 `sys_select` 用栈分配，栈上最多 `256*sizeof(long)=2KB`，超过就 `kmalloc`。但内核里仍有上限检查。
- 跨平台兼容性差：Windows 的 Winsock select 完全无视这个宏。
- 治标不治本：O(n) 遍历依然存在，n=65535 比n=1024 更慢。

**方法二：改用 poll**

poll 用 `pollfd` 数组代替位图，数组大小由用户决定，没有 1024 上限。这是 POSIX 标准的"突破 1024"方案。

**方法三：改用 epoll（推荐）**

epoll 不仅没有 1024 上限，而且 O(1) 就绪通知。Linux 高并发服务的标准选择。

### 3.5 fd_set 在 64 位 vs 32 位系统

| 系统      | sizeof(long) | fds_bits 长度 | sizeof(fd_set) | 容纳 fd 数 |
|-----------|--------------|---------------|----------------|------------|
| 32 位     | 4 字节       | 1024/32 = 32  | 128 字节       | 1024       |
| 64 位     | 8 字节       | 1024/64 = 16  | 128 字节       | 1024       |

有趣的是：无论 32 位还是 64 位，`sizeof(fd_set)` 都是 128 字节，因为 `FD_SETSIZE / (8 * sizeof(long))` 这个除法刚好抵消了 `sizeof(long)` 的变化。

但**位运算的步长不同**：32 位系统上 `fds_bits[fd/32]`，64 位上 `fds_bits[fd/64]`。所以 `fd_set` 的二进制表示在两种系统上**不兼容**，不能直接通过网络/文件传输 `fd_set`。

---

## 4. poll 详解和对比

### 4.1 poll 的 API

```c
#include <poll.h>

struct pollfd {
    int   fd;        /* 关心的 fd，-1 表示忽略此槽位 */
    short events;    /* 用户填：关心哪些事件 */
    short revents;   /* 内核填：实际发生了哪些事件 */
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout);
```

参数：
- `fds`：`pollfd` 数组。
- `nfds`：数组长度。
- `timeout`：超时毫秒。-1 = 永久阻塞，0 = 立即返回（非阻塞），>0 = 等待最多 timeout 毫秒。

返回值：就绪 fd 的数量（`revents != 0` 的个数），0 表示超时，-1 表示出错。

常用事件位：
- `POLLIN`：可读（普通数据可读）。
- `POLLOUT`：可写。
- `POLLERR`：出错（只在 revents，不能在 events 设置）。
- `POLLHUP`：挂起（对端关闭）。
- `POLLNVAL`：fd 无效（不是合法的打开文件）。

### 4.2 poll 的用法示例

```c
#define MAX_CLIENTS 4096
struct pollfd pfds[MAX_CLIENTS];

/* 初始化：把 listen_fd 放第一个 */
pfds[0].fd = listen_fd;
pfds[0].events = POLLIN;
int n_fds = 1;

for (;;) {
    int nready = poll(pfds, n_fds, -1);  /* 永久阻塞 */
    if (nready < 0) continue;

    /* listen_fd 就绪：新连接 */
    if (pfds[0].revents & POLLIN) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        pfds[n_fds].fd = conn_fd;
        pfds[n_fds].events = POLLIN;
        n_fds++;
        if (--nready == 0) continue;
    }

    /* 遍历所有客户端 */
    for (int i = 1; i < n_fds; i++) {
        if (pfds[i].revents & POLLIN) {
            ssize_t n = read(pfds[i].fd, buf, sizeof(buf));
            if (n <= 0) {
                close(pfds[i].fd);
                pfds[i].fd = -1;   /* 标记忽略 */
            } else {
                write(pfds[i].fd, buf, n);
            }
            if (--nready == 0) break;
        }
    }
}
```

### 4.3 poll vs select：逐项对比

| 对比项              | select                          | poll                          |
|---------------------|----------------------------------|-------------------------------|
| fd 集合表示         | 位图 `fd_set`（固定 1024 bit）   | `pollfd` 数组（动态大小）     |
| fd 上限             | 1024（FD_SETSIZE）               | 无上限（受限于内存）          |
| 每次调用前重置       | **要**（select 会清空未就绪位）  | **不要**（events 不被修改）   |
| 内核拷贝            | 3 个 fd_set 全拷贝              | 整个 pollfd 数组拷贝          |
| 返回后查找就绪 fd    | 遍历 0..nfds 用 FD_ISSET        | 遍历 0..nfds 检查 revents     |
| 事件类型粒度         | 读/写/异常 三类                  | POLLIN/POLLOUT/POLLERR 等更细 |
| 可移植性            | POSIX + Windows（Winsock）      | POSIX only（Windows 没有）    |
| 大量空闲 fd          | 仍要遍历所有 bit                | 可用 fd=-1 跳过槽位           |

poll 的两个关键改进：
1. **无 1024 上限**：数组大小由用户决定。
2. **不用每次重置**：`events` 字段内核不改，只改 `revents`。下次调用直接复用。

### 4.4 poll 的内核实现

poll 的内核路径和 select 几乎一样：

```
sys_poll → do_sys_poll → pollfd_to_fd_set（转成内部位图）→ do_select
```

是的，**Linux 内核里 poll 和 select 共用 `do_select` 的核心逻辑**！poll 在内核里先把 `pollfd` 数组转成内部的 fd_set 位图，然后调用同样的轮询+等待代码。

这意味着 poll **继承了 select 的所有性能瓶颈**：
- O(n) 遍历：依然每次都要扫所有 fd。
- 内存拷贝：`pollfd` 数组要 `copy_from_user`。
- 等待队列登记：每次都要把自己加入所有 fd 的等待队列。

poll 只是 API 更好用（无上限、不重置），**性能并没有本质提升**。

### 4.5 poll 的缺点

**缺点 1：仍是 O(n) 遍历**

10000 个客户端，1 个就绪，poll 返回后你仍要遍历 10000 个 `pollfd` 检查 `revents`。

**缺点 2：内核仍要拷贝整个数组**

即使大部分 fd 没事件，内核也要把整个 `pollfd` 数组拷贝进内核、拷贝出去。数组越大，拷贝越慢。

**缺点 3：等待队列重复登记**

每次 `poll()` 调用，内核都要把当前进程加入所有 fd 的等待队列。fd 多时这个登记本身就是 O(n) 开销，而且每次调用都要重做一遍（因为 poll 返回后登记就失效了）。

epoll 的关键改进就是：**登记只做一次**（`epoll_ctl ADD` 时登记），之后 `epoll_wait` 直接等就绪事件，不再重复登记。

### 4.6 poll 的一个隐藏优势：水平触发天然支持

poll 是**水平触发**（Level Triggered）：只要 fd 还可读，每次 poll 都会返回它就绪。这对初学者很友好——不会因为"忘了读完"而丢失事件。

epoll 默认也是水平触发，但可以切到边缘触发（Edge Triggered），边缘触发容易丢数据，要配合非阻塞 IO 小心处理。poll 没有这个选项，永远是水平触发，简单但慢。

---

## 5. select / poll / epoll 的适用场景

### 5.1 什么时候用 select

**场景 1：连接数少（< 200）且跨平台**

select 是唯一在 Windows 上也能用的 IO 多路复用（Winsock 提供）。如果你的程序要在 Windows + Linux 都跑，且连接数不多，select 是最便携的选择。

**场景 2：写教学示例**

select 的 API 最简单，概念最直观（位图、FD_SET/ISSET）。教学项目从 select 入门最合适——这正是本项目 stage3 的选择。

**场景 3：fd 编号确实都很小**

如果 fd 编号都 < 1024 且数量少，select 的位图很紧凑（128 字节），缓存友好，性能不差。

**不适用**：连接数 > 1000，或 fd 编号可能超过 1024。

### 5.2 什么时候用 poll

**场景 1：连接数 > 1024 但不多（几千）**

poll 没有 1024 上限，几千个连接能扛住。但再多（上万）就力不从心了。

**场景 2：只跑 Linux/UNIX，不想用 select**

poll 的 API 比 select 干净（不用每次重置、无 1024 上限），如果不需要 Windows 兼容，poll 比 select 好用。

**不适用**：高并发（> 10000），或需要 Windows 兼容。

### 5.3 什么时候用 epoll

**场景 1：高并发（> 10000 连接）**

epoll 是 Linux 高并发的标准方案。Nginx、Redis、Memcached 都用 epoll。1 万、10 万、甚至 100 万连接都能扛。

**场景 2：长连接多、活跃连接少**

epoll 的优势在"全量 fd 多但活跃 fd 少"时最明显——`epoll_wait` 只返回活跃的 fd，O(活跃数) 而非 O(全量数)。
长连接服务（如 IM、推送）典型场景：100 万连接，同时只有几百个在收发数据。select/poll 要扫 100 万次，epoll 只返回几百个。

**不适用**：
- 跨平台（epoll 是 Linux 专属，BSD 用 kqueue，Windows 用 IOCP）。
- 连接数很少且每次都活跃——epoll 的 `epoll_ctl` 维护开销可能反而比 select 大。

### 5.4 实际项目中的选择

```
连接数        平台           推荐
< 200         跨平台         select
< 5000        Linux only     poll
> 5000        Linux          epoll
> 5000        BSD            kqueue
> 5000        Windows        IOCP
任何数        跨平台         libevent / libuv（它们自动选底层）
```

**生产项目的常见做法**：不直接用 select/poll/epoll，而是用 **libevent** 或 **libuv** 这类封装库。它们在运行时自动选择当前平台最快的机制（Linux 上选 epoll，BSD 上选 kqueue，Windows 上选 IOCP），并提供统一的 API。

本项目 stage3 用 select，是为了**教学**——理解最原始的 IO 多路复用，才能理解后面 epoll 为什么好。

---

## 6. 代码逐行讲解和调试

### 6.1 select 主循环逐行讲解

回到 4.4 节的完整代码，重点看主循环：

```c
for (;;) {
    fd_set rset = all_set;  /* ← 第116行：每次拷贝一份 */
```

**为什么每次都要拷贝？** 因为 `select` 会修改 `rset`：调用前 `rset` 表示"我关心哪些 fd"，调用后 `rset` 表示"哪些 fd 就绪了"。不拷贝的话，下一轮就不知道该关心谁了。

`all_set` 是"长期记忆"——记录所有要监控的 fd。`rset` 是"临时副本"——每次 select 用完就扔。

```c
    int nready = select(max_fd + 1, &rset, NULL, NULL, NULL);
```

- `max_fd + 1`：告诉内核"只扫描 0..max_fd 这些位"。传 max_fd+1 是因为参数含义是"fd 数量"而非"最大 fd"。如果 max_fd=5，内核扫描 bit 0,1,2,3,4,5 共 6 个。
- `&rset`：关心可读的 fd 集合。
- 后两个 NULL：不关心可写、不关心异常。
- 最后 NULL：永久阻塞，直到有 fd 就绪。

```c
    if (nready < 0) continue;
```

`select` 返回 -1 通常是被信号中断（`EINTR`）。这里简单 `continue` 重试。生产代码应该检查 `errno == EINTR` 才重试，其他错误要处理。

```c
    if (FD_ISSET(listen_fd, &rset)) {
        int conn_fd = accept(listen_fd, NULL, NULL);
```

`listen_fd` 可读 = 有新连接到达。`accept` 取出连接，得到 `conn_fd`。

```c
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] < 0) {
                clients[i] = conn_fd;
                break;
            }
        }
```

在 `clients` 数组里找一个空槽位（值为 -1）存放新 conn_fd。这是线性查找，O(n)。生产代码可以用更高效的数据结构（如空闲链表）。

```c
        FD_SET(conn_fd, &all_set);      /* 加入长期监控集 */
        if (conn_fd > max_fd) max_fd = conn_fd;  /* 更新 max_fd */
```

把新 conn_fd 加入 `all_set`，并更新 `max_fd`（select 第一个参数要用）。

```c
        if (--nready == 0) continue;  /* 没有更多就绪的 */
    }
```

`nready` 是 select 返回的就绪 fd 总数。处理完 listen_fd 后 `--nready`，如果归零说明只有 listen_fd 就绪，跳过客户端检查。这是个**优化**——避免无谓的遍历。

```c
    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = clients[i];
        if (fd < 0) continue;
        if (FD_ISSET(fd, &rset)) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) {
                close(fd);
                FD_CLR(fd, &all_set);   /* 从监控集移除 */
                clients[i] = -1;
            } else {
                write(fd, buf, n);      /* echo 回去 */
            }
            if (--nready == 0) break;
        }
    }
```

遍历所有客户端，检查每个是否就绪（`FD_ISSET`）。`read` 返回 0 表示对端关闭，返回 -1 表示出错，都要清理。`n > 0` 就把读到的数据原样写回（echo）。

`if (--nready == 0) break;` 同样是优化——处理完所有就绪 fd 就提前退出，不浪费后续遍历。

### 6.2 select 的 timeout 参数详解

```c
struct timeval {
    long tv_sec;   /* 秒 */
    long tv_usec;  /* 微秒 */
};

int select(int nfds, fd_set *r, fd_set *w, fd_set *e,
           struct timeval *timeout);
```

`timeout` 的三种用法：

**用法 1：NULL —— 永久阻塞**

```c
select(max_fd+1, &rset, NULL, NULL, NULL);
/* 一直等，直到有 fd 就绪 */
```

**用法 2：非零超时 —— 等一段时间**

```c
struct timeval tv = {5, 0};  /* 5 秒 */
select(max_fd+1, &rset, NULL, NULL, &tv);
/* 最多等 5 秒，5 秒内没 fd 就绪就返回 0 */
```

**用法 3：零超时 —— 非阻塞轮询**

```c
struct timeval tv = {0, 0};
select(max_fd+1, &rset, NULL, NULL, &tv);
/* 立即返回，检查当前有没有 fd 就绪 */
```

**重要陷阱：timeout 会被内核修改！**

select 返回后，`timeout` 会被改成"剩余的等待时间"（部分实现）。所以**每次调用前都要重新设置 timeout**：

```c
for (;;) {
    struct timeval tv = {5, 0};  /* 每次都要重新设！ */
    int n = select(max_fd+1, &rset, NULL, NULL, &tv);
    /* ... */
}
```

如果不重设，第二次调用时 `tv` 可能已经是 0，select 立即返回，CPU 打满。

### 6.3 用 strace 观察 select 的行为

编译运行服务器：

```bash
gcc echo_select.c -o echo_select
./echo_select 8080
```

另开终端用 strace 跟踪：

```bash
strace -p $(pgrep echo_select) -e trace=select,accept4,read,write
```

你会看到类似输出：

```
select(6, [3 4 5], NULL, NULL, NULL) = 1 (in [4])
read(4, "hello\n", 4096)               = 6
write(4, "hello\n", 6)                 = 6
select(6, [3 4 5], NULL, NULL, NULL) = 1 (in [3])
accept4(3, ..., SOCK_NONBLOCK)        = 6
select(7, [3 4 5 6], NULL, NULL, NULL) = 1 (in [5])
read(5, "world\n", 4096)               = 6
write(5, "world\n", 6)                 = 6
```

解读：
- `select(6, [3 4 5], ...)`：第一个参数 6 = max_fd+1，第二个参数 `[3 4 5]` 是关心的 fd 列表。
- `= 1 (in [4])`：返回 1 个就绪 fd，是 fd 4。
- 然后 `read(4, ...)` 读取 fd 4 的数据。
- 下一次 `select` 的 fd 列表变成 `[3 4 5 6]`——新连接 fd 6 被加入了。

strace 是理解 IO 多路复用最直观的工具，强烈建议自己跑一遍。

### 6.4 常见错误和解决方法

**错误 1：忘记拷贝 fd_set**

```c
/* 错误写法 */
fd_set rset;
FD_ZERO(&rset);
FD_SET(listen_fd, &rset);
for (;;) {
    select(max_fd+1, &rset, ...);  /* rset 被改了！ */
    /* 下一轮 rset 只剩就绪的 fd，其他全丢了 */
}
```

**解决**：每轮拷贝一份，或用 `all_set` + `rset = all_set` 的模式。

**错误 2：忘记更新 max_fd**

```c
int conn_fd = accept(...);
FD_SET(conn_fd, &all_set);
/* 忘了 if (conn_fd > max_fd) max_fd = conn_fd; */
```

后果：select 第一个参数太小，新 conn_fd 超出扫描范围，永远检测不到它的就绪。客户端发数据服务器没反应。

**解决**：每次新增 fd 都要更新 max_fd；每次关闭 fd 后，如果关的是 max_fd，要重新扫描找新的 max_fd（本项目代码省略了这一步，简化处理）。

**错误 3：FD_SET 越界（fd ≥ 1024）**

```c
int conn_fd = accept(...);  /* conn_fd 可能 = 1024 */
FD_SET(conn_fd, &all_set);  /* 越界写内存！ */
```

后果：内存损坏，可能段错误，可能数据错乱，难调试。

**解决**：检查 `conn_fd < FD_SETSIZE`，超了就拒绝或改用 poll/epoll。

**错误 4：read 没处理 EINTR**

```c
ssize_t n = read(fd, buf, sizeof(buf));
if (n <= 0) { /* 关闭 */ }
```

`read` 被信号中断时返回 -1 且 `errno == EINTR`，这不是真错误，应该重试。上面代码把 EINTR 当成关闭，会误断连接。

**解决**：

```c
ssize_t n = read(fd, buf, sizeof(buf));
if (n < 0) {
    if (errno == EINTR) continue;  /* 被信号中断，重试 */
    /* 其他错误才关闭 */
    close(fd); ...
} else if (n == 0) {
    close(fd); ...  /* 对端关闭 */
} else {
    write(fd, buf, n);
}
```

**错误 5：select 返回 -1 没区分 errno**

```c
if (nready < 0) continue;  /* 简单重试 */
```

`select` 返回 -1 可能是 `EINTR`（重试）、`EBADF`（有 fd 无效，要清理）、`EINVAL`（参数错，要修代码）、`ENOMEM`（内存不够）。无脑 continue 可能导致死循环。

**解决**：至少区分 `EINTR`：

```c
if (nready < 0) {
    if (errno == EINTR) continue;
    perror("select");
    exit(1);
}
```

---

## 7. select 的历史和影响

### 7.1 select 的 UNIX 起源

select 系统调用最早出现在 **4.2BSD**（1983年），用于在 BSD socket API 上同时监控多个连接。当时的设计目标是在单进程内服务多个网络连接，避免为每个连接 fork 一个进程（fork 在 80 年代很贵）。

POSIX.1-2001 把 select 标准化，`FD_SETSIZE = 1024` 也是那时定的。80 年代 1024 个连接已经是"天文数字"，没人想到要突破。但 30 年后互联网爆发，1024 成了致命瓶颈。

select 的设计反映了 80 年代的硬件现实：
- 位图紧凑（128 字节），缓存友好。
- fd 编号小（一个进程开不了几个文件）。
- 连接数少（一台服务器几十个连接就算多了）。

### 7.2 为什么 Windows 也有 select

Windows 的网络栈叫 **Winsock**（Windows Sockets API），设计于 1993 年，目标是**源码级兼容 BSD socket**。为了让 BSD 网络程序能直接在 Windows 编译，Winsock 实现了 select。

但 Windows 的 select 实现和 UNIX 完全不同：
- Windows 的 fd_set 不是位图，而是**fd 数组**（`fd_count` + `fd_array[]`）。
- `FD_SETSIZE` 在 Windows 默认是 64（不是 1024！）。
- 内部用事件对象模拟，性能比 UNIX select 还差。

Windows 上真正的高并发方案是 **IOCP**（IO Completion Port），异步 IO 模型，和 select/epoll 思路完全不同。libuv（Node.js 底层）就是基于 IOCP。

所以"select 跨平台"只是 API 层面——同一份代码能编译，但性能和上限在 Windows 上更差。真正的跨平台高性能 IO 多路复用要用 libevent/libuv。

### 7.3 libevent / libuv 对 select 的封装

**libevent**（2002 年 Niels Provos 创建）的思路：
- 提供统一 API：`event_add` 注册 fd + 事件 + 回调。
- 运行时检测平台，自动选最快后端：Linux 用 epoll，BSD 用 kqueue，Solaris 用 evport，都没有才退到 poll/select。
- 老版本 libevent 默认后端就是 select，后来 epoll 成熟后改成 epoll 优先。

**libuv**（Node.js 作者 Ryan Dahl 创建）的思路：
- 异步 IO，回调驱动，不暴露 select/poll/epoll 概念。
- Linux 用 epoll，Windows 用 IOCP，BSD 用 kqueue。
- 比 libevent 更"异步原生"，没有同步 IO 多路复用的影子。

这两个库的存在说明一件事：**select/poll/epoll/kqueue/IOCP 的差异是平台细节，不该暴露给应用代码**。教学项目从 select 学起是为了理解原理，生产项目应该用封装库。

### 7.4 select 的教学价值

尽管生产环境几乎没人直接用 select，它仍是 IO 多路复用的**最佳教学起点**：
- API 最简单：一个位图、四个宏、一个函数。
- 概念最直观：位图 = 关心的 fd 集合，FD_ISSET = 检查就绪。
- 缺点最明显：1024 上限、O(n) 遍历、内存拷贝——理解了这些缺点，epoll 的改进才有意义。

本项目 stage3 用 select，stage4 会进化到 epoll。读完这两阶段，你就理解了从"原始多路复用"到"现代高并发"的完整演进。

---

## 8. 小结

| 知识点              | 关键记忆                                                       |
|---------------------|----------------------------------------------------------------|
| fd_set 本质         | 1024 bit 的位图，128 字节，每位对应一个 fd                     |
| FD_SET/FD_ISSET     | 位运算：`fds_bits[fd/64]` 的第 `fd%64` 位                       |
| select 三大缺点     | 1024 上限、每次重置 fd_set、O(n) 遍历                          |
| select 内核路径     | sys_select → core_sys_select → do_select（轮询+等待）          |
| do_select 的循环    | 遍历所有 fd 检查就绪 → 没有就睡眠 → 被唤醒再遍历一遍           |
| poll 的改进         | 无 1024 上限、不用重置、事件粒度更细                           |
| poll 的内核         | 和 select 共用 do_select，性能瓶颈相同                          |
| epoll 的改进        | O(1) 就绪通知、红黑树管理 fd、一次登记                          |
| 适用场景            | <200 跨平台用 select，<5000 Linux 用 poll，>5000 Linux 用 epoll |
| 调试工具            | strace -e trace=select 观察 select 调用                        |
| 生产实践            | 用 libevent/libuv 封装，不要直接用 select/poll/epoll           |

下一篇 stage4 我们进入 epoll，看它如何一个一个解决 select 的瓶颈。

