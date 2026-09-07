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

## 2. fork 的内核实现详解

前面我们用 `fork()` 创建子进程，看起来就像"魔法"一样：调用一次、返回两次、子进程拥有父进程的一切。
这一节我们钻进 Linux 内核，看看 `fork()` 到底做了什么，为什么它"贵"又"不贵"——这背后是操作系统最精妙的设计之一：写时复制（Copy-On-Write）。

### 2.1 fork() 系统调用的完整路径

用户态调用 `fork()` 后，glibc 通过软中断（`int 0x80` 或 `syscall` 指令）陷入内核，最终走到内核函数 `do_fork()`。整个调用链大致如下：

```
用户态:  fork()
           │
           ▼  (syscall 指令，陷入内核)
内核态:  entry_SYSCALL_64
           │
           ▼
         sys_fork()              ← 系统调用入口
           │
           ▼
         do_fork()               ← 通用 fork 主流程
           │
           ▼
         copy_process()          ← 复制进程描述符和资源
           │
           ├── copy_creds()      ← 复制凭证（uid、gid、capabilities）
           ├── copy_files()      ← 复制文件描述符表（fd 表）
           ├── copy_fs()         ← 复制当前目录、根目录信息
           ├── copy_sighand()    ← 复制信号处理函数表
           ├── copy_signal()     ← 复制信号相关数据
           ├── copy_mm()         ← 复制内存空间（页表，COW！）
           ├── copy_namespaces() ← 复制命名空间
           ├── copy_io()         ← 复制 I/O 上下文
           ├── copy_thread()     ← 复制内核栈、CPU 寄存器
           │
           ▼
         wake_up_new_task()      ← 把子进程加入就绪队列，准备调度
```

注意 `copy_process()` 里每个 `copy_xxx()` 都在"复制"父进程的一项资源。这正好对应了 UNIX 的经典哲学：**进程 = 代码 + 数据 + 一堆打开的文件 + 信号处理 + ...**，fork 就是把这些东西都复制一份。

但"复制"不等于"真的拷贝内存"。下面我们看最关键的一步：`copy_mm()`。

### 2.2 copy_mm() 与写时复制（Copy-On-Write）

`copy_mm()` 负责复制父进程的整个内存空间。如果真的把父进程的几十 MB 内存一字节一字节拷给子进程，fork 会慢得无法忍受。Linux 的做法是 **COW（Copy-On-Write，写时复制）**：

1. **不复制物理页，只复制页表**：子进程拿到一份和父进程几乎一样的页表，但所有数据页都指向**同一份物理内存**。
2. **把页表项标记为只读**：父子进程的页表项都改成"只读"。
3. **真正写入时才复制**：谁先写谁触发缺页中断，内核这时才把那一页复制一份，改成可写。

用一个例子说明。假设父进程有变量 `x` 在虚拟地址 `0x4000`，对应物理页 `#100`：

```
fork 之前：
  父进程页表:  VA 0x4000 → PA #100 (可读可写)
  物理页 #100:  存着 x 的值

fork 之后（COW）：
  父进程页表:  VA 0x4000 → PA #100 (只读!)   ← 被改成只读
  子进程页表:  VA 0x4000 → PA #100 (只读!)   ← 共享同一物理页
  物理页 #100:  存着 x 的值（只有一份）

子进程写 x = 99：
  ① 子进程写 0x4000，但页表说只读 → 触发缺页中断
  ② 内核分配新物理页 #200
  ③ 把 #100 的内容拷到 #200
  ④ 子进程页表: VA 0x4000 → PA #200 (可读可写)
  ⑤ 子进程在 #200 上写入 99
  ⑥ 父进程页表恢复: VA 0x4000 → PA #100 (可读可写)
```

这就是 COW 的精髓：**延迟复制，按页按需复制**。如果子进程 fork 后立刻 `exec` 加载新程序，所有旧页表都被丢弃，COW 让我们一分钱内存都没浪费。

### 2.3 用一段代码观察 COW

```c
/* cow_demo.c — 观察写时复制 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void)
{
    /* 分配 100 MB 内存，全部清零 */
    size_t sz = 100 * 1024 * 1024;
    char *big = malloc(sz);
    for (size_t i = 0; i < sz; i += 4096) big[i] = 1;  /* 先全部写入，确保物理页已分配 */

    printf("fork 前: 父进程 RSS 约 100MB\n");
    getchar();  /* 暂停，方便观察 /proc/<pid>/status 的 VmRSS */

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程：什么都不做，等 10 秒 */
        printf("fork 后: 子进程不写任何内存，等 10 秒...\n");
        sleep(10);
        _exit(0);
    }

    /* 父进程也等 10 秒，观察子进程 RSS */
    sleep(10);
    waitpid(pid, NULL, 0);
    printf("子进程退出。如果 COW 生效，子进程 RSS 应该接近 0\n");
    free(big);
    return 0;
}
```

运行后在另一个终端观察：

```bash
# 假设父进程 PID 是 1000，子进程是 1001
grep VmRSS /proc/1000/status   # 父进程约 100MB
grep VmRSS /proc/1001/status   # 子进程约 几百KB！共享了父进程的物理页
```

你会看到子进程的 RSS（常驻内存）非常小，因为它没有写那 100MB，所有页都和父进程共享。一旦子进程写入，对应页才会被复制，RSS 才会涨。

### 2.4 COW 的实际开销分析

很多人说"fork 有 COW 所以很便宜"——这只对了一半。COW 省的是**数据页的复制**，但 fork 仍然要做这些事，它们都不便宜：

