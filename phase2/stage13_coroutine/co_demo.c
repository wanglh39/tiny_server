/*
 * co_demo.c —— 协程基础演示
 *
 * 不涉及网络，纯演示协程切换原理。
 * 三个协程交替执行，展示 yield/resume 机制。
 *
 * 运行：build/bin/co_demo
 */

#include <stdio.h>
#include <unistd.h>
#include "coroutine.h"
#include "log.h"

/*
 * 协程 A：打印 1, 2, 3，每次打印后 yield
 */
static void co_a(void *arg)
{
    (void)arg;
    coroutine_t *self = g_sched->coroutines[0];

    for (int i = 1; i <= 3; i++) {
        printf("  协程 A: 第 %d 次执行\n", i);
        co_yield(self);
    }
    printf("  协程 A: 结束\n");
}

/*
 * 协程 B：打印 a, b, c，每次打印后 yield
 */
static void co_b(void *arg)
{
    (void)arg;
    coroutine_t *self = g_sched->coroutines[1];

    for (int i = 1; i <= 3; i++) {
        printf("  协程 B: 第 %d 次执行\n", i);
        co_yield(self);
    }
    printf("  协程 B: 结束\n");
}

/*
 * 协程 C：打印 X, Y, Z，每次打印后 yield
 */
static void co_c(void *arg)
{
    (void)arg;
    coroutine_t *self = g_sched->coroutines[2];

    for (int i = 1; i <= 3; i++) {
        printf("  协程 C: 第 %d 次执行\n", i);
        co_yield(self);
    }
    printf("  协程 C: 结束\n");
}

int main(void)
{
    log_info("=== 协程基础演示 ===");
    printf("\n三个协程交替执行：\n\n");

    scheduler_t *sched = sched_create();

    sched_add(sched, co_a, NULL);
    sched_add(sched, co_b, NULL);
    sched_add(sched, co_c, NULL);

    sched_run(sched);

    printf("\n所有协程执行完毕\n");

    sched_free(sched);
    return 0;
}