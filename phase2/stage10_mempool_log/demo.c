/*
 * demo.c —— 内存池 + 异步日志 演示
 *
 * 运行：
 *   build/bin/mempool_log_demo
 *
 * 演示内容：
 *   1. 内存池分配/释放（对比 malloc）
 *   2. 异步日志高频写入（不阻塞主线程）
 *   3. 多线程并发写日志
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "mempool.h"
#include "async_log.h"

/* ---------- 内存池演示 ---------- */
static void demo_mempool(void)
{
    printf("=== 内存池演示 ===\n\n");

    /* 创建池：4KB block */
    pool_t *pool = pool_create(4096);
    if (!pool) {
        printf("pool_create 失败\n");
        return;
    }

    /* 小分配：从 block 切 */
    char *p1 = pool_alloc(pool, 100);
    char *p2 = pool_alloc(pool, 200);
    char *p3 = pool_alloc(pool, 300);
    strcpy(p1, "hello from p1");
    strcpy(p2, "hello from p2");
    strcpy(p3, "hello from p3");

    printf("小分配:\n");
    printf("  p1 = %p → \"%s\" (100 字节)\n", p1, p1);
    printf("  p2 = %p → \"%s\" (200 字节)\n", p2, p2);
    printf("  p3 = %p → \"%s\" (300 字节)\n", p3, p3);

    /* 大分配：独立 malloc */
    char *big = pool_alloc(pool, 8192);
    memset(big, 'X', 8192);
    printf("  big = %p (8192 字节，大分配)\n", big);

    /* 统计 */
    pool_stats_t stats;
    pool_stats(pool, &stats);
    printf("\n内存池统计:\n");
    printf("  小块 block 数:  %zu\n", stats.small_blocks);
    printf("  大块数:         %zu\n", stats.large_blocks);
    printf("  小块已用:       %zu 字节\n", stats.small_used);
    printf("  小块总容量:     %zu 字节\n", stats.small_capacity);
    printf("  利用率:         %.1f%%\n",
           stats.small_capacity > 0 ?
           100.0 * stats.small_used / stats.small_capacity : 0);

    /* 模拟 HTTP 请求处理：分配 → 使用 → 重置 */
    printf("\n模拟 1000 次 HTTP 请求（每次分配 500 字节）:\n");
    clock_t start = clock();
    for (int i = 0; i < 1000; i++) {
        pool_reset(pool);
        for (int j = 0; j < 10; j++) {
            char *p = pool_alloc(pool, 50);
            memset(p, 'A', 50);
        }
    }
    clock_t end = clock();
    printf("  内存池耗时: %.3f ms\n", 1000.0 * (end - start) / CLOCKS_PER_SEC);

    /* 对比 malloc */
    start = clock();
    for (int i = 0; i < 1000; i++) {
        char *ptrs[10];
        for (int j = 0; j < 10; j++) {
            ptrs[j] = malloc(50);
            memset(ptrs[j], 'A', 50);
        }
        for (int j = 0; j < 10; j++) {
            free(ptrs[j]);
        }
    }
    end = clock();
    printf("  malloc 耗时: %.3f ms\n", 1000.0 * (end - start) / CLOCKS_PER_SEC);
    printf("  （内存池省掉了 10000 次 free 调用）\n");

    pool_destroy(pool);
    printf("\n");
}

/* ---------- 异步日志演示 ---------- */
#define NUM_THREADS  4
#define LOGS_PER_THREAD 100000

static void *log_thread(void *arg)
{
    int tid = *(int *)arg;
    for (int i = 0; i < LOGS_PER_THREAD; i++) {
        alog_info("thread %d: log message %d, some data here", tid, i);
    }
    return NULL;
}

static void demo_async_log(void)
{
    printf("=== 异步日志演示 ===\n\n");

    /* 初始化异步日志 */
    if (async_log_init("demo.log", LOG_LEVEL_INFO) < 0) {
        printf("async_log_init 失败\n");
        return;
    }

    printf("写入 %d 条日志（%d 线程 × %d 条/线程）...\n",
           NUM_THREADS * LOGS_PER_THREAD, NUM_THREADS, LOGS_PER_THREAD);

    /* 多线程并发写日志 */
    pthread_t threads[NUM_THREADS];
    int tids[NUM_THREADS];

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_THREADS; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, log_thread, &tids[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1000000.0;

    printf("耗时: %.1f ms\n", elapsed);
    printf("吞吐: %.0f 条/秒\n",
           NUM_THREADS * LOGS_PER_THREAD / elapsed * 1000);

    /* 停止，刷出剩余日志 */
    async_log_stop();

    printf("日志已写入 demo.log\n\n");
}

/* ---------- main ---------- */
int main(void)
{
    printf("stage10: 内存池 + 异步日志\n");
    printf("==========================\n\n");

    demo_mempool();
    demo_async_log();

    printf("完成。\n");
    return 0;
}