| 步骤 | 开销 | 说明 |
|------|------|------|
| 复制页表 | O(页表大小) | 100MB 内存 ≈ 25600 页，页表项每项 8 字节 ≈ 200KB，要全部拷一遍 |
| 分配新 task_struct | 几 KB | 进程描述符，内核栈 |
| 复制 fd 表 | O(打开的 fd 数) | 每个 fd 一个 struct file 引用，引用计数 +1 |
| 复制信号表 | 小 | 信号处理函数数组 |
| TLB 刷新 | 贵！ | 修改页表后要刷新 CPU 的 TLB 缓存，多核时还要 IPI 通知其他核 |

所以 **fork 的开销和进程的内存大小成正比**，主要是页表复制和 TLB 刷新。对于 echo server 这种内存小的进程，fork 还算快；对于 PostgreSQL 这种几 GB 内存的进程，fork 就明显有成本（这也是 PostgreSQL 后来引入 `posix_fork` 加速的原因）。

COW 还有一个隐藏成本：**第一次写时的缺页中断**。子进程 fork 后第一次写每一页都会触发一次缺页中断（用户态 → 内核态 → 分配页 → 复制 → 返回），这个开销在大量写时累积起来也不小。

### 2.5 vfork：不复制页表的"危险 fork"

标准 fork 即使有 COW，也要复制页表。`vfork()` 是更激进的版本：**连页表都不复制，子进程直接借用父进程的内存空间**。

```c
pid_t pid = vfork();
if (pid == 0) {
    /* 子进程：和父进程共享同一份内存！ */
    /* 不能修改任何变量，不能 return，只能 _exit 或 exec */
    execve("/bin/echo", argv, envp);
    _exit(127);  /* exec 失败才走到这 */
}
/* 父进程在此期间被挂起，直到子进程 _exit 或 exec */
```

vfork 的规则非常严格：
- 子进程**绝对不能修改任何变量**（包括调用 `return`，因为 return 会修改栈上的返回地址）。
- 子进程只能调用 `_exit()` 或 `execve()`。
- 父进程在子进程 `_exit`/`exec` 之前**完全挂起**，不能运行。

为什么这么危险还要用？因为 fork+exec 这个组合太常见了（shell 执行命令就是 fork+exec），而 fork 复制的页表在 exec 后立刻被丢弃，纯属浪费。vfork 省掉这次页表复制，对 shell 这种频繁 fork+exec 的场景有明显加速。

现代 glibc 的 `posix_spawn()` 内部就用 vfork 或 clone 优化过，比 fork+exec 更快。

### 2.6 clone()：fork 的底层，Linux 线程也用它

在 Linux 里，**进程和线程没有本质区别**，都是 task_struct。区别只在于"创建时共享了哪些资源"。`clone()` 是真正的底层系统调用，fork 和 pthread_create 都基于它：

```c
/* clone 的原型（简化） */
long clone(unsigned long flags,
           void *child_stack,
           int *ptid, int *ctid,
           unsigned long newtls);
```

`flags` 决定父子共享什么：

| flag | 含义 |
|------|------|
| `CLONE_VM` | 共享内存空间（线程必须） |
| `CLONE_FILES` | 共享 fd 表（线程必须） |
| `CLONE_SIGHAND` | 共享信号处理表（线程必须） |
| `CLONE_THREAD` | 放入同一线程组（线程必须） |
| `CLONE_PARENT` | 共享同一个父进程 |

于是：

```c
/* fork ≈ clone 不共享任何东西 */
clone(SIGCHLD, ...);

/* pthread_create ≔ clone 共享 VM/FILES/SIGHAND/THREAD */
clone(CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | ..., ...);
```

这就是 Linux 著名的"线程只是共享资源多的进程"。理解了这点，你就明白为什么 Linux 上线程有时叫 LWP（Light Weight Process）——它确实就是个进程，只是 clone 时多带了几个共享标志。

### 2.7 小结

- `fork()` → `do_fork()` → `copy_process()`，逐项复制父进程资源。
- `copy_mm()` 用 COW：只复制页表，物理页共享，写时才复制。
- COW 省了数据页复制，但页表复制、TLB 刷新仍然和内存大小成正比。
- `vfork()` 连页表都不复制，子进程借用父进程内存，只能 `_exit`/`exec`。
- `clone()` 是底层，fork 和 pthread_create 都是它的特例，区别在共享多少资源。

---

## 3. 僵尸进程和孤儿进程深入

stage2 的 echo server 必须处理 SIGCHLD 回收子进程，否则就会产生**僵尸进程**。这一节我们彻底搞清楚：僵尸是什么、为什么可怕、怎么可靠地回收，以及父进程先死会怎样（孤儿进程）。

### 3.1 进程的几种状态回顾

Linux 进程主要有这些状态（`ps` 里看到的字母）：

| 状态字母 | 名称 | 含义 |
|---------|------|------|
| R | Running | 正在运行或在就绪队列 |
| S | Sleep | 可中断睡眠（等待事件、可被信号唤醒） |
| D | Disk sleep | 不可中断睡眠（等磁盘 I/O，不能被信号杀） |
| T | Traced/Stopped | 被 ptrace 或 SIGSTOP 暂停 |
| Z | Zombie | 僵尸！已退出但父进程还没 wait |

