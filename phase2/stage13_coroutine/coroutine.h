/*
 * coroutine.h —— 阶段 13：协程库（ucontext 实现）
 *
 * ============================================================
 * 协程（Coroutine）是用户态的"轻量级线程"。
 * 和线程的区别：
 *   - 线程：内核调度，抢占式，任意时刻可能被切走
 *   - 协程：用户态调度，协作式，只在 yield 时切换
 *
 * 优势：
 *   - 用同步的写法写异步代码（没有回调地狱）
 *   - 切换开销极小（只需保存/恢复寄存器，无系统调用）
 *   - 每个协程独立栈，但都在一个线程内
 *
 * ucontext API:
 *   getcontext(&ctx)        —— 保存当前上下文到 ctx
 *   makecontext(&ctx, fn, n, args...) —— 设置 ctx 的入口函数
 *   swapcontext(&old, &new) —— 保存当前到 old，切换到 new
 *
 * 用法：
 *   coroutine_t *co = co_create(my_func, arg);
 *   co_resume(co);   // 运行到 yield 或 return
 *   co_resume(co);   // 从 yield 处继续
 *   co_free(co);
 * ============================================================
 */

#ifndef COROUTINE_H
#define COROUTINE_H

#include <ucontext.h>

/* ---------- 协程状态 ---------- */
typedef enum {
    CO_READY,      /* 已创建，未运行 */
    CO_RUNNING,    /* 正在运行 */
    CO_SUSPENDED,  /* 已 yield，等待恢复 */
    CO_DEAD,       /* 已结束（函数 return） */
} co_state_t;

/* ---------- 协程结构体 ---------- */
typedef struct coroutine {
    ucontext_t   ctx;          /* 协程上下文（寄存器、栈指针等） */
    ucontext_t  *caller;       /* 调用者上下文（resume 后回到这里） */
    co_state_t   state;        /* 协程状态 */
    void       (*func)(void *); /* 协程入口函数 */
    void        *arg;          /* 传递给入口函数的参数 */
    char        *stack;        /* 协程独立栈 */
    int          id;           /* 协程 ID（调试用） */
} coroutine_t;

/* ---------- API ---------- */

/*
 * co_create —— 创建协程
 *   func: 协程入口函数
 *   arg:  传递给 func 的参数
 * 返回: 协程指针
 */
coroutine_t *co_create(void (*func)(void *), void *arg);

/*
 * co_resume —— 恢复协程（从 yield 处或开头继续运行）
 *   协程会运行到下次 yield 或 return
 */
void co_resume(coroutine_t *co);

/*
 * co_yield —— 挂起当前协程，回到调用者
 *   只能在协程函数内部调用
 */
void co_yield(coroutine_t *co);

/*
 * co_state —— 获取协程状态
 */
co_state_t co_state(coroutine_t *co);

/*
 * co_free —— 释放协程资源
 */
void co_free(coroutine_t *co);

/* ---------- 协程调度器 ---------- */

#define MAX_COROUTINES 4096

typedef struct scheduler {
    coroutine_t *coroutines[MAX_COROUTINES];
    int          count;
    int          current;   /* 当前运行的协程索引 */
    ucontext_t   main_ctx;  /* 调度器主上下文 */
} scheduler_t;

/*
 * sched_create —— 创建调度器
 */
scheduler_t *sched_create(void);

/*
 * sched_add —— 添加协程到调度器
 *   返回协程 ID
 */
int sched_add(scheduler_t *sched, void (*func)(void *), void *arg);

/*
 * sched_run —— 运行所有协程直到完成
 */
void sched_run(scheduler_t *sched);

/*
 * sched_free —— 释放调度器
 */
void sched_free(scheduler_t *sched);

/*
 * sched_current —— 获取当前调度器（全局变量，协程内部用）
 */
extern scheduler_t *g_sched;

#endif /* COROUTINE_H */