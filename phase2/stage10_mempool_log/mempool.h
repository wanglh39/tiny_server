#ifndef MEMPOOL_H
#define MEMPOOL_H

/*
 * mempool.h —— 内存池
 *
 * 设计参考 Nginx 的 ngx_pool_t
 *
 * 为什么不用 malloc？
 *   1. 碎片：频繁 malloc/free 产生内存碎片，浪费空间
 *   2. 缓存局部性：malloc 分配的内存分散，cache miss 多
 *   3. 系统调用：大块分配时 malloc 可能调 brk/mmap
 *   4. 生命周期：HTTP 请求来了分配一堆，请求结束全释放
 *      → 整池释放比逐个 free 快得多
 *
 * 内存池模型：
 *   ┌──────────────────────────┐
 *   │  pool                    │
 *   │  ├── block 1 (4KB)       │  ← 小分配从这里切
 *   │  │   [used][used][free]  │
 *   │  ├── block 2 (4KB)       │  ← block 1 不够时开新 block
 *   │  │   [used][free]        │
 *   │  ├── large 1 (16KB)      │  ← 大分配独立 block
 *   │  └── large 2 (32KB)      │
 *   └──────────────────────────┘
 *
 *   pool_alloc(pool, 100)  → 从 block 切 100 字节
 *   pool_alloc(pool, 8000) → 太大，独立 malloc 一块
 *   pool_destroy(pool)     → 释放所有 block
 */

#include <stddef.h>

#define POOL_DEFAULT_SIZE  4096   /* 默认 block 大小 */
#define POOL_MAX_ALLOC     1024   /* 超过这个大小走大分配 */

typedef struct pool_block  pool_block_t;
typedef struct pool_large  pool_large_t;
typedef struct pool        pool_t;

/* 小块内存的 block（链表） */
struct pool_block {
    pool_block_t *next;       /* 下一个 block */
    char         *last;       /* 当前 block 可用位置 */
    char         *end;        /* 当前 block 结束位置 */
    char         *start;      /* 数据区起始位置 */
};

/* 大块内存（独立 malloc，链表） */
struct pool_large {
    pool_large_t *next;       /* 下一个大块 */
    void         *data;       /* 实际数据指针 */
};

/* 内存池 */
struct pool {
    pool_block_t  block;      /* 第一个 block（内联，减少一次 malloc） */
    pool_block_t *current;    /* 当前用于分配的 block */
    pool_large_t *large;      /* 大块链表头 */
    size_t        block_size; /* 每个 block 的大小 */
};

/*
 * pool_create —— 创建内存池
 *   block_size: 每个 block 的大小（0 = 默认 4KB）
 */
pool_t *pool_create(size_t block_size);

/*
 * pool_destroy —— 销毁内存池，释放所有内存
 */
void pool_destroy(pool_t *pool);

/*
 * pool_alloc —— 从池中分配内存
 *   小分配：从 block 切
 *   大分配：独立 malloc
 */
void *pool_alloc(pool_t *pool, size_t size);

/*
 * pool_calloc —— 分配并清零
 */
void *pool_calloc(pool_t *pool, size_t size);

/*
 * pool_reset —— 重置池（释放所有 block 的内容，但保留 block 结构）
 * 用于请求结束后重用池
 */
void pool_reset(pool_t *pool);

/*
 * pool_stats —— 统计信息
 */
typedef struct {
    size_t total_allocated;  /* 总分配量 */
    size_t small_blocks;     /* 小块 block 数 */
    size_t large_blocks;     /* 大块数 */
    size_t small_used;       /* 小块已用字节 */
    size_t small_capacity;   /* 小块总容量 */
} pool_stats_t;

void pool_stats(pool_t *pool, pool_stats_t *stats);

#endif /* MEMPOOL_H */