僵尸状态 Z 是这一节的主角。一个进程调用 `exit()` 后，它的代码、数据、打开的文件、内核栈**全部被释放**，唯独留下一个 `task_struct`（几十字节到几百字节），里面存着退出码和一些统计信息（用了多少 CPU 时间、占了多少内存峰值等），等父进程通过 `wait()` 来读取。

### 3.2 僵尸进程的产生原因

僵尸产生的根本原因：**父进程还没调用 wait/waitpid，子进程就先 exit 了**。

```
时间线：
  t1: 父进程 fork 出子进程
  t2: 子进程干活，exit(0)        ← 内核把子进程标记为 Z（僵尸）
  t3: 父进程还在忙别的，没 wait  ← 子进程一直保持 Z 状态
  t4: 父进程终于 waitpid         ← 内核回收 task_struct，僵尸消失
```

在 t2 到 t4 之间，子进程就是僵尸。它的特征：
- `ps` 里显示状态 `Z`，名字可能变成 `<defunct>`。
- 不占内存（除 task_struct），不占 CPU，不占 fd。
- 但占一个 PID（PID 资源有限，默认 32768）。
- 杀不掉！`kill -9` 对僵尸无效，因为它已经死了。

### 3.3 僵尸进程的危害

单个僵尸不可怕，可怕的是**僵尸堆积**：

1. **PID 耗尽**：每个僵尸占一个 PID。系统默认 max_pid=32768，几万个僵尸就能耗光，导致无法再 fork。
2. **task_struct 占内存**：每个 task_struct 几百字节到 1KB+，10 万个僵尸 ≈ 100MB 内核内存（不可换出）。
3. **监控告警**：运维看到一堆 defunct 进程会报警。
4. **`ps`/`top` 变慢**：进程多了遍历就慢。

真实案例：一个 PHP-CGI 服务器 fork 处理请求，但忘了 `wait`，跑了一晚上积累了上万僵尸，第二天新连接进不来——PID 耗尽。

### 3.4 回收僵尸的方法一：wait / waitpid

最直接的回收方式是父进程主动调用 `wait()` 或 `waitpid()`：

```c
#include <sys/wait.h>

/* wait：阻塞等待任意一个子进程退出 */
pid_t ret = wait(&status);
/* ret 是退出的子进程 PID，status 是退出码 */

/* waitpid：更灵活 */
pid_t ret = waitpid(-1, &status, 0);     /* 等任意子进程，阻塞 */
pid_t ret = waitpid(pid, &status, 0);    /* 只等指定 pid */
pid_t ret = waitpid(-1, &status, WNOHANG); /* 非阻塞：没有子进程退出就立刻返回 0 */
```

`WNOHANG` 是关键：它让 `waitpid` 不阻塞。如果没有子进程退出，立刻返回 0；如果有，返回那个子进程的 PID；如果没子进程可等了（都回收完），返回 -1 并设 errno=ECHILD。

但 echo server 不能在主循环里阻塞 `wait`——主循环要 `accept` 新连接。所以我们需要**异步回收**：让内核在子进程退出时通知我们，这就是 SIGCHLD。

### 3.5 回收僵尸的方法二：SIGCHLD 处理器

子进程退出时，内核会向父进程发 `SIGCHLD`（默认动作是忽略）。我们注册一个处理器，在处理器里 `waitpid`：

```c
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;  /* 保存 errno，防止 waitpid 改写 */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;  /* 循环回收所有已退出的子进程 */
    errno = saved_errno;  /* 恢复 errno */
}
```

两个细节非常重要：

**① 为什么用 while 循环？**
信号会合并。如果 3 个子进程在同一瞬间退出，内核可能只给父进程排一个 SIGCHLD（因为 SIGCHLD 是普通信号，未决位只有 1 bit，多次发送会被合并成 1 次）。如果处理器里只 `waitpid` 一次，就会漏掉 2 个僵尸。所以必须循环 `waitpid` 直到返回 0（没有更多可回收的）。

**② 为什么保存/恢复 errno？**
信号处理器可能在主程序正在执行某个系统调用（比如 `read`）时被中断。如果 `read` 返回 -1 且 errno=EINTR，处理器里又调了 `waitpid`（可能改写 errno），主程序看到的 errno 就被污染了。保存/恢复 errno 是信号处理器的标准做法。

### 3.6 孤儿进程：父进程先退出

僵尸是"子先死，父不收"。反过来，**父先死，子还在跑**，子进程就变成**孤儿进程（orphan）**。

孤儿不会一直没人管。内核规定：**父进程退出后，它的所有子进程被 init（PID=1）领养**。在现代 systemd 系统上，init 就是 systemd，它内置了一个循环不断 `wait`，所以孤儿到了 init 手里会被自动回收，不会变僵尸。

```
fork 前：
  父(1000) → 子(1001)

父进程 1000 退出：
  内核把 1001 的 parent 指针改成 init(1)
  init(1) → 子(1001)   ← 被领养

子进程 1001 退出：
  内核发 SIGCHLD 给 init(1)
  systemd 的 wait 循环回收它   ← 不会变僵尸
```

所以孤儿本身不可怕——可怕的是孤儿在变成孤儿的那一刻**正在做的事**。比如孤儿持有的锁、孤儿打开的文件描述符，这些都还在，但"管理者"已经换了。

### 3.7 实战案例：fork 后不回收导致僵尸堆积

下面这个"坏服务器"演示了僵尸堆积。它故意不处理 SIGCHLD：

