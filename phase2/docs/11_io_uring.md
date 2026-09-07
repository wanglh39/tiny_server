# stage9 - io_uring 异步 IO

## 本章导读

在 phase1 的 stage4，我们用了 epoll——它是 Linux 网络编程的标准武器。
但 epoll 有一个根本性的问题：**它只告诉你"可以读了"，你还得自己调 `read()`**。

```
epoll 模式:
  epoll_wait() → "fd 可读了" → read(fd, ...) → "读到 100 字节" → 处理
                                ↑ 这还是一次系统调用！
```

**io_uring**（Linux 5.1+）彻底改变了这个范式：

```
io_uring 模式:
  提交读请求 → 去干别的 → 内核读完了通知你 → 直接处理结果
  ↑ 提交和完成都通过共享内存，无系统调用！
```

这是真正的**异步 IO**——提交请求后立刻返回，内核在后台执行，完成后通知你。

```bash
# 编译
cmake --build build --target uring_echo_server

# 运行
build/bin/uring_echo_server 8080

# 测试
echo "hello io_uring" | nc localhost 8080
```

---

## 一、为什么需要 io_uring

### 1.1 Linux IO 模型演进

```
1. 阻塞 IO        read() 阻塞直到有数据         ← stage1
2. 非阻塞 IO      read() 立刻返回 EAGAIN        ← 轮询，浪费 CPU
3. IO 多路复用    epoll_wait + read             ← stage4，仍是同步读写
4. 信号驱动 IO    SIGIO 通知                    ← 很少用，复杂
5. 异步 IO        提交请求 → 完成通知           ← io_uring！
```

前四种模型，**read/write 本身都是同步的**——数据拷贝时 CPU 必须参与。
只有第五种（io_uring）是真正的异步——数据拷贝由内核完成，CPU 去干别的。

### 1.2 epoll 的局限性

```c
// epoll 的工作方式
while (1) {
    int n = epoll_wait(epfd, events, MAX, -1);  // 等就绪
    for (int i = 0; i < n; i++) {
        int r = read(events[i].data.fd, buf, sz); // ← 系统调用！
        // 处理 buf
        int w = write(events[i].data.fd, buf, r); // ← 又一次系统调用！
    }
}
```

每次 read/write 都是一次系统调用（用户态 → 内核态切换）。
高并发下，几万个连接同时读写，系统调用开销不可忽视。

### 1.3 io_uring 的革命

```c
// io_uring 的工作方式
while (1) {
    // 提交请求（通过共享内存，无系统调用）
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, fd, buf, sz, 0);
    io_uring_submit(&ring);  // 一次提交可以攒多个请求

    // 等完成
    io_uring_wait_cqe(&ring, &cqe);
    // cqe->res 就是读到的字节数，数据已经在 buf 里了
    // 不需要再调 read()！
}
```

**关键区别**：
- epoll：`epoll_wait` 返回 → 你调 `read` → 内核拷贝数据 → 返回
- io_uring：你提交读请求 → 内核拷贝数据 → `io_uring_wait_cqe` 返回结果

数据拷贝发生在你等待期间，不需要你参与。

---

## 二、io_uring 核心概念

### 2.1 双环形队列

io_uring 的核心是两个**共享内存环形队列**：

```
┌─────────────────────────────────────────┐
│              共享内存区域                 │
│                                         │
│  ┌─────────┐         ┌─────────┐       │
│  │    SQ    │         │    CQ    │       │
│  │ (提交队列) │         │ (完成队列) │       │
│  │         │         │         │       │
│  │ SQE[0]  │         │ CQE[0]  │       │
│  │ SQE[1]  │         │ CQE[1]  │       │
│  │ SQE[2]  │         │ CQE[2]  │       │
│  │  ...    │         │  ...    │       │
│  │ SQE[N-1]│         │ CQE[N-1]│       │
│  └─────────┘         └─────────┘       │
│                                         │
│  用户写 SQ，内核读 SQ                    │
│  内核写 CQ，用户读 CQ                    │
└─────────────────────────────────────────┘
```

- **SQ（Submission Queue）**：用户往里放 IO 请求（SQE），内核取出来执行
- **CQ（Completion Queue）**：内核往里放完成结果（CQE），用户取出来处理

两个队列都是**环形缓冲区**（ring buffer），通过 mmap 和内核共享内存。
用户写 SQ 和读 CQ **不需要系统调用**——直接读写共享内存。

### 2.2 SQE 和 CQE

**SQE（Submission Queue Entry）**——一个 IO 请求：

