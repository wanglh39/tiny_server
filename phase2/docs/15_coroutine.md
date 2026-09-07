# stage13 - 协程（Coroutine）

## 本章导读

在 phase1 的 stage4（epoll echo 服务器），我们用**回调**处理异步 IO：

```c
/* 回调风格：逻辑分散 */
void on_read(int fd) {
    int n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        write(fd, buf, n);  // 如果 write 也会阻塞呢？
    }
}
```

回调风格的问题：
- **逻辑碎片化**：一个完整流程被拆成多个回调
- **状态管理复杂**：需要在回调间传递状态（用结构体或闭包）
- **错误处理困难**：try-catch 无法跨回调
- **回调地狱**：嵌套回调层层缩进

**协程**的解法极其优雅：**用同步的写法写异步代码**。

```c
/* 协程风格：逻辑是线性的 */
void echo_routine(void *arg) {
    for (;;) {
        int n = co_read(fd, buf, sizeof(buf));   // 等 IO 时自动 yield
        if (n <= 0) break;
        co_write(fd, buf, n);                     // 等 IO 时自动 yield
    }
}
```

`co_read` 内部：有数据就返回，没数据就 `yield`（挂起协程），等 epoll 通知 fd 就绪后再 `resume`（恢复协程）。
**看起来是同步的 `read()`，实际是异步的**。

```bash
cmake --build build --target co_demo
build/bin/co_demo           # 协程基础演示

cmake --build build --target co_echo_server
build/bin/co_echo_server 8080
echo "hello coroutine" | nc localhost 8080
```

---

## 一、什么是协程

### 1.1 协程 vs 线程

协程（Coroutine）是**用户态的轻量级线程**。

| 特性         | 线程              | 协程              |
| ------------ | ----------------- | ----------------- |
| 调度者       | 内核              | 用户态（程序员）  |
| 调度方式     | 抢占式            | 协作式            |
| 切换时机     | 任意时刻          | 只在 yield 时     |
| 切换开销     | 系统调用（大）    | 保存/恢复寄存器（小）|
| 栈大小       | 内核线程 8MB      | 用户指定（如 64KB）|
| 数据共享     | 需要锁            | 不需要锁（单线程）|
| 数量上限     | 几千              | �十万             |

**抢占式 vs 协作式**：

```
线程（抢占式）:
  线程 A 正在执行 i++
  ← 内核时钟中断，切到线程 B
  线程 B 执行 i--          ← 可能在任意位置被切走
  ← 内核时钟中断，切回线程 A
  线程 A 继续执行

协程（协作式）:
  协程 A 正在执行 i++
  协程 A 执行 yield         ← 只有 yield 才会切换
  协程 B 执行 i--
  协程 B 执行 yield
  协程 A 从 yield 后继续
```

协程的"协作式"意味着：**协程主动让出 CPU，不会被抢占**。
好处是不需要锁（单线程内），坏处是一个协程不让出 CPU 会饿死其他协程。

### 1.2 协程的执行流程

```
调度器              协程 A              协程 B
  │                   │                   │
  │── resume(A) ───→ │                   │
  │                   │ 执行代码          │
  │                   │── yield ─────→   │
  │ ←── (A 挂起) ─── │                   │
  │                   │                   │
  │── resume(B) ──────────────────────→ │
  │                   │                   │ 执行代码
  │                   │                   │── yield ──→
  │ ←── (B 挂起) ────────────────────── │
  │                   │                   │
  │── resume(A) ───→ │                   │
  │                   │ 从 yield 后继续  │
  │                   │ 执行代码          │
  │                   │── return ──→     │
  │ ←── (A 结束) ─── │                   │
  │                   │                   │
  │── resume(B) ──────────────────────→ │
  │                   │                   │ 从 yield 后继续
  │                   │                   │── return ──→
  │ ←── (B 结束) ────────────────────── │
  │
  ▼ (所有协程结束)
```

### 1.3 为什么协程适合异步 IO

异步 IO 的本质：**IO 操作不阻塞，没数据时先做别的事，有数据时再回来处理**。

回调风格的问题：每次 IO 操作都要注册一个回调，代码流程被打断。

协程风格的解法：**IO 操作不阻塞，没数据时 yield，有数据时 resume**。
代码流程是线性的，和同步代码一样。

```
回调风格:
  read(fd, buf, callback1)
  → callback1(n) { process(buf); write(fd, buf, callback2) }
  → callback2(n) { read(fd, buf, callback1) }  // 循环

协程风格:
  while (1) {
      n = co_read(fd, buf);      // 没 data 就 yield
      process(buf);
      co_write(fd, buf, n);      // 写不完就 yield
  }
```