```c
/* bad_server.c — 故意制造僵尸 */
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(void)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 128);

    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        pid_t pid = fork();
        if (pid == 0) {
            close(listen_fd);
            char buf[1024];
            ssize_t n = read(conn_fd, buf, sizeof(buf));  /* 读一次就退 */
            if (n > 0) write(conn_fd, buf, n);
            close(conn_fd);
            _exit(0);   /* 子进程退出，父进程没 wait → 变僵尸 */
        }
        close(conn_fd);
        /* 故意不 wait，也不装 SIGCHLD 处理器 */
    }
}
```

测试：

```bash
gcc bad_server.c -o bad_server && ./bad_server &
# 用 nc 连 100 次
for i in $(seq 1 100); do echo hi | nc -q1 localhost 8080; done
# 看僵尸数量
ps aux | grep -c defunct   # 会看到接近 100 个僵尸！
```

每来一个客户端就留一个僵尸，跑一晚上 PID 就耗光了。修复方法就是加上 3.5 节的 SIGCHLD 处理器。

### 3.8 双 fork 技巧：让 init 帮忙回收

有时候你不想写 SIGCHLD 处理器（比如写一次性脚本），可以用**双 fork**技巧：fork 两次，中间的子进程立刻 exit，让最里面的孙子变成孤儿被 init 领养：

```c
pid_t pid = fork();
if (pid == 0) {
    /* 中间子进程 */
    pid_t grandchild = fork();
    if (grandchild == 0) {
        /* 孙子：真正干活的 */
        do_work();
        _exit(0);
    }
    _exit(0);  /* 中间子进程立刻退出，孙子变孤儿被 init 领养 */
}
/* 父进程：wait 中间子进程（它立刻就退，很好等） */
waitpid(pid, NULL, 0);
/* 孙子由 init 回收，父进程不用管 */
```

这种技巧常见于 daemon 化的代码，但日常服务器还是推荐用 SIGCHLD 处理器，更直接。

### 3.9 小结

- 僵尸 = 子进程已 exit，父进程还没 wait，task_struct 没释放。
- 僵尸占 PID 和少量内核内存，堆积会耗光 PID。
- 回收方法：阻塞 `wait`、非阻塞 `waitpid(WNOHANG)`、SIGCHLD 处理器里循环 `waitpid`。
- 孤儿 = 父进程先 exit，子进程被 init(PID=1) 领养，init 自动回收，不会变僵尸。
- 双 fork 可以让 init 帮忙回收，但服务器场景还是用 SIGCHLD 更清晰。

---

## 4. 信号处理详解

stage2 的 SIGCHLD 处理看起来就几行代码，但信号这块坑非常多：信号会丢、信号会中断系统调用、信号处理器里能调什么函数都有限制。这一节把信号处理讲透。

### 4.1 信号的本质

信号是内核发给进程的"软件中断"。每个信号有一个编号（1~31 是传统信号，34~64 是实时信号）。信号到达时，内核暂停进程当前执行，转去运行注册的信号处理器，处理完再回来。

关键事实：
- **信号是异步的**：随时可能来，你不知道它会在哪一行代码之间插入。
- **传统信号会合并**：同一个信号多次 pending 只记一次（未决位是 bitmap）。
- **信号处理器里能做的事很有限**：因为随时可能打断主程序，处理器里只能调"异步信号安全"的函数（man 7 signal-safety 有列表）。`printf`、`malloc` 都不安全！

### 4.2 signal() vs sigaction()

注册信号处理器有两个 API：

```c
/* 老接口：简单但不可移植，行为不一致 */
void (*old)(int) = signal(SIGCHLD, handler);

/* 新接口：推荐，行为统一，选项丰富 */
struct sigaction sa;
sa.sa_handler = handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART;
sigaction(SIGCHLD, &sa, NULL);
```

`signal()` 的问题：不同 UNIX 版本对"被信号中断的系统调用是否自动重启"行为不一致。在老 System V 上不重启（系统调用返回 -1 errno=EINTR），在 BSD 上重启。写跨平台代码用 `sigaction()` 才能明确控制。

### 4.3 SA_RESTART：自动重启被中断的系统调用

信号到达时，主程序可能正卡在某个慢系统调用上（`accept`、`read`、`write`）。默认行为是：系统调用返回 -1，errno 设为 EINTR。这很烦——你得手动判断并重试。

`SA_RESTART` 让内核自动重启这些调用，就像信号没发生过一样：

```c
struct sigaction sa;
sa.sa_handler = sigchld_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART;   /* 关键：accept 被信号打断后自动重启，不返回 EINTR */
sigaction(SIGCHLD, &sa, NULL);

/* 之后 accept 即使被 SIGCHLD 打断，也不会返回 -1/EINTR，而是继续等 */
int conn_fd = accept(listen_fd, NULL, NULL);
```

注意：`SA_RESTART` 对**所有**慢系统调用都有效，但对 `accept` 在某些系统上有特殊行为（Linux 上是有效的）。我们的 echo server 加了 SA_RESTART，所以主循环里 `accept` 不会被 SIGCHLD 打断返回 EINTR。

### 4.4 SA_NOCLDWAIT：让内核不产生僵尸

`sigaction` 还有个 flag `SA_NOCLDWAIT`，设置后子进程退出时内核**直接回收，不产生僵尸**，也不发 SIGCHLD：

```c
struct sigaction sa;
sa.sa_handler = SIG_DFL;
sa.sa_flags = SA_NOCLDWAIT;   /* 子进程退出自动回收，无僵尸 */
sigaction(SIGCHLD, &sa, NULL);
```