```c
struct io_uring_sqe {
    __u8  opcode;       // 操作类型: ACCEPT/READ/WRITE/...
    __u8  flags;        // 标志位
    __s32 fd;           // 文件描述符
    union {
        __u64 off;      // 偏移量（文件 IO 用）
        __u64 addr2;    // 地址2
    };
    union {
        __u64 addr;     // 缓冲区地址
        __u64 splice_off_in;
    };
    __u32 len;          // 长度
    __u64 user_data;    // 用户数据（完成时原样返回）
    // ... 其他字段
};
```

**CQE（Completion Queue Entry）**——一个完成结果：

```c
struct io_uring_cqe {
    __u64 user_data;    // 提交时设置的数据（原样返回）
    __s32 res;          // 结果（>= 0 成功，< 0 错误码）
    __u32 flags;        // 标志位
};
```

`user_data` 是连接提交和完成的桥梁——提交时设置，完成时原样返回。
你可以用它存指针、fd、操作类型等任何 8 字节数据。

### 2.3 支持的操作类型

```c
IORING_OP_NOP          // 空操作（测试用）
IORING_OP_READ         // 读（等价于 read）
IORING_OP_WRITE        // 写（等价于 write）
IORING_OP_READV        // scatter 读（等价于 readv）
IORING_OP_WRITEV       // gather 写（等价于 writev）
IORING_OP_SENDMSG      // 发送消息（等价于 sendmsg）
IORING_OP_RECVMSG      // 接收消息（等价于 recvmsg）
IORING_OP_ACCEPT       // 接受连接（等价于 accept）
IORING_OP_CONNECT      // 发起连接（等价于 connect）
IORING_OP_SEND         // 发送（等价于 send）
IORING_OP_RECV         // 接收（等价于 recv）
IORING_OP_TIMEOUT      // 超时
IORING_OP_LINK_TIMEOUT // 链式超时
IORING_OP_CANCEL       // 取消请求
IORING_OP_SPLICE       // 零拷贝管道传输
IORING_OP_TEE          // 零拷贝管道复制
IORING_OP_CLOSE        // 关闭 fd
IORING_OP_STATX        // 获取文件信息（等价于 statx）
IORING_OP_OPENAT       // 打开文件（等价于 openat）
// ... 还有很多
```

几乎所有系统调用都有对应的 io_uring 操作！

### 2.4 提交流程

```
用户空间                          内核空间
  │                                 │
  │  1. io_uring_get_sqe()          │
  │     从 SQ 取一个空闲 SQE        │
  │     （读写共享内存，无系统调用）  │
  │                                 │
  │  2. io_uring_prep_xxx()         │
  │     填写 SQE 的字段             │
  │     （opcode, fd, buf, len...） │
  │                                 │
  │  3. io_uring_submit()           │
  │     通知内核 SQ 有新请求 ─────→ │  从 SQ 取 SQE
  │     （一次系统调用，可批量）      │  执行 IO 操作
  │                                 │
  │                                 │  完成后写 CQE 到 CQ
  │                                 │
  │  4. io_uring_wait_cqe()         │
  │     等 CQ 有完成事件 ←────────── │
  │     （一次系统调用）             │
  │                                 │
  │  5. 处理 cqe->res              │
  │     io_uring_cq_advance()       │
  │     消费 CQE                    │
  │                                 │
```

**关键优化**：步骤 1-2 不需要系统调用！可以攒多个 SQE，步骤 3 一次提交。

---

## 三、liburing 库

io_uring 的原始接口很复杂（mmap、ring buffer 操作、内存屏障等）。
**liburing** 是官方提供的 C 封装库，大幅简化使用。

### 3.1 安装

```bash
# Ubuntu/Debian
sudo apt install liburing-dev

# CMake 链接
target_link_libraries(your_target uring)
```

### 3.2 核心 API

```c
/* 初始化 */
struct io_uring ring;
io_uring_queue_init(ENTRIES, &ring, 0);  // ENTRIES = 队列大小

/* 提交请求 */
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);  // 取空 SQE
io_uring_prep_read(sqe, fd, buf, len, offset);        // 填写
io_uring_sqe_set_data(sqe, user_data);                // 设置用户数据
io_uring_submit(&ring);                               // 提交

/* 等完成 */
struct io_uring_cqe *cqe;
io_uring_wait_cqe(&ring, &cqe);                       // 等一个完成
int result = cqe->res;                                // 结果
void *data = io_uring_cqe_get_data(cqe);              // 用户数据
io_uring_cqe_seen(&ring, cqe);                        // 标记已消费

/* 清理 */
io_uring_queue_exit(&ring);
```

### 3.3 prep 函数族