---

## 二、ucontext API

### 2.1 什么是 ucontext

`ucontext` 是 POSIX 提供的**用户态上下文切换**接口。
它允许保存和恢复 CPU 的全部状态（寄存器、栈指针、指令指针等），
从而实现协程切换。

```c
#include <ucontext.h>

typedef struct ucontext {
    unsigned long     uc_flags;     /* 标志 */
    ucontext_t       *uc_link;      /* 后继上下文（当前结束后切到哪） */
    stack_t           uc_stack;     /* 栈 */
    mcontext_t        uc_mcontext;  /* 机器寄存器（rip, rsp 等） */
    sigset_t          uc_sigmask;   /* 信号掩码 */
    /* ... 平台相关字段 ... */
} ucontext_t;
```

### 2.2 四个核心函数

```c
/* 获取当前上下文（保存到 ucp） */
int getcontext(ucontext_t *ucp);

/* 设置上下文的入口函数 */
void makecontext(ucontext_t *ucp, void (*func)(), int argc, ...);

/* 切换上下文：保存当前到 oucp，恢复到 ucp */
int swapcontext(ucontext_t *oucp, const ucontext_t *ucp);

/* 设置上下文（直接恢复 ucp，不保存当前） */
int setcontext(const ucontext_t *ucp);
```

### 2.3 getcontext + makecontext + swapcontext

典型用法：

```c
ucontext_t ctx;
char stack[64 * 1024];

/* 1. 获取当前上下文作为基础 */
getcontext(&ctx);

/* 2. 设置栈 */
ctx.uc_stack.ss_sp   = stack;
ctx.uc_stack.ss_size = sizeof(stack);
ctx.uc_stack.ss_flags = 0;

/* 3. 设置后继上下文（func 结束后切到哪） */
ctx.uc_link = NULL;  // 或指向主上下文

/* 4. 设置入口函数 */
makecontext(&ctx, my_func, 0);

/* 5. 切换到新上下文 */
ucontext_t main_ctx;
swapcontext(&main_ctx, &ctx);  // 保存当前到 main_ctx，切到 ctx

// my_func 执行完毕或 yield 后回到这里
```

### 2.4 上下文切换原理

`swapcontext(&old, &new)` 做的事：

```
1. 保存当前 CPU 寄存器到 old：
   old.uc_mcontext.rip = 当前指令地址
   old.uc_mcontext.rsp = 当前栈指针
   old.uc_mcontext.rbp = 当前基址指针
   old.uc_mcontext.rbx = ...
   ... (保存所有 callee-saved 寄存器)

2. 从 new 恢复 CPU 寄存器：
   rip = new.uc_mcontext.rip
   rsp = new.uc_mcontext.rsp
   rbp = new.uc_mcontext.rbp
   ...

3. CPU 从 new 的 rip 继续执行
```

这和内核的线程切换完全一样，区别是：
- **线程切换**：在内核态，由内核调度器触发
- **ucontext 切换**：在用户态，由用户代码触发

### 2.5 栈的作用

每个协程有自己的**独立栈**：

```
主线程栈:           协程 A 栈:         协程 B 栈:
┌──────────┐       ┌──────────┐       ┌──────────┐
│ main()   │       │ co_func()│       │ co_func()│
│ ...      │       │ ...      │       │ ...      │
│ swapctx  │       │ yield    │       │ yield    │
│          │       │          │       │          │
└──────────┘       └──────────┘       └──────────┘
  低地址              低地址              低地址
```

切换时，`rsp` 指向不同协程的栈，函数调用和局部变量互不干扰。

---

## 三、协程库实现

### 3.1 数据结构

```c
typedef enum {
    CO_READY,      /* 已创建，未运行 */
    CO_RUNNING,    /* 正在运行 */
    CO_SUSPENDED,  /* 已 yield，等待恢复 */
    CO_DEAD,       /* 已结束（函数 return） */
} co_state_t;

typedef struct coroutine {
    ucontext_t   ctx;          /* 协程上下文 */
    ucontext_t  *caller;       /* 调用者上下文 */
    co_state_t   state;        /* 协程状态 */
    void       (*func)(void *); /* 入口函数 */
    void        *arg;          /* 函数参数 */
    char        *stack;        /* 独立栈 */
    int          id;           /* 协程 ID */
} coroutine_t;
```

### 3.2 创建协程