听起来很美好，但有 caveat：
- 子进程的退出码你拿不到了（因为没 wait 机会）。
- 如果子进程是因信号异常退出，行为可能不一致。
- 不是所有系统都完全等价（Linux 上基本可用）。

echo server 不关心子进程退出码，理论上可以用 SA_NOCLDWAIT。但更常见、更可控的做法还是显式装 SIGCHLD 处理器 + `waitpid` 循环。

### 4.5 信号会中断哪些系统调用

"慢系统调用"会被信号中断，"快系统调用"不会。慢系统调用指可能无限期阻塞的：

- `read`/`write` 对管道、socket、终端（对普通磁盘文件不会阻塞，所以不会被中断）。
- `accept`、`connect`、`recvfrom`、`sendto`。
- `wait`、`waitpid`、`pause`。
- `select`、`poll`、`epoll_wait`、`sleep`。

被中断时：如果设了 `SA_RESTART`，内核自动重启（部分调用如 `epoll_wait` 在 Linux 上即使设了 SA_RESTART 也会返回 EINTR，要手动重试）；没设就返回 -1 errno=EINTR。

健壮的写法是**无论如何都处理 EINTR**：

```c
int conn_fd;
do {
    conn_fd = accept(listen_fd, NULL, NULL);
} while (conn_fd < 0 && errno == EINTR);   /* 被信号打断就重试 */
if (conn_fd < 0) { /* 真错误 */ perror("accept"); }
```

### 4.6 信号处理器里能调什么

信号处理器里只能调**异步信号安全（async-signal-safe）**的函数。man 7 signal-safety 列了完整列表，常见的有：

- `write`、`read`、`close`、`open`
- `waitpid`、`_exit`
- `signal`、`sigaction`、`sigprocmask`
- `errno`（读）

**不安全**的（但很多人误用）：
- `printf`、`fprintf`、`sprintf` —— 因为 stdio 内部有全局锁，主程序正 printf 到一半被信号打断，处理器里又 printf，死锁。
- `malloc`、`free` —— 堆有全局锁，同样可能死锁。
- `syslog` —— 内部可能 malloc。

我们的 SIGCHLD 处理器里只调了 `waitpid` 和读写 `errno`，都是安全的。如果你想打印调试信息，用 `write(2, msg, len)` 直接写 stderr，不要用 `printf`。

### 4.7 信号阻塞：sigprocmask

有时候你想保护一段关键代码不被信号打断（比如更新全局链表），用 `sigprocmask` 临时阻塞信号：

```c
sigset_t mask, oldmask;
sigemptyset(&mask);
sigaddset(&mask, SIGCHLD);

sigprocmask(SIG_BLOCK, &mask, &oldmask);   /* 阻塞 SIGCHLD */
/* 这段代码不会被 SIGCHLD 打断 */
update_global_list();
sigprocmask(SIG_SETMASK, &oldmask, NULL);  /* 恢复，期间积压的 SIGCHLD 这时才递送 */
```

注意：阻塞不是丢弃，信号会 pending，解除阻塞后才递送。这和 `SIG_IGN`（忽略，直接丢）不同。

### 4.8 自我管道（self-pipe）技巧

信号处理器里能做的事太少，一个经典技巧是 **self-pipe**：处理器里只往一个管道写一个字节，主循环用 `select`/`epoll` 监听这个管道，读到字节再在主循环里慢慢处理（可以调任何函数）。

```c
int pipefd[2];
pipe(pipefd);  /* pipefd[0] 读端，pipefd[1] 写端 */

void handler(int sig) {
    char c = 1;
    write(pipefd[1], &c, 1);  /* 只写一个字节，write 是异步信号安全的 */
}

/* 主循环 */
while (1) {
    /* select 同时监听 listen_fd 和 pipefd[0] */
    /* 如果 pipefd[0] 可读，说明有信号发生过，慢慢处理 */
}
```

这是 libevent、Redis 等高性能库处理信号的通用模式。它把"异步信号"转成"同步 I/O 事件"，在主循环里统一处理，避免了信号处理器里这不能调那不能调的麻烦。

### 4.9 小结

- 用 `sigaction` 不用 `signal`，行为明确可移植。
- `SA_RESTART` 自动重启被中断的系统调用，省去手动重试。
- `SA_NOCLDWAIT` 让内核不产生僵尸，但拿不到退出码。
- 信号处理器里只能调异步信号安全函数，**不能** `printf`/`malloc`。
- 复杂场景用 self-pipe 把信号转成 I/O 事件，在主循环里处理。

---

## 5. 多进程模型的优缺点分析

fork 并发模型是 UNIX 最古老的并发方式，至今仍在用（Apache prefork、PostgreSQL、PHP-FPM）。但它到底好在哪、差在哪？什么时候该用进程、什么时候该用线程、什么时候该用事件循环？这一节系统分析。

### 5.1 优点

**① 隔离性好**
每个子进程有独立的地址空间。一个子进程崩溃（段错误、内存越界）只死自己，父进程和其他子进程不受影响。父进程甚至能在 SIGCHLD 处理器里发现"子进程因 SIGSEGV 死了"，记日志后继续服务。这是多进程最大的优势。

对比多线程：一个线程段错误，整个进程（包括所有其他线程）一起死，没法恢复。

**② 编程简单，不用加锁**
子进程之间内存独立，改自己的变量不影响别人。不需要互斥锁、条件变量、原子操作这些容易写错的东西。echo server 的子进程各干各的，没有任何同步代码。