```c
io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
io_uring_prep_read(sqe, fd, buf, len, offset);
io_uring_prep_write(sqe, fd, buf, len, offset);
io_uring_prep_readv(sqe, fd, iovec, nr_vecs, offset);
io_uring_prep_writev(sqe, fd, iovec, nr_vecs, offset);
io_uring_prep_send(sqe, fd, buf, len, flags);
io_uring_prep_recv(sqe, fd, buf, len, flags);
io_uring_prep_connect(sqe, fd, addr, addrlen);
io_uring_prep_close(sqe, fd);
io_uring_prep_timeout(sqe, ts, count, flags);
io_uring_prep_splice(sqe, fd_in, off_in, fd_out, off_out, len, flags);
```

每个 prep 函数只是填写 SQE 的字段，不发起系统调用。

---

## 四、代码解读

### 4.1 整体流程

```
main()
  │
  ├── io_uring_queue_init()     初始化 io_uring
  ├── 创建 listen socket
  ├── submit_accept()           提交第一个 accept 请求
  │
  └── for (;;) {
        io_uring_wait_cqe()     等完成
        handle_completion()     处理完成事件
        io_uring_cq_advance()   消费 CQE
      }
```

### 4.2 user_data 编码

```c
enum { OP_ACCEPT = 0, OP_READ = 1, OP_WRITE = 2 };

static unsigned long encode_ud(int op, int fd)
{
    return ((unsigned long)op << 32) | (unsigned long)(unsigned int)fd;
}
```

CQE 完成时，我们需要知道：
1. 这是什么操作的完成？（accept/read/write）
2. 是哪个 fd 的？

用 user_data 的高 32 位存操作类型，低 32 位存 fd。
一次编码，完成时一次解码。

### 4.3 提交 accept

```c
static void submit_accept(int listen_fd)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, (void *)encode_ud(OP_ACCEPT, listen_fd));
    io_uring_submit(&ring);
}
```

和 `accept()` 的区别：
- `accept()` 阻塞直到有连接
- `io_uring_prep_accept` 提交请求后立刻返回，内核在后台等连接

### 4.4 处理完成

```c
static void handle_completion(struct io_uring_cqe *cqe, int listen_fd)
{
    unsigned long ud = (unsigned long)io_uring_cqe_get_data(cqe);
    int op = decode_op(ud);  // 操作类型
    int fd = decode_fd(ud);  // fd
    int res = cqe->res;      // 结果

    switch (op) {
    case OP_ACCEPT:
        // res = 新连接 fd
        submit_read(res);       // 提交 read 请求
        submit_accept(listen_fd); // 继续等下一个连接
        break;
    case OP_READ:
        // res = 读到的字节数
        if (res > 0) submit_write(fd, res);  // echo: 提交 write
        else close_connection(fd);            // 0 或错误: 关闭
        break;
    case OP_WRITE:
        // res = 写的字节数
        if (res > 0) submit_read(fd);  // 继续读下一批
        else close_connection(fd);
        break;
    }
}
```

整个流程是**事件驱动的**：
```
accept 完成 → 提交 read
read 完成  → 提交 write
write 完成 → 提交 read
...
```

没有一行 `read()` 或 `write()` 系统调用！

### 4.5 批量处理 CQE

```c
unsigned head;
unsigned count = 0;

io_uring_for_each_cqe(&ring, head, cqe) {
    handle_completion(cqe, listen_fd);
    count++;
}
io_uring_cq_advance(&ring, count);
```

`io_uring_for_each_cqe` 遍历所有未消费的 CQE。
一次 `io_uring_wait_cqe` 可能唤醒多个完成事件（多个 IO 同时完成）。
批量处理减少循环次数。

---

## 五、io_uring vs epoll 对比

### 5.1 编程模型对比

```c
/* epoll: 就绪通知 + 同步读写 */
while (1) {
    int n = epoll_wait(epfd, events, MAX, -1);
    for (i = 0; i < n; i++) {
        int r = read(events[i].data.fd, buf, sz);  // 系统调用
        write(events[i].data.fd, buf, r);           // 系统调用
    }
}

/* io_uring: 提交 + 完成通知 */
while (1) {
    // 提交请求（无系统调用）
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, fd, buf, sz, 0);
    io_uring_submit(&ring);

    // 等完成
    io_uring_wait_cqe(&ring, &cqe);
    // cqe->res 就是结果，buf 里已经有数据了
}
```

### 5.2 系统调用次数对比

假设 100 个连接各发一个请求：

