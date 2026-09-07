/*
 * coroutine.c —— 协程库实现（ucontext）
 *
 * ============================================================
 * 核心原理：
 *
 * ucontext_t 保存了 CPU 的"全部状态"：
 *   - 寄存器（rip, rsp, rbp, rbx, r12-r15 等）
 *   - 信号掩码
 *   - 栈指针
 *
 * swapcontext(&old, &new) 做的事：
 *   1. 把当前寄存器保存到 old
 *   2. 从 new 恢复寄存器
 *   3. CPU 从 new 的 rip 继续执行
 *
 * 协程切换流程：
 *   调度器 ──swapcontext──→ 协程函数
 *   协程函数 ──yield──→ swapcontext ──→ 调度器
 *   调度器 ──resume──→ swapcontext ──→ 协程函数（从 yield 后继续）
 *
 * 图示：
 *
 *   调度器          协程
 *     │               │
 *     │── resume ──→ │  co_func() {
 *     │               │    ...
 *     │               │    yield;  ←── 挂起
 *     │ ←── yield ── │    ...      ←── resume 后从这里继续
 *     │               │    return;  ←── 协程结束
 *     │ ←── done ─── │  }
 *     │               │
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#include "coroutine.h"
#include "log.h"

#define CO_STACK_SIZE (64 * 1024)  /* 每个协程 64KB 栈 */

/* 全局调度器指针（协程内部用 co_yield 时需要） */
scheduler_t *g_sched = NULL;

/* 当前正在运行的协程（全局，用于 co_yield） */
static coroutine_t *g_current_co = NULL;

/* ============================================================
 * 协程入口包装函数
 * ============================================================
 *
 * makecontext 要求入口函数签名是 void f(void)，
 * 但我们需要传 arg，所以用这个包装函数。
 *
 * 协程函数 return 后，我们标记为 CO_DEAD 并切回调度器。
 */
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

/* ============================================================
 * co_create —— 创建协程
 * ============================================================ */
coroutine_t *co_create(void (*func)(void *), void *arg)
{
    static int next_id = 0;

    coroutine_t *co = calloc(1, sizeof(coroutine_t));
    if (!co) return NULL;

    co->func  = func;
    co->arg   = arg;
    co->state = CO_READY;
    co->id    = next_id++;
    co->stack = malloc(CO_STACK_SIZE);
    if (!co->stack) {
        free(co);
        return NULL;
    }

    /* 初始化上下文 */
    getcontext(&co->ctx);
    co->ctx.uc_stack.ss_sp   = co->stack;
    co->ctx.uc_stack.ss_size = CO_STACK_SIZE;
    co->ctx.uc_link          = NULL;  /* 我们手动切回 */

    /* 设置入口函数 */
    makecontext(&co->ctx, co_entry, 0);

    return co;
}

/* ============================================================
 * co_resume —— 恢复协程
 * ============================================================ */
void co_resume(coroutine_t *co)
{
    if (co->state == CO_DEAD) return;

    /* 保存调度器上下文到 caller，切到协程 */
    coroutine_t *prev = g_current_co;
    g_current_co = co;
    co->state = CO_RUNNING;

    ucontext_t caller_ctx;
    co->caller = &caller_ctx;

    swapcontext(&caller_ctx, &co->ctx);

    /* 协程 yield 或 return 后回到这里 */
    g_current_co = prev;
}

/* ============================================================
 * co_yield —— 挂起当前协程
 * ============================================================ */
void co_yield(coroutine_t *co)
{
    if (!co || co->state != CO_RUNNING) return;

    co->state = CO_SUSPENDED;
    swapcontext(&co->ctx, co->caller);
}

/* ============================================================
 * co_state —— 获取协程状态
 * ============================================================ */
co_state_t co_state(coroutine_t *co)
{
    return co->state;
}

/* ============================================================
 * co_free —— 释放协程
 * ============================================================ */
void co_free(coroutine_t *co)
{
    if (!co) return;
    free(co->stack);
    free(co);
}

/* ============================================================
 * 调度器实现
 * ============================================================ */

scheduler_t *sched_create(void)
{
    scheduler_t *s = calloc(1, sizeof(scheduler_t));
    s->count   = 0;
    s->current = -1;
    return s;
}

int sched_add(scheduler_t *sched, void (*func)(void *), void *arg)
{
    if (sched->count >= MAX_COROUTINES) {
        log_error("协程数量超限");
        return -1;
    }
    int id = sched->count;
    sched->coroutines[id] = co_create(func, arg);
    sched->count++;
    return id;
}

/*
 * sched_run —— 轮转调度
 *
 * 简单策略：轮询所有协程，resume 每个非 DEAD 的协程。
 * 协程 yield 后回到这里，继续调度下一个。
 *
 * 真实场景中，调度器和 epoll 结合：
 *   - epoll 事件就绪 → resume 对应协程
 *   - 协程 yield（等 IO）→ 回到 epoll_wait
 */
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
            sched->current = i;
            co_resume(co);

            if (co->state == CO_DEAD) {
                log_debug("清理协程 %d", co->id);
                co_free(co);
                sched->coroutines[i] = NULL;
            }
        }
    }

    g_sched = NULL;
}

void sched_free(scheduler_t *sched)
{
    if (!sched) return;
    for (int i = 0; i < sched->count; i++) {
        if (sched->coroutines[i]) {
            co_free(sched->coroutines[i]);
        }
    }
    free(sched);
}