**③ 利用多核**
每个进程是一个独立的调度单位，内核可以把不同子进程调度到不同 CPU 核上，真正并行。这是相对单线程事件循环（如 select/poll）的优势——事件循环虽然能处理万级并发，但一个线程只能用一个核。

**④ 资源限制天然按进程隔离**
`setrlimit` 设的内存上限、CPU 时间上限，每个子进程独立计算。一个客户端把子进程的内存吃光，只影响这个子进程，不会拖垮全局。

**⑤ fork+exec 安全**
如果子进程要 `exec` 另一个程序（比如 CGI 调脚本），多进程模型天然合适，exec 会替换整个地址空间，没有线程状态要清理。

### 5.2 缺点

**① 进程开销大**
每个进程有自己的 task_struct、内核栈、页表、fd 表、内存空间。即使什么不干，一个进程也占几 MB（栈默认 8MB，加上共享库映射）。1000 并发 = 1000 进程 = 几 GB 内存，光内存就吃不消。

**② fork 本身慢**
如前所述，fork 要复制页表、刷新 TLB，开销和进程内存大小成正比。对几 GB 内存的进程，fork 一次可能要几毫秒。高 QPS 场景下这是瓶颈。

**③ IPC 复杂**
进程间不共享内存，要交换数据得用 IPC：pipe（字节流）、shm（共享内存，但要自己加锁）、msgqueue、semaphore、Unix domain socket。每种都有 API 要学、边界要处理。多线程直接共享全局变量，简单太多。

**④ 上下文切换成本高**
进程切换要切换页表、TLB 失效，比线程切换（不切页表）贵。进程数等于核数时还好，进程数远超核数时切换开销显著。

**⑤ 扩展性差**
进程数受内存和 PID 限制，几千就到顶。而事件循环（epoll）单进程能处理几万连接。C10K 问题就是多进程模型搞不定的。

### 5.3 Apache prefork 模型分析

Apache HTTP Server 的 prefork MPM 是经典的多进程并发模型，至今仍是很多发行版的默认。它的工作方式：

```
启动时：
  主进程 fork 出 N 个子进程（StartServers，默认 5）
  每个子进程阻塞在 accept 上等连接

运行时：
  一个子进程接到连接 → 处理 → 处理完继续 accept
  主进程监控负载：
    负载高 → 再 fork 几个子进程（最多 MaxClients）
    负载低 → 杀掉空闲子进程（保留 MinSpareServers）
```

配置示例：

```apache
<IfModule mpm_prefork_module>
    StartServers         5    # 启动时 fork 5 个
    MinSpareServers      5    # 至少保留 5 个空闲
    MaxSpareServers     10    # 最多保留 10 个空闲
    MaxClients         150    # 最多 150 个并发（150 个进程）
    MaxRequestsPerChild 1000  # 每个子进程处理 1000 个请求后退出（防内存泄漏）
</IfModule>
```

注意 `MaxRequestsPerChild`：每个子进程处理固定数量请求后主动退出，由父进程重新 fork 一个补上。这是为了**防止内存泄漏累积**——PHP 等脚本容易泄漏，定期重启子进程把泄漏清零。这是多进程模型独有的优势：进程死了内存就释放了，多线程没法这么干。

prefork 的优缺点就是 5.1/5.2 那些。它稳定可靠（一个请求崩了不影响别的），但 MaxClients 150 就到顶了，再多内存撑不住。所以 Apache 后来引入了 worker MPM（多进程+多线程）和 event MPM（多进程+异步），就是为了突破这个限制。

### 5.4 多进程 vs 多线程

| 维度 | 多进程（fork） | 多线程（pthread） |
|------|---------------|------------------|
| 隔离性 | 好，崩了不影响别人 | 差，一个崩全死 |
| 内存 | 重，每进程几 MB | 轻，每线程几 KB 栈 |
| 启动成本 | 高，fork 复制页表 | 低，只分配栈 |
| 同步 | 不需要（内存独立） | 需要锁/原子操作 |
| IPC | 复杂（pipe/shm） | 简单（共享全局变量） |
| 上下文切换 | 贵（切页表） | 便宜（不切页表） |
| 多核利用 | 能 | 能 |
| 调试 | 简单（各看各的） | 难（竞态、死锁） |
| 资源泄漏恢复 | 重启子进程即可 | 难，得整个进程重启 |

选择建议：
- **要稳定、要隔离、要能重启恢复** → 多进程（如 Web 服务器、数据库后端进程）。
- **要高并发、低开销、共享数据多** → 多线程（如游戏服务器、计算密集型服务）。
- **要极致并发（C10K+）** → 单进程事件循环（epoll）或异步框架。

### 5.5 现代混合模型

实际生产中很少用纯多进程或纯多线程，而是混合：

- **Nginx**：多进程 + 每进程 epoll。主进程 fork N 个 worker，每个 worker 用 epoll 处理几千连接。结合了多核利用和事件循环的高并发。
- **Gunicorn/uWSGI**：多进程 + 多线程。master fork N 个 worker，每个 worker 内部起 M 个线程。
- **Node.js cluster**：多进程，每个进程单线程事件循环。

理解了 fork 多进程的优缺点，你就能理解为什么这些框架这么设计——它们都在用多进程拿隔离和多核，用事件循环/线程拿并发量。

### 5.6 小结