```c
#define CO_STACK_SIZE (64 * 1024)  /* 64KB 栈 */

coroutine_t *co_create(void (*func)(void *), void *arg)
{
    static int next_id = 0;

    coroutine_t *co = calloc(1, sizeof(coroutine_t));
    co->func  = func;
    co->arg   = arg;
    co->state = CO_READY;
    co->id    = next_id++;
    co->stack = malloc(CO_STACK_SIZE);

    /* 初始化上下文 */
    getcontext(&co->ctx);
    co->ctx.uc_stack.ss_sp   = co->stack;
    co->ctx.uc_stack.ss_size = CO_STACK_SIZE;
    co->ctx.uc_link          = NULL;

    /* 设置入口函数 */
    makecontext(&co->ctx, co_entry, 0);

    return co;
}
```

`co_entry` 是包装函数，因为 `makecontext` 要求 `void f(void)` 签名：

```c
static void co_entry(void)
{
    coroutine_t *co = g_current_co;

    /* 执行用户函数 */
    co->func(co->arg);

    /* 函数返回，协程结束 */
    co->state = CO_DEAD;
    log_debug("协程 %d 结束", co->id);

    /* 切回调度器 */
    swapcontext(&co->ctx, co->caller);
}
```

### 3.3 恢复协程

```c
void co_resume(coroutine_t *co)
{
    if (co->state == CO_DEAD) return;

    coroutine_t *prev = g_current_co;
    g_current_co = co;
    co->state = CO_RUNNING;

    ucontext_t caller_ctx;
    co->caller = &caller_ctx;

    /* 保存当前上下文到 caller_ctx，切到协程 */
    swapcontext(&caller_ctx, &co->ctx);

    /* 协程 yield 或 return 后回到这里 */
    g_current_co = prev;
}
```

`co->caller = &caller_ctx` 保存了 `co_resume` 的栈帧。
协程 `yield` 时 `swapcontext(&co->ctx, co->caller)` 切回这里。

### 3.4 挂起协程

```c
void co_yield(coroutine_t *co)
{
    if (!co || co->state != CO_RUNNING) return;

    co->state = CO_SUSPENDED;

    /* 保存协程上下文到 co->ctx，切回调用者 */
    swapcontext(&co->ctx, co->caller);
}
```

`yield` 做的事：
1. 把 `CO_RUNNING` 改为 `CO_SUSPENDED`
2. 保存当前协程的寄存器到 `co->ctx`
3. 从 `co->caller` 恢复寄存器（回到 `co_resume`）

下次 `co_resume` 时，`swapcontext(&caller_ctx, &co->ctx)` 会从 `co->ctx` 恢复，
CPU 从 `yield` 后面继续执行。

### 3.5 状态转换

```
co_create() ──→ CO_READY
                    │
                    │ co_resume()
                    ▼
               CO_RUNNING ←──────────┐
                    │                │
                    │ co_yield()     │ co_resume()
                    ▼                │
               CO_SUSPENDED ────────┘
                    │
                    │ co_resume() → 函数 return
                    ▼
                 CO_DEAD
                    │
                    │ co_free()
                    ▼
                  [释放]
```

---

## 四、协程调度器

### 4.1 调度器结构

```c
#define MAX_COROUTINES 4096

typedef struct scheduler {
    coroutine_t *coroutines[MAX_COROUTINES];
    int          count;
    int          current;   /* 当前运行的协程索引 */
    ucontext_t   main_ctx;  /* 调度器主上下文 */
} scheduler_t;
```

### 4.2 轮转调度

```c
void sched_run(scheduler_t *sched)
{
    g_sched = sched;

    int alive = 1;
    while (alive) {
        alive = 0;
        for (int i = 0; i < sched->count; i++) {
            coroutine_t *co = sched->coroutines[i];
            if (!co || co->state == CO_DEAD) continue;

            alive = 1;
            co_resume(co);

            if (co->state == CO_DEAD) {
                co_free(co);
                sched->coroutines[i] = NULL;
            }
        }
    }
}
```

这是最简单的调度策略：**轮询所有协程，resume 每个非 DEAD 的协程**。

真实场景中，调度器和 epoll 结合：
- `epoll_wait` 返回就绪 fd → `resume` 对应协程
- 协程 `yield`（等 IO）→ 回到 `epoll_wait`

### 4.3 调度流程

```
sched_run()
    │
    │ while (alive)
    ▼
    ┌── for each coroutine ──┐
    │                        │
    │   co_resume(co[i])     │
    │       │                │
    │       ▼                │
    │   协程执行...          │
    │       │                │
    │       ├── yield ──→ 回到 resume
    │       │                │
    │       └── return ──→ 标记 DEAD
    │                        │
    └────────────────────────┘
    │
    │ 所有协程 DEAD → alive = 0
    ▼
   退出
```

---

## 五、协程版 IO

