/*
 * mempool.c —— 内存池实现
 */

#include "mempool.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 对齐到 8 字节（64 位系统指针/long 对齐） */
#define ALIGN(d, a) (((d) + (a) - 1) & ~((a) - 1))
#define ALIGN_PTR(p, a) (char *)ALIGN((unsigned long)(p), (a))

pool_t *pool_create(size_t block_size)
{
    if (block_size == 0) block_size = POOL_DEFAULT_SIZE;

    /* 分配池结构 + 第一个 block 的数据空间 */
    size_t total = sizeof(pool_t) + block_size;
    pool_t *pool = (pool_t *)malloc(total);
    if (!pool) return NULL;

    /* 第一个 block 紧跟在 pool 结构后面 */
    pool->block.next  = NULL;
    pool->block.start = (char *)pool + sizeof(pool_t);
    pool->block.last  = (char *)pool + sizeof(pool_t);
    pool->block.end   = (char *)pool + total;

    pool->current     = &pool->block;
    pool->large       = NULL;
    pool->block_size  = block_size;

    return pool;
}

void pool_destroy(pool_t *pool)
{
    if (!pool) return;

    /* 释放大块 */
    pool_large_t *large = pool->large;
    while (large) {
        pool_large_t *next = large->next;
        free(large->data);
        free(large);
        large = next;
    }

    /* 释放小块 block（第一个 block 内联在 pool 里，不用单独 free） */
    pool_block_t *block = pool->block.next;
    while (block) {
        pool_block_t *next = block->next;
        free(block);
        block = next;
    }

    /* 释放 pool 本身（包含第一个 block） */
    free(pool);
}

void *pool_alloc(pool_t *pool, size_t size)
{
    if (!pool) return NULL;

    /* 大分配：独立 malloc */
    if (size > pool->block_size || size > POOL_MAX_ALLOC) {
        pool_large_t *large = (pool_large_t *)malloc(sizeof(pool_large_t));
        if (!large) return NULL;

        large->data = malloc(size);
        if (!large->data) {
            free(large);
            return NULL;
        }

        large->next = pool->large;
        pool->large = large;

        return large->data;
    }

    /* 小分配：从 current block 切 */
    pool_block_t *block = pool->current;
    for (;;) {
        char *p = ALIGN_PTR(block->last, 8);
        char *new_last = p + size;

        if (new_last <= block->end) {
            /* 当前 block 够用 */
            block->last = new_last;
            return p;
        }

        /* 当前 block 不够，试下一个 */
        if (block->next) {
            block = block->next;
        } else {
            /* 开新 block */
            size_t bs = pool->block_size;
            pool_block_t *new_block = (pool_block_t *)malloc(sizeof(pool_block_t) + bs);
            if (!new_block) return NULL;

            new_block->next  = NULL;
            new_block->start = (char *)new_block + sizeof(pool_block_t);
            new_block->last  = (char *)new_block + sizeof(pool_block_t);
            new_block->end   = (char *)new_block + sizeof(pool_block_t) + bs;

            block->next = new_block;
            block = new_block;

            /* 更新 current（跳过已满的 block） */
            pool->current = new_block;
        }
    }
}

void *pool_calloc(pool_t *pool, size_t size)
{
    void *p = pool_alloc(pool, size);
    if (p) memset(p, 0, size);
    return p;
}

void pool_reset(pool_t *pool)
{
    /* 重置所有 block 的 last 指针 */
    pool_block_t *block = &pool->block;
    while (block) {
        block->last = block->start;
        block = block->next;
    }
    pool->current = &pool->block;

    /* 释放大块 */
    pool_large_t *large = pool->large;
    while (large) {
        pool_large_t *next = large->next;
        free(large->data);
        free(large);
        large = next;
    }
    pool->large = NULL;
}

void pool_stats(pool_t *pool, pool_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));

    /* 小块统计 */
    pool_block_t *block = &pool->block;
    while (block) {
        stats->small_blocks++;
        stats->small_capacity += block->end - block->start;
        stats->small_used += block->last - block->start;
        block = block->next;
    }

    /* 大块统计 */
    pool_large_t *large = pool->large;
    while (large) {
        stats->large_blocks++;
        large = large->next;
    }

    stats->total_allocated = stats->small_used;
}