| 操作 | epoll | io_uring |
|------|-------|----------|
| 等待事件 | 1 (epoll_wait) | 1 (io_uring_wait_cqe) |
| 读 100 个连接 | 100 (read) | 0（提交时已包含） |
| 写 100 个连接 | 100 (write) | 0（提交时已包含） |
| **总计** | **201** | **1 + 提交次数** |

io_uring 可以攒 100 个 SQE 一次 `io_uring_submit`，只需 2 次系统调用！

### 5.3 数据拷贝对比

```
epoll:
  epoll_wait 返回 → read() → 内核拷贝数据到用户 buf → 返回
  CPU 必须等 read 完成

io_uring:
  提交 read 请求 → CPU 去干别的 → 内核拷贝数据 → 通知完成
  CPU 不用等，数据拷贝和 CPU 计算并行
```

### 5.4 功能对比

| 特性 | epoll | io_uring |
|------|-------|----------|
| 文件 IO | 不支持（read 阻塞） | 支持（异步读文件） |
| 网络 IO | 支持 | 支持 |
| 零拷贝 | sendfile/splice | splice/tee（原生支持） |
| 批量提交 | 不适用 | 支持（攒多个 SQE） |
| 超时管理 | timerfd | IORING_OP_TIMEOUT |
| 取消请求 | 不支持 | IORING_OP_CANCEL |
| 内核轮询 | 不适用 | IORING_SETUP_SQPOLL |

---

## 六、SQPOLL 模式（内核轮询）

### 6.1 默认模式 vs SQPOLL

**默认模式**：每次 `io_uring_submit` 都是一次系统调用。

**SQPOLL 模式**：内核开一个专用线程轮询 SQ，用户只需要写 SQE 到共享内存，**连 submit 都不需要**！

```c
/* SQPOLL 模式初始化 */
io_uring_queue_init(ENTRIES, &ring, IORING_SETUP_SQPOLL);

/* 提交请求 —— 不需要 io_uring_submit！ */
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, fd, buf, len, 0);
// 内核线程会自动发现 SQ 中的新请求
```

**零系统调用提交 IO 请求！**

### 6.2 SQPOLL 的代价

- 需要一个内核线程持续轮询（消耗 CPU）
- 适合高吞吐场景（IO 密集型）
- 需要 root 或 CAP_SYS_NICE 权限
- 空闲时内核线程会休眠（可配置 `sq_thread_idle`）

---

## 七、链式请求（IOSQE_IO_LINK）

io_uring 支持**链式请求**——前一个完成才提交下一个：

```c
/* 链式：read → write，read 完成后自动提交 write */
sqe1 = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe1, fd, buf, len, 0);
sqe1->flags |= IOSQE_IO_LINK;  // 标记为链式

sqe2 = io_uring_get_sqe(&ring);
io_uring_prep_write(sqe2, fd, buf, len, 0);
// sqe2 不需要 link，它是链的最后一个

io_uring_submit(&ring);
// 内核自动保证：read 完成后才提交 write
```

链式请求减少了用户态的等待和调度开销。
比如 read → process → write 可以用链式请求 + 固定缓冲区实现。

---

## 八、固定缓冲区（registered buffers）

### 8.1 为什么要固定缓冲区

普通 io_uring：每次提交 read/write 时，内核需要把用户缓冲区映射到内核地址空间。
如果缓冲区地址不变，这个映射是重复的浪费。

### 8.2 注册固定缓冲区

```c
/* 注册一组缓冲区，内核预映射 */
struct iovec iov = { .iov_base = buf, .iov_len = BUF_SIZE };
io_uring_register_buffers(&ring, &iov, 1);

/* 使用固定缓冲区读写 */
sqe = io_uring_get_sqe(&ring);
io_uring_prep_read_fixed(sqe, fd, buf, BUF_SIZE, 0, /*buf_index=*/0);
// 内核直接用预映射的地址，省去映射开销
```

适合高性能场景（数据库、存储引擎）。

---

## 九、非阻塞 vs 异步

### 9.1 概念区分

```
非阻塞 (non-blocking):
  read() 立刻返回
  如果没数据，返回 EAGAIN
  你需要轮询或用 epoll 等就绪
  → "你不阻塞了，但你得自己查"

异步 (asynchronous):
  提交请求后立刻返回
  内核在后台执行
  完成后通知你
  → "你提交就不管了，好了叫你"
```

epoll + 非阻塞 = 非阻塞 IO（不是异步）
io_uring = 异步 IO

### 9.2 为什么 epoll 不是异步

```c
// epoll + 非阻塞
int n = epoll_wait(epfd, events, MAX, -1);  // 等就绪
// 此时 fd 可读，但数据还没拷贝
int r = read(fd, buf, sz);  // ← 数据拷贝在这里发生
// read 返回时数据才到 buf
```