### 5.1 co_read

```c
static int co_read(coroutine_t *co, int fd, char *buf, int size)
{
    for (;;) {
        int n = read(fd, buf, size);
        if (n > 0) return n;      /* 有数据，返回 */
        if (n == 0) return 0;     /* 对端关闭 */

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_yield(co);         /* 没数据，挂起协程 */
            continue;             /* 恢复后重试 read */
        }
        return -1;                /* 错误 */
    }
}
```

这就是协程的魔力所在：
- 有数据 → 直接返回，和普通 `read` 一样
- 没数据 → `yield`（挂起协程），调度器在 fd 可读后 `resume`
- 恢复后 → 回到 `for` 循环，重试 `read`，这次有数据了

**从调用者角度看，`co_read` 就是一个普通的阻塞 `read`**。
但底层是非阻塞的，协程会在等 IO 时让出 CPU。

### 5.2 co_write

```c
static int co_write(coroutine_t *co, int fd, const char *buf, int size)
{
    int written = 0;
    while (written < size) {
        int n = write(fd, buf + written, size - written);
        if (n > 0) {
            written += n;
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_yield(co);
            continue;
        }
        return -1;
    }
    return written;
}
```

### 5.3 对比回调版

```c
/* 回调版（phase1/stage4）*/
void on_read(int fd) {
    int n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        /* 写入可能不完整，需要注册写事件 */
        int written = write(fd, buf, n);
        if (written < n) {
            /* 注册 EPOLLOUT，等可写再继续写 */
            /* 需要保存 buf + written 和 n - written 到某处 */
            /* 状态管理开始变复杂... */
        }
    }
}

/* 协程版（本文件）*/
void echo_routine(void *arg) {
    for (;;) {
        int n = co_read(self, fd, buf, sizeof(buf));
        if (n <= 0) break;
        co_write(self, fd, buf, n);  /* 自动处理部分写 */
    }
}
```

协程版代码是**线性的**，没有回调嵌套，不需要手动管理状态。

---

## 六、协程版 echo 服务器

### 6.1 连接协程

每个连接分配一个协程：

```c
typedef struct co_conn {
    int             fd;
    coroutine_t    *co;
    char            buf[BUF_SIZE];
} co_conn_t;
```

### 6.2 echo 协程函数

```c
static void echo_routine(void *arg)
{
    co_conn_t *conn = (co_conn_t *)arg;
    coroutine_t *self = conn->co;
    char buf[BUF_SIZE];

    for (;;) {
        /* 读数据（IO 不就绪时自动 yield） */
        int n = co_read(self, conn->fd, buf, sizeof(buf));
        if (n <= 0) break;

        /* Echo：原样写回 */
        co_write(self, conn->fd, buf, n);
    }
}
```

这段代码和**同步阻塞版 echo 服务器**几乎一模一样！
唯一的区别是 `read` → `co_read`，`write` → `co_write`。

### 6.3 主循环

```c
for (;;) {
    int n = epoll_wait(g_epfd, events, MAX_EVENTS, -1);

    for (int i = 0; i < n; i++) {
        if (events[i].data.fd == listen_fd) {
            /* 新连接：创建协程 */
            int conn_fd = accept(listen_fd, NULL, NULL);
            co_conn_t *conn = calloc(1, sizeof(co_conn_t));
            conn->fd = conn_fd;
            conn->co = co_create(echo_routine, conn);

            /* 加入 epoll */
            epoll_ctl(g_epfd, EPOLL_CTL_ADD, conn_fd, &cev);
        } else {
            /* 数据到达：恢复对应协程 */
            int fd = events[i].data.fd;
            handle_conn(g_epfd, g_conns[fd]);
        }
    }
}
```

### 6.4 handle_conn

```c
static void handle_conn(int epfd, co_conn_t *conn)
{
    /* 恢复协程 */
    co_resume(conn->co);

    if (co_state(conn->co) == CO_DEAD) {
        /* 协程结束，清理连接 */
        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
        close(conn->fd);
        co_free(conn->co);
        free(conn);
    }
    /* 如果协程还活着（SUSPENDED），什么都不做。
       epoll 会在 fd 就绪时再次调用 handle_conn。 */
}
```

### 6.5 工作流程

```
epoll_wait 返回 fd 可读
        │
        ▼
handle_conn(fd)
        │
        ├── co_resume(conn->co)
        │       │
        │       ▼
        │   echo_routine:
        │       │
        │       ├── co_read()
        │       │     ├── read() → 有数据 → 返回 n
        │       │     └── read() → EAGAIN → co_yield() ──→ 回到 handle_conn
        │       │
        │       ├── co_write()
        │       │     ├── write() → 写完 → 返回
        │       │     └── write() → EAGAIN → co_yield() ──→ 回到 handle_conn
        │       │
        │       └── 循环回到 co_read()
        │
        ├── 协程 yield（等 IO）→ 回到 epoll_wait
        │
        └── 协程 DEAD → 清理连接
```