- 多进程优点：隔离好、不用锁、能利用多核、能重启恢复。
- 多进程缺点：内存重、fork 慢、IPC 复杂、扩展性差（几千到顶）。
- Apache prefork 是经典实现，用 MaxRequestsPerChild 防泄漏。
- 现代 Web 服务器多用"多进程 + 事件循环"混合模型，兼顾隔离和高并发。

---

## 6. fork echo server 完整代码逐行讲解

最后我们把 3.4 节的完整代码拿出来，逐行讲清楚每一行在做什么、为什么这么写。读完这一节你应该能自己从零写出这个 echo server。

```c
/* stage2_echo_fork/echo_fork.c */
#include <stdio.h>      /* printf, perror */
#include <stdlib.h>     /* atoi, exit */
#include <string.h>     /* memset（虽然这里用 = {0} 初始化） */
#include <unistd.h>     /* read, write, close, fork, _exit */
#include <signal.h>     /* sigaction, sigemptyset, SIGCHLD, SA_RESTART */
#include <sys/socket.h> /* socket, bind, listen, accept, setsockopt */
#include <netinet/in.h> /* sockaddr_in, AF_INET, htons, htonl, INADDR_ANY */
#include <sys/wait.h>   /* waitpid, WNOHANG */
```

头文件按功能分组：stdio 是标准 I/O，unistd 是 POSIX 系统调用，sys/socket 是 socket API，sys/wait 是 waitpid。注意 `sys/wait.h` 是处理子进程必须的。

```c
#define BUF_SIZE 4096
```

读写缓冲区大小。4KB 是个折中：太小则 read/write 系统调用次数多（每次系统调用都有用户态/内核态切换开销）；太大则栈占用多。一般 4KB~64KB 都合理，对应一个或几个页。

```c
static void sigchld_handler(int sig)
{
    (void)sig;  /* 显式忽略参数，避免未使用参数警告 */
    /* 循环回收所有已退出的子进程 */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}
```

SIGCHLD 处理器。`(void)sig` 是消除编译器"未使用参数"警告的惯用法。`waitpid(-1, NULL, WNOHANG)`：
- 第一个参数 `-1` 表示等任意子进程。
- 第二个参数 `NULL` 表示不关心退出码（echo server 不需要）。
- 第三个参数 `WNOHANG` 表示非阻塞，没有子进程退出就立刻返回 0。
- 返回值 > 0 是回收的子进程 PID，== 0 表示还有子进程但都没退出，< 0 表示出错（比如没有子进程了）。

`while` 循环是为了应对信号合并：多个子进程同时退出可能只触发一次 SIGCHLD，必须循环把所有已退出的都回收掉。

注意这个处理器里只调了 `waitpid`，是异步信号安全的。没有 `printf`、没有 `malloc`。

```c
int main(int argc, char *argv[])
{
    int port = argc > 1 ? atoi(argv[1]) : 8080;
```

从命令行参数读端口，没传就用 8080。`argc > 1` 判断防止访问越界。

```c
    /* 设置 SIGCHLD 处理 */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;  /* 被信号打断的系统调用自动重启 */
    sigaction(SIGCHLD, &sa, NULL);
```

用 `sigaction` 而不是 `signal`，因为 `sigaction` 行为明确可移植。`sa_mask` 设为空集，表示处理器执行期间不额外阻塞其他信号。`SA_RESTART` 让被 SIGCHLD 打断的 `accept` 自动重启，不返回 EINTR。

这一段必须在 `fork` 之前设置，否则子进程继承的信号处理可能不对（虽然 fork 会复制父进程的信号表，但提前设好更清晰）。

```c
    /* 创建监听 socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
```

`AF_INET` 是 IPv4，`SOCK_STREAM` 是 TCP，第三个参数 0 让内核自动选协议（TCP）。返回一个 fd，后续所有操作都用它。

```c
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
```

`SO_REUSEADDR` 允许绑定处于 TIME_WAIT 状态的端口。没有这行，服务器重启时会 bind 失败（"Address already in use"），要等几十秒 TIME_WAIT 过期。这是服务器必加的选项。

```c
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 128);
```

`= {0}` 把整个结构体清零（比 memset 简洁）。`htons`/`htonl` 把主机字节序转网络字节序（大端），这是网络编程必须的，因为不同 CPU 字节序不同。`INADDR_ANY` 表示绑定所有网卡（0.0.0.0），不挑具体接口。

`listen` 的第二个参数 128 是 backlog，内核 accept 队列的最大长度——已完成三次握手但还没被 accept 的连接最多 128 个。超出内核会拒绝新连接（客户端看到连接被重置）。

```c
    printf("echo server (fork) listening on 0.0.0.0:%d\n", port);
```

启动提示。注意这是 fork 之前，只有父进程会打印一次。如果放在 fork 之后，每个子进程都会打印，日志就乱了。

```c
    char buf[BUF_SIZE];
```

读写缓冲区。**注意它声明在主循环外面，每个子进程 fork 时通过 COW 继承一份**。子进程读写它不影响父进程。如果用多线程，这个 buf 就不能是全局的（所有线程共享会互相覆盖），得每个线程自己分配——这是多进程模型"不用加锁"的一个小体现。

```c
    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) continue;
```

无限循环接客。`accept` 从已完成队列取一个连接，返回新的 conn_fd。两个 NULL 表示不关心客户端地址。如果返回 < 0 是出错（比如被信号打断且没设 SA_RESTART），这里简单 continue 重试。生产代码应该区分 errno：EINTR 重试，其他真错误记日志。

```c
        pid_t pid = fork();
        if (pid < 0) {
            /* fork 失败 */
            close(conn_fd);
            continue;
        }
```