`epoll_wait` 返回只是说"fd 可读了"，数据拷贝发生在 `read` 调用时。
CPU 必须在 `read` 期间参与——这不是异步。

```c
// io_uring
io_uring_prep_read(sqe, fd, buf, sz, 0);
io_uring_submit(&ring);
// 提交后立刻返回，数据拷贝在内核后台进行
// CPU 可以去提交其他请求

io_uring_wait_cqe(&ring, &cqe);
// 此时数据已经在 buf 里了
// CPU 没有参与数据拷贝
```

---

## 十、性能优化技巧

### 10.1 批量提交

```c
/* 攒多个 SQE，一次提交 */
for (int i = 0; i < n; i++) {
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, fds[i], bufs[i], sz, 0);
}
io_uring_submit(&ring);  // 一次系统调用提交 n 个请求
```

### 10.2 使用 provide_buffers

```c
/* 预提供一组缓冲区，内核自动分配 */
io_uring_prep_provide_buffers(sqe, bufs, buf_size, nbufs, group_id, 0);

/* read 时不需要指定 buf，内核自动从池里取 */
io_uring_prep_read(sqe, fd, NULL, buf_size, 0);
sqe->flags |= IOSQE_BUFFER_SELECT;
```

### 10.3 多线程共享 io_uring

每个线程一个 io_uring 实例（类似 epoll 的 per-thread 模型）。
不要多线程共享一个 io_uring——SQ/CQ 不是线程安全的。

---

## 十一、运行与测试

### 11.1 基本测试

```bash
# 编译
cmake --build build --target uring_echo_server

# 运行
build/bin/uring_echo_server 8080

# 另一个终端测试
echo "hello" | nc -q1 localhost 8080
# 输出: hello

printf "line1\nline2\nline3\n" | nc -q1 localhost 8080
# 输出:
# line1
# line2
# line3
```

### 11.2 并发测试

```bash
# 多个客户端同时连接
for i in $(seq 1 100); do
    echo "msg_$i" | nc -q1 localhost 8080 &
done
wait
```

### 11.3 检查内核支持

```bash
# 内核版本 >= 5.1
uname -r

# 检查 io_uring 支持
# 如果 io_uring_queue_init 返回 -ENOSYS，说明内核不支持
```

---

## 十二、io_uring 的未来

### 12.1 内核版本支持

| 内核版本 | 新增功能 |
|----------|----------|
| 5.1 | 基本功能（read/write/accept 等） |
| 5.5 | timeout、cancel、linked SQE |
| 5.6 | sendmsg/recvmsg、splice |
| 5.7 | openat、close、statx |
| 5.11 | shutdown、rename、unlink |
| 5.12 | getdents（目录读取） |
| 5.18 | futex、waitid |
| 6.0 | 通用的 buffer 注册优化 |

### 12.2 谁在用 io_uring

- **Redis**：6.0+ 可选 io_uring 后端
- **Nginx**：实验性 io_uring 模块
- **QEMU**：io_uring 作为磁盘 IO 后端
- **RocksDB**：io_uring 作为异步 IO 后端
- **fio**：io_uring 引擎（性能测试）
- **Python asyncio**：experimental io_uring event loop

### 12.3 io_uring vs Windows IOCP

Windows 的 IOCP（IO Completion Port）一直是异步 IO 的标杆。
io_uring 借鉴了 IOCP 的思想，但设计更灵活：

| 特性 | IOCP | io_uring |
|------|------|----------|
| 模型 | 完成端口 | 共享内存环形队列 |
| 批量 | 支持 | 支持（更强） |
| 文件 IO | 支持 | 支持 |
| 网络 IO | 支持 | 支持 |
| 零拷贝 | 不原生 | splice/tee 原生 |
| 链式请求 | 不支持 | IOSQE_IO_LINK |
| 内核轮询 | 不适用 | SQPOLL |

---

## 十三、常见问题

### Q1: io_uring 和 aio (libaio) 的区别？

Linux 之前的异步 IO（libaio/aio_read）只支持**直接 IO**（O_DIRECT），
对普通文件和 socket 不起作用。io_uring 对所有 IO 类型都支持异步。

### Q2: io_uring 安全吗？

io_uring 引入了一些安全漏洞（如 CVE-2023-2597），因为共享内存模型复杂。
一些发行版（如 Debian）默认限制 io_uring 给非特权用户。
但内核持续修复，6.x 版本已经很稳定。

### Q3: SQ 满了怎么办？

`io_uring_get_sqe` 返回 NULL。需要先 `io_uring_submit` 释放一些 SQE，
或者增大队列大小（`io_uring_queue_init` 的 entries 参数）。