---

## 七、协程基础演示

### 7.1 三个协程交替执行

```c
static void co_a(void *arg)
{
    coroutine_t *self = g_sched->coroutines[0];
    for (int i = 1; i <= 3; i++) {
        printf("协程 A: 第 %d 次执行\n", i);
        co_yield(self);
    }
    printf("协程 A: 结束\n");
}

// co_b, co_c 类似

int main(void)
{
    scheduler_t *sched = sched_create();
    sched_add(sched, co_a, NULL);
    sched_add(sched, co_b, NULL);
    sched_add(sched, co_c, NULL);
    sched_run(sched);
}
```

### 7.2 输出

```
协程 A: 第 1 次执行
协程 B: 第 1 次执行
协程 C: 第 1 次执行
协程 A: 第 2 次执行
协程 B: 第 2 次执行
协程 C: 第 2 次执行
协程 A: 第 3 次执行
协程 B: 第 3 次执行
协程 C: 第 3 次执行
协程 A: 结束
协程 B: 结束
协程 C: 结束
```

三个协程**交替执行**，每次 `yield` 后切到下一个协程。
这就是协作式调度：每个协程主动 `yield` 让出 CPU。

---

## 八、上下文切换的代价

### 8.1 ucontext 切换

`swapcontext` 做的事：

```
1. 保存当前寄存器（约 8 个 callee-saved 寄存器）
   - 8 次 mov 指令
2. 保存信号掩码（sigprocmask 系统调用）
3. 恢复目标寄存器
   - 8 次 mov 指令
4. 恢复信号掩码（sigprocmask 系统调用）
```

每次切换约 **100-200 ns**（含 2 次 sigprocmask 系统调用）。

### 8.2 vs 线程切换

| 操作           | 线程切换     | ucontext 切换 |
| -------------- | ------------ | -------------- |
| 保存寄存器     | 内核态       | 用户态         |
| 切换栈         | 内核态       | 用户态         |
| 信号掩码       | 内核态       | 内核态         |
| 调度策略       | 内核决定     | 用户决定       |
| 总开销         | ~1-5 μs      | ~100-200 ns    |
| 系统调用次数   | 1+           | 2 (sigprocmask)|

协程切换比线程切换快 **5-50 倍**。

### 8.3 更快的切换

如果去掉信号掩码操作（用汇编直接保存/恢复寄存器），切换可以降到 **20-30 ns**：

```x86asm
; 协程切换（简化版，不处理信号）
switch:
    push rbp        ; 保存 callee-saved 寄存器
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp  ; 保存当前栈指针到 old->ctx
    mov rsp, [rsi]  ; 从 new->ctx 恢复栈指针
    pop r15         ; 恢复 callee-saved 寄存器
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret             ; 返回到新协程的 rip
```

这就是 Boost.Context、libco 等库的做法。
本实现用 ucontext（教学简化），生产环境可以用汇编版。

---

## 九、协程的内存模型

### 9.1 每个协程独立栈

```
堆内存:
┌─────────────────────────────────┐
│ co->stack (64KB)                │ ← 协程 A 的栈
│   ┌───────────────────┐         │
│   │ echo_routine()    │         │
│   │   buf[4096]       │         │ ← 协程 A 的局部变量
│   │   n               │         │
│   │   ...             │         │
│   └───────────────────┘         │
├─────────────────────────────────┤
│ co->stack (64KB)                │ ← 协程 B 的栈
│   ┌───────────────────┐         │
│   │ echo_routine()    │         │
│   │   buf[4096]       │         │ ← 协程 B 的局部变量
│   │   ...             │         │
│   └───────────────────┘         │
└─────────────────────────────────┘
```

每个协程有 64KB 独立栈，局部变量互不干扰。
1 万个协程 = 640MB 栈内存（可以调小栈来节省）。

### 9.2 共享堆

所有协程在同一个线程内，**共享堆内存**：

```c
/* 所有协程可以安全访问共享数据，不需要锁！ */
static int g_counter = 0;

void co_func(void *arg) {
    g_counter++;  /* 安全：单线程内，不会被抢占 */
    co_yield();
    g_counter++;  /* 仍然安全 */
}
```

这是协程相对于线程的一大优势：**单线程内不需要锁**。

### 9.3 栈大小选择