fork 一次，返回两次。父进程拿到子进程 PID（>0），子进程拿到 0，失败返回 -1。fork 失败通常是因为内存不够或进程数到上限，这里关掉 conn_fd 继续服务，不让一个失败拖垮整个服务器。

```c
        if (pid == 0) {
            /* 子进程 */
            close(listen_fd);  /* 不需要门口 */
```

子进程分支。**必须 close(listen_fd)**：子进程不接新连接，留着 listen_fd 没用还占引用。更严重的是，如果子进程不关 listen_fd，父进程想退出时端口不会立刻释放（引用计数还 > 0）。

```c
            ssize_t n;
            while ((n = read(conn_fd, buf, sizeof(buf))) > 0) {
                write(conn_fd, buf, n);
            }
```

echo 主循环：读到多少写回多少。`read` 返回 > 0 是读到的字节数，== 0 是对端关闭（EOF），< 0 是出错。`write` 这里没检查返回值，严格说应该检查（write 可能因为对端关闭只写了一部分，或被信号中断），但 echo server 教学版简化了。

注意 `n` 是 `ssize_t`（有符号），因为要区分正数（字节数）、0（EOF）、-1（错误）。如果用 `size_t`（无符号），-1 会变成巨大的正数，循环就出 bug。

```c
            close(conn_fd);
            exit(0);
```

子进程干完活，关连接，退出。用 `exit` 不用 `_exit`，因为 `exit` 会 flush stdio 缓冲区（虽然这里没用 stdio 写）。如果在信号处理器里退出才必须用 `_exit`。

```c
        } else {
            /* 父进程 */
            close(conn_fd);  /* 不需要通道 */
        }
```

父进程分支。**必须 close(conn_fd)**：父进程不处理这个连接，留着没意义。更关键的是 fd 泄漏——如果不关，每来一个客户端父进程的 fd 表就多一项，几百个后就耗光 fd（默认 1024）。而且 conn_fd 的内核引用计数还是 2，子进程 close 只减到 1，连接不会真正关闭。

```c
    }
    return 0;
}
```

主循环永不退出（`for(;;)`），`return 0` 实际上到不了，写上只是让编译器满意（main 必须有返回值）。生产服务器会装 SIGTERM/SIGINT 优雅退出。

### 6.1 整体流程图

```
父进程                          子进程
  │                               │
  ├─ accept ──→ conn_fd           │
  │                               │
  ├─ fork ──────────────────────→ │
  │                               │
  │ close(conn_fd)                close(listen_fd)
  │                               │
  │                               ├─ read(conn_fd)
  │                               ├─ write(conn_fd)
  │                               ├─ ... 循环
  │                               │
  │                               ├─ close(conn_fd)
  │                               └─ exit(0) → 僵尸 Z
  │                               │
  ├─ accept（下一个连接）          │
  │                               │
  ├─ 收到 SIGCHLD                 │
  ├─ waitpid → 回收僵尸           │
  │                               │
  └─ ...                          ▼
```

### 6.2 测试运行

编译运行：

```bash
gcc -Wall -o echo_fork echo_fork.c
./echo_fork 8080
# 另一个终端
echo "hello" | nc localhost 8080   # 应该返回 hello
# 多并发测试
for i in $(seq 1 50); do
    (echo "client $i" | nc -q1 localhost 8080 &)
done
wait
# 看进程
ps aux | grep echo_fork   # 应该看到 1 个父进程 + 若干子进程（处理完就消失）
# 看僵尸（应该没有）
ps aux | grep echo_fork | grep -c defunct   # 0
```

如果注释掉 SIGCHLD 处理那段代码再测，你会看到僵尸堆积——这就是 3.7 节演示的现象。

### 6.3 小结

逐行看完，这个不到 80 行的 echo server 涉及的知识点其实非常多：socket API、字节序、TIME_WAIT、fork 的返回值语义、fd 引用计数、僵尸回收、信号处理、异步信号安全、缓冲区大小选择……每一行背后都有操作系统课程的一个章节。这就是"从零写 Web 服务器"的价值——你被迫理解每一层。

---

## 7. 本篇总结

这一篇我们深入了 stage2 的多进程并发模型：

1. **fork 内核实现**：`do_fork → copy_process → copy_mm`，COW 让 fork 不必真复制内存，但页表复制和 TLB 刷新仍和内存大小成正比。vfork 更激进不复制页表，clone 是底层统一接口。
2. **僵尸与孤儿**：僵尸是子先死父不收，堆积会耗光 PID；孤儿是父先死被 init 领养，init 自动回收。回收用 SIGCHLD 处理器里循环 `waitpid(WNOHANG)`。
3. **信号处理**：用 `sigaction` 不用 `signal`，`SA_RESTART` 自动重启被中断的系统调用，处理器里只能调异步信号安全函数，复杂场景用 self-pipe。
4. **多进程模型**：优点是隔离好、不用锁、能重启恢复；缺点是内存重、fork 慢、IPC 复杂、扩展性差。Apache prefork 是经典实现，现代服务器多用"多进程 + 事件循环"混合。
5. **逐行讲解**：80 行代码背后是 socket、fork、fd、信号、僵尸一整套操作系统知识。

理解了这些，你不仅能写 echo server，还能看懂 Apache、PostgreSQL、PHP-FPM 这些生产级软件的进程模型设计。下一篇 stage3 我们进入多线程（pthread），看线程模型如何用更轻的方式实现同样的并发。