### Q4: 为什么我的 io_uring 程序比 epoll 慢？

可能原因：
1. 队列太小（ENTRIES 不够）
2. 没有批量提交（每次只提交一个 SQE）
3. 没有批量处理 CQE
4. 连接数太少（io_uring 的优势在高并发）
5. 用了中断模式而不是 SQPOLL

### Q5: io_uring 能完全替代 epoll 吗？

理论上可以。但：
1. 需要内核 5.1+
2. 编程模型更复杂
3. 对简单场景，epoll 够用且更简单
4. 生态迁移需要时间

---

## 十四、本章总结

### 学到了什么

1. **io_uring 的核心思想**：提交请求 → 内核异步执行 → 完成通知
2. **双环形队列**：SQ（提交）和 CQ（完成），共享内存无系统调用
3. **SQE 和 CQE**：请求和完成的数据结构
4. **liburing API**：io_uring_queue_init / get_sqe / prep_xxx / submit / wait_cqe
5. **与 epoll 的区别**：epoll 是就绪通知（还得自己 read），io_uring 是完成通知（数据已就绪）
6. **批量提交**：攒多个 SQE 一次 submit，减少系统调用
7. **SQPOLL 模式**：内核轮询 SQ，零系统调用提交
8. **链式请求**：IOSQE_IO_LINK，前一个完成才提交下一个
9. **固定缓冲区**：预映射减少开销
10. **性能优势**：高并发下系统调用次数大幅减少

### 代码量

`uring_echo_server.c` 约 280 行，实现了：
- io_uring 初始化
- 异步 accept
- 异步 read
- 异步 write
- 完成事件处理
- 连接管理

和 phase1/stage4 的 epoll echo server 相比，代码量差不多，
但系统调用次数在高并发下大幅减少。

### 下一站

下一章我们做 **内存池 + 异步日志**——高性能服务器的基础设施。
内存池解决 malloc 碎片和缓存局部性问题，
异步日志解决 IO 线程不能阻塞在日志写入的问题。

---

## 十五、io_uring 用于文件 IO

io_uring 不只能做网络 IO，文件 IO 才是它的主场——
因为传统 `read()` 读普通文件会阻塞（等磁盘），epoll 对文件 fd 无效。

### 15.1 异步读文件

```c
/* 异步读文件：open → read → close，全链路异步 */
struct io_uring ring;
io_uring_queue_init(32, &ring, 0);

/* 第一步：异步 open */
sqe = io_uring_get_sqe(&ring);
io_uring_prep_openat(sqe, AT_FDCWD, "test.txt", O_RDONLY, 0);
io_uring_sqe_set_data(sqe, (void *)OP_OPEN);
io_uring_submit(&ring);

/* 等 open 完成 */
io_uring_wait_cqe(&ring, &cqe);
int file_fd = cqe->res;  // 打开的文件 fd
io_uring_cqe_seen(&ring, cqe);

/* 第二步：异步 read */
sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, file_fd, buf, sizeof(buf), 0);
io_uring_sqe_set_data(sqe, (void *)OP_READ);
io_uring_submit(&ring);

/* 等 read 完成 */
io_uring_wait_cqe(&ring, &cqe);
int bytes_read = cqe->res;  // 读到的字节数
io_uring_cqe_seen(&ring, cqe);

/* 第三步：异步 close */
sqe = io_uring_get_sqe(&ring);
io_uring_prep_close(sqe, file_fd);
io_uring_submit(&ring);
```

### 15.2 链式文件操作

```c
/* open → read 链式提交，内核自动保证顺序 */
sqe1 = io_uring_get_sqe(&ring);
io_uring_prep_openat(sqe1, AT_FDCWD, "test.txt", O_RDONLY, 0);
sqe1->flags |= IOSQE_IO_LINK;  // 链式

sqe2 = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe2, /*fd=*/0, buf, sizeof(buf), 0);
// 注意：fd=0 是占位，内核会用前一个操作的结果（open 返回的 fd）
sqe2->flags |= IOSQE_IO_LINK;

sqe3 = io_uring_get_sqe(&ring);
io_uring_prep_close(sqe3, /*fd=*/0);
// 同样，用前一个的 fd

io_uring_submit(&ring);
// 一次提交，内核自动：open → read → close
```

### 15.3 Web 服务器用 io_uring 读文件