| 栈大小  | 1万协程内存 | 适用场景           |
| ------- | ----------- | ------------------ |
| 8KB     | 80MB        | 简单逻辑（echo）   |
| 32KB    | 320MB       | 一般业务           |
| 64KB    | 640MB       | 复杂逻辑（本实现） |
| 128KB   | 1.28GB      | 深层调用栈         |

Go 协程默认 8KB（可动态增长），libco 默认 128KB。
本实现用固定 64KB，够用且简单。

---

## 十、协程 vs 其他异步方案

### 10.1 回调（Callback）

```c
/* 回调 */
read_async(fd, [](int n) {
    process(buf);
    write_async(fd, buf, n, [](int n) {
        read_async(fd, ...);  // 回调地狱
    });
});

/* 协程 */
int n = co_read(fd, buf);
process(buf);
co_write(fd, buf, n);  // 线性代码
```

### 10.2 Promise / Future

```c
/* Promise */
read_async(fd).then([](int n) {
    process(buf);
    return write_async(fd, buf, n);
}).then([](int n) {
    return read_async(fd, ...);
});

/* 协程 */
int n = co_read(fd, buf);
process(buf);
co_write(fd, buf, n);
```

### 10.3 async/await（C20 协程）

```c
/* C20 协程 */
task<void> echo() {
    int n = co_await read_async(fd);
    co_await write_async(fd, buf, n);
}

/* 本实现（C99 协程）*/
int n = co_read(fd, buf);   // 内部 co_yield
co_write(fd, buf, n);
```

C20 的 `co_await` 和我们的 `co_read` 本质一样：
都是"等 IO 时挂起，IO 就绪后恢复"。

### 10.4 对比总结

| 方案             | 代码风格   | 状态管理 | 错误处理 | C 版本 |
| ---------------- | ---------- | -------- | -------- | ------ |
| 回调             | 碎片化     | 手动     | 困难     | C89+   |
| Promise          | 链式       | 半自动   | 较好     | C++11  |
| async/await      | 线性       | 自动     | 简单     | C++20  |
| ucontext 协程    | 线性       | 自动     | 简单     | C99    |
| 汇编协程         | 线性       | 自动     | 简单     | 任意   |

---

## 十一、测试验证

### 11.1 协程演示

```bash
build/bin/co_demo
```

输出：
```
协程 A: 第 1 次执行
协程 B: 第 1 次执行
协程 C: 第 1 次执行
协程 A: 第 2 次执行
...
协程 C: 结束
所有协程执行完毕
```

### 11.2 echo 服务器

```bash
build/bin/co_echo_server 8080

# 单连接
echo "hello coroutine" | nc localhost 8080
# 输出: hello coroutine

# 多消息
(echo "msg1"; sleep 0.2; echo "msg2") | nc localhost 8080
# 输出: msg1\nmsg2

# 并发连接
for i in 1 2 3 4 5; do
    echo "conn$i" | nc localhost 8080 &
done
wait
```

### 11.3 测试结果

```
=== 测试 1: 单连接 echo ===
hello coroutine

=== 测试 2: 多消息 ===
msg1
msg2
msg3

=== 测试 3: 并发连接 ===
conn1
conn2
conn3
conn4
conn5

=== 所有测试完成 ===
```

---

## 十二、协程的局限

### 12.1 不支持多核

本实现的协程在**单线程**内运行，无法利用多核。

要利用多核：
- **多线程 + 协程**：每个线程一个协程调度器（类似 Go 的 GMP 模型）
- **多进程 + 协程**：每个进程一个协程调度器（类似 Nginx）

### 12.2 一个协程阻塞会阻塞所有协程

```c
void co_func(void *arg) {
    /* 阻塞系统调用！所有协程都被阻塞 */
    int n = read(fd, buf, sizeof(buf));  // 阻塞 read
}
```

因为协程在单线程内，一个协程调用了阻塞系统调用，整个线程被阻塞。
所以必须用 `co_read`（非阻塞 + yield），不能用阻塞 `read`。

### 12.3 栈溢出

每个协程的栈是固定大小（64KB），如果递归太深或局部变量太大，会栈溢出。

```c
void co_func(void *arg) {
    char big[100 * 1024];  // 100KB，超过 64KB 栈！
    // 栈溢出，段错误
}
```

Go 协程用**动态增长栈**解决这个问题，但实现复杂。
本实现用固定栈，需要程序员注意栈使用量。

### 12.4 不支持抢占

一个协程如果不 `yield`，其他协程永远得不到执行机会：

```c
void co_func(void *arg) {
    while (1) {
        // 死循环，不 yield
        // 其他协程饿死
    }
}
```

Go 协程在函数调用点插入抢占检查，本实现不支持。

---

## 十三、生产级协程库

### 13.1 libco（微信后台）

腾讯微信后台使用的协程库：
- 用汇编实现上下文切换（比 ucontext 快 5-10 倍）
- 支持共享栈（多个协程共用一个栈，节省内存）
- 配合 epoll 实现网络协程

### 13.2 Boost.Context（C++）

C++ 的高性能协程库：
- 汇编实现，支持 x86/x86_64/ARM/ARM64
- 提供 `fiber`（协程）和 `continuation`（续延）
- 是 C++20 `co_await` 的底层实现

### 13.3 Go goroutine

Go 语言的协程（goroutine）：
- **动态增长栈**：从 2KB 开始，按需增长到 1GB
- **抢占式调度**：在函数调用点检查抢占标志
- **多核支持**：GMP 模型，多个线程跑协程
- **工作窃取**：空闲线程从其他线程偷协程

### 13.4 对比

| 特性         | 本实现    | libco     | Boost     | Go        |
| ------------ | --------- | --------- | --------- | --------- |
| 切换实现     | ucontext  | 汇编      | 汇编      | 汇编      |
| 切换开销     | ~200ns    | ~30ns     | ~20ns     | ~100ns    |
| 栈大小       | 固定 64KB | 固定/共享 | 固定      | 动态 2KB+ |
| 多核         | 否        | 否        | 否        | 是        |
| 抢占         | 否        | 否        | 否        | 是        |
| 语言         | C99       | C         | C++       | Go        |

---

## 十四、代码结构

### 14.1 文件清单

```
stage13_coroutine/
├── coroutine.h         # 协程库头文件
├── coroutine.c         # 协程库实现（ucontext）
├── co_demo.c           # 基础演示（三协程交替）
├── co_echo_server.c    # 协程版 echo 服务器
├── test_co_echo.sh     # 测试脚本
└── CMakeLists.txt      # 构建配置
```

### 14.2 构建配置

```cmake
# 协程库
add_library(coroutine STATIC coroutine.c)

# 基础演示
add_executable(co_demo co_demo.c)
target_link_libraries(co_demo coroutine common_v2)

# 协程版 echo 服务器
add_executable(co_echo_server co_echo_server.c)
target_link_libraries(co_echo_server coroutine common_v2)
```

---

## 十五、协程实现的核心代码

### 15.1 完整协程库

```c
/* coroutine.h */
#ifndef COROUTINE_H
#define COROUTINE_H

#include <ucontext.h>

typedef enum {
    CO_READY, CO_RUNNING, CO_SUSPENDED, CO_DEAD,
} co_state_t;

typedef struct coroutine {
    ucontext_t   ctx;
    ucontext_t  *caller;
    co_state_t   state;
    void       (*func)(void *);
    void        *arg;
    char        *stack;
    int          id;
} coroutine_t;

coroutine_t *co_create(void (*func)(void *), void *arg);
void         co_resume(coroutine_t *co);
void         co_yield(coroutine_t *co);
co_state_t   co_state(coroutine_t *co);
void         co_free(coroutine_t *co);

/* 调度器 */
typedef struct scheduler {
    coroutine_t *coroutines[4096];
    int count, current;
    ucontext_t main_ctx;
} scheduler_t;

scheduler_t *sched_create(void);
int  sched_add(scheduler_t *, void (*func)(void *), void *arg);
void sched_run(scheduler_t *);
void sched_free(scheduler_t *);

extern scheduler_t *g_sched;
#endif
```

### 15.2 核心实现

```c
/* coroutine.c */
#define CO_STACK_SIZE (64 * 1024)
scheduler_t *g_sched = NULL;
static coroutine_t *g_current_co = NULL;

static void co_entry(void) {
    coroutine_t *co = g_current_co;
    co->func(co->arg);
    co->state = CO_DEAD;
    swapcontext(&co->ctx, co->caller);
}

coroutine_t *co_create(void (*func)(void *), void *arg) {
    static int next_id = 0;
    coroutine_t *co = calloc(1, sizeof(coroutine_t));
    co->func = func;
    co->arg = arg;
    co->state = CO_READY;
    co->id = next_id++;
    co->stack = malloc(CO_STACK_SIZE);

    getcontext(&co->ctx);
    co->ctx.uc_stack.ss_sp = co->stack;
    co->ctx.uc_stack.ss_size = CO_STACK_SIZE;
    makecontext(&co->ctx, co_entry, 0);
    return co;
}

void co_resume(coroutine_t *co) {
    if (co->state == CO_DEAD) return;
    coroutine_t *prev = g_current_co;
    g_current_co = co;
    co->state = CO_RUNNING;

    ucontext_t caller_ctx;
    co->caller = &caller_ctx;
    swapcontext(&caller_ctx, &co->ctx);

    g_current_co = prev;
}

void co_yield(coroutine_t *co) {
    if (!co || co->state != CO_RUNNING) return;
    co->state = CO_SUSPENDED;
    swapcontext(&co->ctx, co->caller);
}
```