```c
/* HTTP 请求处理：用 io_uring 异步读文件 */
void handle_http_request(int client_fd, const char *path) {
    /* 提交异步 open */
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_openat(sqe, AT_FDCWD, path, O_RDONLY, 0);
    io_uring_sqe_set_data(sqe, encode_ud(OP_FILE_OPEN, client_fd));
    io_uring_submit(&ring);
    // 不阻塞，继续处理其他连接
}

/* open 完成后提交 read */
void on_file_open(struct io_uring_cqe *cqe) {
    int file_fd = cqe->res;
    int client_fd = decode_fd(cqe->user_data);

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, file_fd, file_buf, sizeof(file_buf), 0);
    io_uring_sqe_set_data(sqe, encode_ud(OP_FILE_READ, client_fd));
    io_uring_submit(&ring);
}

/* read 完成后提交 write 到客户端 */
void on_file_read(struct io_uring_cqe *cqe) {
    int bytes = cqe->res;
    int client_fd = decode_fd(cqe->user_data);

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, client_fd, file_buf, bytes, 0);
    io_uring_sqe_set_data(sqe, encode_ud(OP_FILE_WRITE, client_fd));
    io_uring_submit(&ring);
}
```

整个文件读取过程**零阻塞**——磁盘 IO 在内核后台完成，CPU 继续处理其他连接。

---

## 十六、io_uring 内部原理

### 16.1 共享内存布局

```
io_uring_queue_init 后的内存布局:

┌─────────────────────────────────┐
│         SQ ring (mmap)          │  ← 内核和用户共享
│  head, tail, mask, array[]      │
├─────────────────────────────────┤
│         CQ ring (mmap)          │  ← 内核和用户共享
│  head, tail, mask, cqes[]       │
├─────────────────────────────────┤
│         SQEs (mmap)             │  ← 内核和用户共享
│  sqe[0], sqe[1], ..., sqe[N-1]  │
└─────────────────────────────────┘
```

三块共享内存：
1. **SQ ring**：SQ 的头尾指针和索引数组
2. **CQ ring**：CQ 的头尾指针和 CQE 数组
3. **SQEs**：实际的 SQE 数据

用户写 SQE 到 SQEs 区域，通过 SQ ring 的 tail 指针通知内核。
内核写 CQE 到 CQ ring，用户通过 CQ ring 的 head 指针消费。

### 16.2 环形缓冲区操作

```c
/* 简化的 SQ 生产者（用户端） */
struct io_uring_sqe *get_sqe(struct io_uring *ring) {
    unsigned head = *ring->sq.khead;   // 读内核的 head（消费到哪了）
    unsigned tail = *ring->sq.ktail;   // 读自己的 tail（生产到哪了）
    unsigned mask = *ring->sq.kring_mask;

    if (tail - head >= mask + 1)  // 队列满
        return NULL;

    unsigned idx = tail & mask;   // 环形索引
    *ring->sq.ktail = tail + 1;   // 推进 tail
    // smp_store_release();  // 内存屏障，保证内核能看到

    return &ring->sq.sqes[idx];   // 返回 SQE 指针
}
```

```c
/* 简化的 CQ 消费者（用户端） */
struct io_uring_cqe *get_cqe(struct io_uring *ring) {
    unsigned head = *ring->cq.khead;   // 读自己的 head（消费到哪了）
    unsigned tail = *ring->cq.ktail;   // 读内核的 tail（生产到哪了）
    unsigned mask = *ring->cq.kring_mask;

    if (head == tail)  // 队列空
        return NULL;

    unsigned idx = head & mask;   // 环形索引
    return &ring->cq.cqes[idx];   // 返回 CQE 指针
}
```

### 16.3 内存屏障

共享内存并发访问需要**内存屏障**保证可见性：

```c
/* 写 SQE 后，需要 store-release 保证内核看到完整的 SQE */
smp_store_release(&ring->sq.ktail, tail + 1);

/* 读 CQE 前，需要 load-acquire 保证看到完整的 CQE */
unsigned tail = smp_load_acquire(&ring->cq.ktail);
```

这些屏障在 liburing 里已经处理好，用户不需要自己管。

---

## 十七、io_uring 调试

### 17.1 用 strace 观察

```bash
# strace 可以看到 io_uring 的系统调用
strace -e trace=io_uring_setup,io_uring_enter,io_uring_register \
    build/bin/uring_echo_server 8080

# 输出：
# io_uring_setup(256, {flags=0, ...}) = 3
# io_uring_enter(3, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0) = 1
# io_uring_enter(3, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0) = 1
# ...
```

### 17.2 用 perf 分析

```bash
# 统计 io_uring 系统调用次数
sudo perf stat -e 'raw_syscalls:sys_enter:args[1]==425' \
    build/bin/uring_echo_server 8080
# 425 = io_uring_enter 的系统调用号
```

### 17.3 /proc 查看 io_uring

```bash
# 查看进程的 io_uring 实例
cat /proc/$(pidof uring_echo_server)/fdinfo/3
# 3 = io_uring 的 fd
```

---

## 十八、io_uring 与零拷贝

### 18.1 splice 操作

io_uring 原生支持 splice——零拷贝管道传输：

```c
/* 从 socket 零拷贝到管道 */
sqe = io_uring_get_sqe(&ring);
io_uring_prep_splice(sqe,
    socket_fd, 0,        // 源：socket
    pipe_fd, 0,          // 目标：管道
    length, 0);          // 长度
io_uring_submit(&ring);

/* 从管道零拷贝到 socket */
sqe = io_uring_get_sqe(&ring);
io_uring_prep_splice(sqe,
    pipe_fd, 0,          // 源：管道
    socket_fd, 0,        // 目标：socket
    length, 0);
io_uring_submit(&ring);
```

数据全程在内核空间流转，不经过用户空间——真正的零拷贝。

### 18.2 代理服务器的零拷贝

```
反向代理场景:
  客户端 → [代理服务器] → 后端服务器

epoll 方式:
  read(backend_fd, buf, sz)     // 后端 → 用户 buf
  write(client_fd, buf, sz)     // 用户 buf → 客户端
  2 次拷贝

io_uring + splice 方式:
  splice(backend_fd → pipe)     // 后端 → 管道（内核内）
  splice(pipe → client_fd)      // 管道 → 客户端（内核内）
  0 次用户空间拷贝
```

这是 io_uring 在代理场景的核心优势。

---

## 十九、完整代码结构

```
phase2/stage9_io_uring/
├── uring_echo_server.c   # io_uring echo server（~280 行）
└── CMakeLists.txt        # 构建配置
```

### 19.1 代码结构总览

```
uring_echo_server.c
├── 全局变量
│   ├── struct io_uring ring    # io_uring 实例
│   └── conn_t *conns[65536]    # 连接表
│
├── user_data 编解码
│   ├── encode_ud(op, fd)       # 编码
│   ├── decode_op(ud)           # 解码操作类型
│   └── decode_fd(ud)           # 解码 fd
│
├── 提交函数
│   ├── submit_accept(fd)       # 提交 accept
│   ├── submit_read(fd)         # 提交 read
│   └── submit_write(fd, len)   # 提交 write
│
├── 完成处理
│   └── handle_completion(cqe)  # 处理 CQE
│       ├── OP_ACCEPT → submit_read + submit_accept
│       ├── OP_READ   → submit_write 或 close
│       └── OP_WRITE  → submit_read 或 close
│
└── main
    ├── io_uring_queue_init     # 初始化
    ├── 创建 listen socket
    ├── submit_accept           # 提交第一个 accept
    └── for (;;) {
          io_uring_wait_cqe     # 等完成
          handle_completion     # 处理
          io_uring_cq_advance   # 消费
        }
```

### 19.2 和 epoll echo server 的对比

| 方面 | epoll (stage4) | io_uring (stage9) |
|------|----------------|-------------------|
| 等待事件 | epoll_wait | io_uring_wait_cqe |
| 读数据 | read() 系统调用 | io_uring_prep_read（无系统调用） |
| 写数据 | write() 系统调用 | io_uring_prep_write（无系统调用） |
| accept | accept() 系统调用 | io_uring_prep_accept（无系统调用） |
| 编程模型 | 就绪通知 + 同步读写 | 提交 + 完成通知 |
| 代码量 | ~200 行 | ~280 行 |
| 系统调用/请求 | 3 (wait+read+write) | 1 (wait_cqe) |

---

## 二十、从教学到生产

### 20.1 生产代码还需要

1. **多 io_uring 实例**：每个线程一个 ring（类似 per-thread epoll）
2. **连接状态机**：更完整的状态管理（握手、读请求、处理、写响应）
3. **超时管理**：IORING_OP_TIMEOUT + IORING_OP_LINK_TIMEOUT
4. **缓冲区管理**：provide_buffers 或固定缓冲区池
5. **错误恢复**：SQ 满时的退避策略
6. **SQPOLL 模式**：高吞吐场景用内核轮询
7. **统计指标**：完成事件计数、延迟分布

### 20.2 参考实现

- **Nginx io_uring module**：`src/event/modules/ngx_io_uring_module.c`
- **Redis AOF**：`src/io_uring.c`（Redis 6.0+）
- **fio io_uring engine**：`engines/io_uring.c`
- **liburing examples**：`liburing/test/` 目录有很多示例