### 15.3 协程版 echo

```c
/* co_echo_server.c（核心部分）*/

static int co_read(coroutine_t *co, int fd, char *buf, int size) {
    for (;;) {
        int n = read(fd, buf, size);
        if (n > 0) return n;
        if (n == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_yield(co);  /* 没数据，挂起 */
            continue;
        }
        return -1;
    }
}

static void echo_routine(void *arg) {
    co_conn_t *conn = arg;
    char buf[4096];
    for (;;) {
        int n = co_read(conn->co, conn->fd, buf, sizeof(buf));
        if (n <= 0) break;
        co_write(conn->co, conn->fd, buf, n);
    }
}

/* 主循环 */
for (;;) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; i++) {
        if (events[i].data.fd == listen_fd) {
            /* 新连接 → 创建协程 */
            int fd = accept(listen_fd, NULL, NULL);
            co_conn_t *conn = calloc(1, sizeof(co_conn_t));
            conn->fd = fd;
            conn->co = co_create(echo_routine, conn);
            epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
        } else {
            /* 数据到达 → 恢复协程 */
            co_resume(g_conns[fd]->co);
        }
    }
}
```

---

## 十六、从协程到下一步

### 16.1 回顾

本阶段实现了：
- ucontext 协程库（create / resume / yield / free）
- 协程调度器（轮转调度）
- 协程版 IO（co_read / co_write，IO 不就绪时自动 yield）
- 协程版 echo 服务器（线性代码实现异步 IO）
- 基础演示（三协程交替执行）

### 16.2 协程的价值

协程的最大价值：**用同步的写法写异步代码**。

```c
/* 这段代码看起来是阻塞的 */
for (;;) {
    int n = co_read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    process(buf);
    co_write(fd, buf, n);
}

/* 但实际是异步的：
   - co_read 没数据时 yield，不阻塞线程
   - co_write 写不完时 yield，不阻塞线程
   - 其他协程在 yield 期间可以运行
*/
```

这就是 Go 语言 `goroutine` + `channel` 的核心思想，
也是 C++20 `co_await` 的核心思想。
我们用 C99 + ucontext 实现了同样的效果。

### 16.3 下一步

协程解决了"代码风格"问题（同步写法），但 HTTP 协议本身还有性能瓶颈：
**文本协议、串行请求、头部不压缩**。

HTTP/2 解决这些问题：
- **二进制帧**：比文本解析更快
- **多路复用**：一条 TCP 上并行多个请求
- **头部压缩**：HPACK 压缩重复头部
- **服务器推送**：主动推送资源

下一阶段（stage14）我们将实现 **HTTP/2** 的核心特性。

---

## 附录：ucontext 函数签名

```c
#include <ucontext.h>

/* 获取当前上下文 */
int getcontext(ucontext_t *ucp);

/* 创建上下文 */
void makecontext(ucontext_t *ucp,
                 void (*func)(void),
                 int argc, ...);

/* 切换上下文 */
int swapcontext(ucontext_t *oucp,
                const ucontext_t *ucp);

/* 设置上下文（不保存当前） */
int setcontext(const ucontext_t *ucp);
```

## 附录：ucontext_t 结构

```c
typedef struct ucontext {
    unsigned long     uc_flags;
    ucontext_t       *uc_link;      /* 后继上下文 */
    stack_t           uc_stack;     /* 栈 */
    mcontext_t        uc_mcontext;  /* 机器寄存器 */
    sigset_t          uc_sigmask;   /* 信号掩码 */
} ucontext_t;

typedef struct {
    void  *ss_sp;     /* 栈基址 */
    size_t ss_size;   /* 栈大小 */
    int    ss_flags;  /* 标志 */
} stack_t;
```

## 附录：协程状态机

```
                 co_create()
                      │
                      ▼
                  CO_READY
                      │
              co_resume() │
                      ▼
              ┌──→ CO_RUNNING
              │       │
              │       │ co_yield()
              │       ▼
              │   CO_SUSPENDED
              │       │
              └───────┘ co_resume()
                      │
                      │ func() returns
                      ▼
                   CO_DEAD
                      │
              co_free()│
                      ▼
                   [释放]
```