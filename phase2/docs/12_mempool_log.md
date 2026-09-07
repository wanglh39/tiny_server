# stage10 - 内存池 + 异步日志

## 本章导读

高性能服务器有两个不起眼但至关重要的基础设施：

1. **内存池**——为什么不用 malloc？碎片、缓存局部性、生命周期管理
2. **异步日志**——为什么不能在 IO 线程直接 write？会阻塞 EventLoop

这两个东西看起来简单，但在百万 QPS 的场景下，它们决定了服务器的上限。

```bash
cmake --build build --target mempool_log_demo
build/bin/mempool_log_demo
```

---

## 一、内存池

### 1.1 为什么不用 malloc

malloc 看起来万能，但在高性能服务器里有四个问题：

| 问题 | 说明 | 后果 |
|------|------|------|
| **碎片** | 频繁 malloc/free 产生内存碎片 | 内存浪费，大分配失败 |
| **缓存局部性** | malloc 分散分配，数据不连续 | cache miss，性能下降 |
| **系统调用** | 大块分配时可能调 brk/mmap | 用户态/内核态切换 |
| **生命周期** | HTTP 请求要分配很多小块，逐个 free 慢 | O(n) 释放 |

### 1.2 Nginx 的解法

Nginx 每个请求创建一个内存池，请求结束时一把销毁：

```c
/* Nginx 风格 */
ngx_pool_t *pool = ngx_create_pool(4096, log);

/* 请求处理中：大量小分配 */
buf = ngx_palloc(pool, 100);
hdr = ngx_palloc(pool, 256);
uri = ngx_palloc(pool, 1024);

/* 请求结束：一把销毁，不用逐个 free */
ngx_destroy_pool(pool);
```

**核心思想**：不逐个 free，整池销毁。省掉了 n 次 free 调用。

### 1.3 内存池结构

```
pool_t
├── block 1 (4KB)           ← 第一个 block（内联在 pool 里）
│   [p1:100][p2:200][p3:300][free...]
├── block 2 (4KB)           ← block 1 不够时开新 block
│   [p4:500][free...]
├── large 1 (8KB)           ← 大分配独立 malloc
└── large 2 (16KB)          ← 大分配独立 malloc
```

- **小分配**（<= 1024 字节）：从 block 切一块，O(1)
- **大分配**（> 1024 字节）：独立 malloc，挂到 large 链表
- **整池销毁**：遍历 block 链表 + large 链表，逐个 free

### 1.4 分配过程

```c
void *pool_alloc(pool_t *pool, size_t size)
{
    if (size > POOL_MAX_ALLOC) {
        /* 大分配：独立 malloc */
        large = malloc(sizeof(pool_large_t));
        large->data = malloc(size);
        large->next = pool->large;
        pool->large = large;
        return large->data;
    }

    /* 小分配：从 current block 切 */
    block = pool->current;
    for (;;) {
        p = ALIGN(block->last, 8);       /* 8 字节对齐 */
        if (p + size <= block->end) {
            block->last = p + size;       /* 推进可用位置 */
            return p;
        }
        /* 当前 block 不够，开新 block */
        block = alloc_new_block(pool);
    }
}
```

**对齐**：每次分配对齐到 8 字节，保证后续访问的对齐要求。

### 1.5 为什么快

```
1000 次 HTTP 请求，每次 10 个 50 字节分配：

malloc 方式：
  1000 × 10 = 10000 次 malloc
  1000 × 10 = 10000 次 free
  总计 20000 次系统调用（或库调用）

内存池方式：
  1 次 malloc（创建池）
  1000 次 pool_reset（重置 last 指针，O(1)）
  1 次 free（销毁池）
  总计 2 次系统调用
```

实测：内存池 0.067ms vs malloc 0.170ms，**2.5 倍快**。

### 1.6 缓存局部性

```
malloc 分配（分散）:
  p1 = 0x7f1234, p2 = 0x7f5678, p3 = 0x7f9abc
  访问 p1 → cache miss
  访问 p2 → cache miss（不同 cache line）
  访问 p3 → cache miss

内存池分配（连续）:
  p1 = pool+64, p2 = pool+128, p3 = pool+192
  访问 p1 → cache miss
  访问 p2 → cache hit（同一 cache line 或相邻）
  访问 p3 → cache hit
```

连续内存 = 更好的缓存命中率 = 更快的访问。

### 1.7 pool_reset

```c
void pool_reset(pool_t *pool)
{
    /* 只重置 last 指针，不释放内存 */
    block = &pool->block;
    while (block) {
        block->last = block->start;  /* 回到起点 */
        block = block->next;
    }
    /* 释放大块 */
    free_all_large(pool);
}
```

reset 后池恢复到初始状态，但 block 的内存不还给系统——
下次分配直接复用，零系统调用。

---

## 二、异步日志

### 2.1 为什么不能在 IO 线程写日志

```c
/* 错误做法：在 EventLoop 里直接 write 日志 */
void on_request(int fd) {
    log_info("收到请求");  // ← 如果 write 阻塞了呢？
    handle_request(fd);
    log_info("处理完成");  // ← 又一次阻塞？
}
```

`write` 到文件通常很快，但：
- 磁盘忙时 write 会阻塞（等 IO 队列）
- NFS/网络文件系统 write 可能几百毫秒
- 高频日志（每请求 10 条）会让 IO 线程花大量时间在 write 上

**EventLoop 被阻塞 = 所有连接都卡住**。

### 2.2 双缓冲区方案

设计参考 muduo 的 AsyncLogging：

```
前端线程（IO 线程）            后端线程（日志线程）
┌──────────────────┐          ┌──────────────────┐
│  currentBuffer   │          │  写入文件         │
│  [日志1][日志2]... │          │                  │
└──────────────────┘          └──────────────────┘

currentBuffer 满了：
  1. currentBuffer → buffersToWrite 队列
  2. nextBuffer 变成新的 currentBuffer
  3. 唤醒后端线程

后端线程醒来：
  1. 交换 buffersToWrite 到本地（锁内，很快）
  2. 释放锁
  3. 逐个 write 到文件（锁外，不阻塞前端）
  4. 回收 buffer 复用
```

**关键**：前端和后端不会同时操作同一个 buffer。
前端写 currentBuffer，后端写文件里的数据——互不干扰。

### 2.3 为什么用双缓冲

```
单缓冲方案：
  前端写 buffer → 满了 → 唤醒后端 → 前端等后端写完
  ↑ 前端被阻塞了！

双缓冲方案：
  前端写 buffer A → 满了 → 交给后端 → 前端写 buffer B
  后端写 buffer A → 写完 → 还给前端做备用
  ↑ 前端永不阻塞！
```

双缓冲让前端永远不会因为日志而阻塞。

### 2.4 代码结构

```c
/* 前端数据（受 mutex 保护） */
current_buffer    // 前端正在写的 buffer（4MB）
next_buffer       // 备用空 buffer
buffers_to_write  // 满了的 buffer 队列

/* 后端线程 */
while (running) {
    wait(cond);                    // 等前端唤醒
    swap(buffers_to_write → local); // 锁内交换
    unlock();
    for (buf in local)             // 锁外写文件
        write(fd, buf);
    recycle(local → next_buffer);  // 回收复用
}
```

### 2.5 前端写入

```c
void async_log_write(level, fmt, ...)
{
    format_line(line);  // 格式化日志行

    lock(mutex);
    if (current_buffer 有空间) {
        append(current_buffer, line);
    } else {
        // current_buffer 满了
        buffers_to_write.push(current_buffer);
        current_buffer = next_buffer;  // 换备用
        next_buffer = NULL;
        append(current_buffer, line);
        signal(cond);                  // 唤醒后端
    }
    unlock(mutex);
}
```

**锁内只做三件事**：append、swap、signal。不做 IO。
锁持有时间极短（微秒级），不影响并发。

### 2.6 后端写入

```c
void *log_thread(void *arg)
{
    buffer *new1 = create(), *new2 = create();

    while (running) {
        lock(mutex);
        while (buffers_to_write 空 && running)
            wait(cond);

        // 搬运 buffers_to_write 到本地
        local = buffers_to_write;
        buffers_to_write = empty;

        // 如果 current_buffer 也有数据，一并搬走
        if (current_buffer->used > 0) {
            local.push(current_buffer);
            current_buffer = new1;  // 换上新 buffer
            new1 = NULL;
        }
        // 确保 next_buffer 有备用
        ...
        unlock(mutex);

        // 锁外：写文件
        for (buf in local)
            write(fd, buf);

        // 回收 buffer
        recycle(local → new1, new2);
    }
}
```

**锁内只做搬运**（指针交换），锁外做 IO。
这是高性能并发编程的经典模式：**锁内轻量操作，锁外重量级操作**。

### 2.7 buffer 回收

```c
/* 写完后回收 buffer */
if (num_buffers > 2) {
    // 太多了，只保留 2 个，其余释放
    for (i = 2; i < num; i++) free(buffers[i]);
    num = 2;
}

// 把 buffer 还给前端做备用
buffers[0]->used = 0;  // 清空但保留内存
if (!new1) new1 = buffers[0];
else if (!new2) new2 = buffers[0];
else free(buffers[0]);
```

回收策略：最多保留 2 个 buffer 做备用。
如果前端突然爆发写大量日志，创建的 buffer 不会无限增长。

### 2.8 性能数据

```
4 线程 × 100000 条日志 = 400000 条
耗时: 312 ms
吞吐: 1,280,240 条/秒
```

128 万条/秒！这是因为：
1. 前端写入只是 memcpy 到 buffer，极快
2. 后端批量 write，4MB 一次 write 系统调用
3. 双缓冲让前端永不阻塞

---

## 三、内存池代码解读

### 3.1 数据结构

```c
struct pool_block {
    pool_block_t *next;    // 链表
    char *last;            // 可用位置
    char *end;             // 结束位置
    char *start;           // 数据区起始
};

struct pool {
    pool_block_t  block;   // 第一个 block（内联）
    pool_block_t *current; // 当前分配用的 block
    pool_large_t *large;   // 大块链表
    size_t block_size;     // block 大小
};
```

第一个 block 内联在 pool 里，省一次 malloc。
`start` 字段记录数据区起始——第一个 block 的 start 和后续 block 不同。

### 3.2 创建

```c
pool_t *pool_create(size_t block_size)
{
    total = sizeof(pool_t) + block_size;
    pool = malloc(total);

    pool->block.start = (char*)pool + sizeof(pool_t);
    pool->block.last  = pool->block.start;
    pool->block.end   = (char*)pool + total;
    ...
}
```

一次 malloc 搞定 pool 结构 + 第一个 block 的数据区。

### 3.3 分配

```c
void *pool_alloc(pool_t *pool, size_t size)
{
    if (size > POOL_MAX_ALLOC) → 大分配

    block = pool->current;
    for (;;) {
        p = ALIGN(block->last, 8);
        if (p + size <= block->end) {
            block->last = p + size;
            return p;           // 切一块返回
        }
        block = block->next;    // 不够，试下一个
        if (!block) block = new_block(pool);  // 开新 block
    }
}
```

### 3.4 销毁

```c
void pool_destroy(pool_t *pool)
{
    // 释放大块
    for (large = pool->large; large; ...) {
        free(large->data);
        free(large);
    }
    // 释放小块 block（第一个不用 free，它内联在 pool 里）
    for (block = pool->block.next; block; ...) {
        free(block);
    }
    // 释放 pool 本身
    free(pool);
}
```

---

## 四、异步日志代码解读

### 4.1 buffer 结构

```c
typedef struct {
    char   data[4 * 1024 * 1024];  // 4MB 固定大小
    size_t used;
} log_buffer_t;
```

4MB 固定大小——大 enough 攒大量日志，小 enough 不会浪费内存。

### 4.2 全局状态

```c
typedef struct {
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    log_buffer_t    *current_buffer;   // 前端正在写
    log_buffer_t    *next_buffer;      // 备用
    log_buffer_t   **buffers_to_write; // 待写队列
    int num_to_write;
    pthread_t thread;                  // 后端线程
    int fd;                            // 日志文件
    int running;
} async_logger_t;
```

### 4.3 初始化

```c
int async_log_init(filename, level)
{
    fd = open(filename, O_WRONLY | O_CREAT | O_APPEND);
    current_buffer = create_buffer();
    next_buffer = create_buffer();
    buffers_to_write = malloc(16 * sizeof(buffer*));
    pthread_create(&thread, log_thread_func);
}
```

### 4.4 日志格式

```
2026-09-07 13:59:48.368 [INFO ] thread 0: log message 0, some data here
└──── 时间戳 ────┘ └级别┘ └── 消息体 ──────────────────────────┘
```

时间戳精确到毫秒，级别 5 字节对齐。

---

## 五、生产级改进

### 5.1 内存池改进

| 改进 | 说明 |
|------|------|
| 线程安全 | 加锁或 per-thread pool |
| 内存对齐 | 支持 16/32/64 字节对齐（SIMD） |
| 统计 | 分配次数、峰值使用量、碎片率 |
| 上限 | 限制池最大内存，防止 OOM |
| slab 分配 | 固定大小对象池（类似 Linux slab） |

### 5.2 异步日志改进

| 改进 | 说明 |
|------|------|
| 日志轮转 | 按大小/时间切文件（logrotate） |
| 日志压缩 | 后端线程写完压缩旧日志 |
| 多级别文件 | 不同级别写不同文件 |
| 结构化日志 | JSON 格式，方便 ELK 采集 |
| 采样 | 高频日志采样输出（如 1%） |
| backtrace | FATAL 级别自动 dump 调用栈 |
| 异步刷盘 | fsync 频率可控（性能 vs 可靠性） |

### 5.3 Nginx 的日志

Nginx 用单线程写日志（worker 内部），因为：
- Nginx 的日志频率不高（不是每请求都写）
- 用 `writev` 批量写减少系统调用
- 错误日志用全局锁，访问日志 per-worker 文件

### 5.4 muduo 的日志

muduo 的 AsyncLogging 就是本章参考的设计：
- 4MB 双缓冲区
- 前端线程安全
- 后端线程批量 write
- 线程数无关（多个前端线程共享一个后端）

---

## 六、运行与测试

### 6.1 运行 demo

```bash
build/bin/mempool_log_demo
```

输出：
```
=== 内存池演示 ===
小分配:
  p1 = 0x63b5458d12e8 → "hello from p1" (100 字节)
  p2 = 0x63b5458d1350 → "hello from p2" (200 字节)
  ...
内存池统计:
  小块 block 数:  1
  大块数:         1
  利用率:         14.7%

模拟 1000 次 HTTP 请求:
  内存池耗时: 0.067 ms
  malloc 耗时: 0.170 ms

=== 异步日志演示 ===
写入 400000 条日志...
耗时: 312.4 ms
吞吐: 1280240 条/秒
日志已写入 demo.log
```

### 6.2 验证日志文件

```bash
head -3 demo.log
# 2026-09-07 13:59:48.368 [INFO ] thread 0: log message 0, some data here
# 2026-09-07 13:59:48.369 [INFO ] thread 0: log message 1, some data here
# 2026-09-07 13:59:48.369 [INFO ] thread 0: log message 2, some data here

wc -l demo.log
# 400000 demo.log
```

---

## 七、在服务器中集成

### 7.1 内存池用于 HTTP 请求

```c
/* 每个请求创建一个池 */
void handle_connection(int fd) {
    pool_t *pool = pool_create(4096);

    /* 解析请求（从池分配） */
    http_request_t *req = pool_calloc(pool, sizeof(*req));
    req->method = pool_alloc(pool, 8);
    req->uri = pool_alloc(pool, 1024);
    req->headers = pool_alloc(pool, sizeof(header_t) * 32);

    /* 处理请求 */
    handle_request(fd, req);

    /* 请求结束，整池销毁 */
    pool_destroy(pool);
}
```

### 7.2 异步日志替代 printf

```c
/* 服务器启动时初始化 */
async_log_init("/var/log/server.log", LOG_LEVEL_INFO);

/* 各处用 alog 代替 printf */
alog_info("新连接 fd=%d from %s", fd, ip);
alog_error("请求处理失败: %s", strerror(errno));
alog_debug("路由匹配: %s → handler %p", uri, handler);

/* 服务器关闭时停止 */
async_log_stop();
```

---

## 八、本章总结

### 学到了什么

1. **内存池的动机**：碎片、缓存局部性、生命周期、系统调用开销
2. **Nginx 风格内存池**：小分配从 block 切，大分配独立 malloc，整池销毁
3. **pool_reset**：重置不释放内存，O(1) 复用
4. **异步日志的动机**：IO 线程不能阻塞在 write
5. **双缓冲区方案**：前端写 currentBuffer，后端写文件，互不干扰
6. **锁内轻量、锁外重量**：交换指针在锁内，写文件在锁外
7. **buffer 回收**：写完的 buffer 还给前端做备用，不浪费
8. **性能数据**：内存池 2.5x 快于 malloc，异步日志 128 万条/秒

### 代码文件

```
phase2/stage10_mempool_log/
├── mempool.h/.c     # 内存池（~150 行）
├── async_log.h/.c   # 异步日志（~250 行）
├── demo.c           # 演示（~120 行）
└── CMakeLists.txt
```

### 下一站

下一章我们做 **SO_REUSEPORT**——多个线程各自 epoll 监听同一端口，
内核做负载均衡。简单但效果立竿见影的多核利用方案。

---

## 九、内存池深入

### 9.1 Nginx 内存池的完整设计

Nginx 的 `ngx_pool_t` 比本章实现多了几个功能：

```c
struct ngx_pool_s {
    ngx_data_t     d;           /* block 数据（链表） */
    size_t         max;         /* 小分配上限 */
    ngx_pool_t    *current;     /* 当前 block */
    ngx_chain_t   *chain;       /* output chain（用于发送） */
    ngx_pool_log_t *log;        /* 日志 */
    ngx_pool_cleanup_t *cleanup; /* 清理回调链 */
    ngx_alloc_handler_t  alloc_handler;
    void           *data;       /* 用户数据 */
};
```

**清理回调**：注册销毁时的回调函数，用于关闭 fd、释放资源等：

```c
/* Nginx 的 cleanup 机制 */
typedef struct {
    void (*handler)(void *data);
    void *data;
    ngx_pool_cleanup_t *next;
} ngx_pool_cleanup_t;

/* 注册清理回调 */
cln = ngx_pool_cleanup_add(pool, sizeof(my_data));
cln->handler = my_cleanup;
cln->data = my_data;

/* pool_destroy 时自动调用所有清理回调 */
```

本章的简化版没有 cleanup，生产代码应该加上。

### 9.2 slab 分配器

对于固定大小的对象（如连接结构体），slab 分配器更高效：

```c
/* slab 分配器：固定大小对象池 */
typedef struct {
    void  **free_list;    /* 空闲对象链表 */
    size_t obj_size;      /* 每个对象大小 */
    int    capacity;      /* 容量 */
    char  *memory;        /* 预分配内存 */
} slab_t;

void *slab_alloc(slab_t *s) {
    if (s->free_list == NULL) return NULL;
    void *obj = s->free_list;
    s->free_list = *(void **)obj;  /* 从链表头取 */
    return obj;
}

void slab_free(slab_t *s, void *obj) {
    *(void **)obj = s->free_list;  /* 放回链表头 */
    s->free_list = obj;
}
```

Linux 内核的 slab/slub 分配器就是这个原理。
对于连接对象、HTTP 请求对象等固定大小场景，slab 比 pool 更快。

### 9.3 内存池的陷阱

**陷阱 1：池的生命周期不匹配**
```c
/* 错误：从短生命周期池分配长生命周期对象 */
pool_t *req_pool = pool_create(4096);
char *global_data = pool_alloc(req_pool, 100);  // 全局用
pool_destroy(req_pool);  // global_data 变成野指针！
```

**陷阱 2：池太大占内存**
```c
/* 错误：每个连接创建大池 */
pool_t *pool = pool_create(1024 * 1024);  // 1MB 每连接
// 10000 连接 = 10GB 内存！
```

**陷阱 3：线程安全**
```c
/* 本章的 pool 不是线程安全的！ */
/* 多线程共享同一个 pool 会导致数据竞争 */
/* 解决：per-thread pool 或加锁 */
```

### 9.4 调试内存池

```c
/* 加 debug 头到每次分配 */
typedef struct {
    size_t size;        /* 分配大小 */
    const char *file;   /* 分配位置 */
    int line;           /* 行号 */
    int magic;          /* 魔数（检测越界） */
} pool_debug_header_t;

void *pool_alloc_debug(pool_t *p, size_t size, const char *file, int line)
{
    void *raw = pool_alloc(p, size + sizeof(pool_debug_header_t) + 8);
    pool_debug_header_t *h = (pool_debug_header_t *)raw;
    h->size = size;
    h->file = file;
    h->line = line;
    h->magic = 0xDEADBEEF;
    /* 尾部加魔数检测越界 */
    *(int *)((char *)raw + sizeof(*h) + size) = 0xDEADBEEF;
    return (char *)raw + sizeof(*h);
}

#define pool_alloc(p, s) pool_alloc_debug(p, s, __FILE__, __LINE__)
```

---

## 十、异步日志深入

### 10.1 为什么 4MB buffer

```c
#define LOG_BUF_SIZE (4 * 1024 * 1024)  // 4MB
```

4MB 是经验值：
- 太小（如 4KB）：频繁交换 buffer，后端频繁 write
- 太大（如 64MB）：浪费内存，flush 延迟高
- 4MB：一次 write 系统调用写 4MB，效率高，延迟可控

muduo 也用 4MB，这是实践验证的好值。

### 10.2 前端锁的粒度

```c
void async_log_write(...)
{
    lock(mutex);           // ← 锁
    append(current_buffer, line);  // memcpy，纳秒级
    unlock(mutex);         // ← 解锁
}
```

锁内只做 memcpy（4KB 以内），不做格式化、不做 IO。
锁持有时间 < 1 微秒，对并发几乎无影响。

**对比：如果在锁内 write 文件**
```c
// 错误做法
lock(mutex);
write(fd, line, len);  // 可能阻塞几毫秒！
unlock(mutex);
// 其他线程全被阻塞
```

### 10.3 后端的批量写入

```c
/* 后端一次 write 多个 buffer */
for (int i = 0; i < num_buffers; i++) {
    write(fd, buffers[i]->data, buffers[i]->used);
}
```

如果 4 个 buffer 各 4MB，一次写 16MB——
1 次 write 系统调用 vs 128000 次 write（每条日志一次）。

### 10.4 flush 时机

当前实现：current_buffer 满了才唤醒后端。
如果日志不密集，日志会"卡"在 buffer 里不写文件。

**改进：定时 flush**

```c
/* 后端线程用超时等待 */
struct timespec timeout;
timeout.tv_sec = time(NULL) + 3;  // 3 秒超时
pthread_cond_timedwait(&cond, &mutex, &timeout);

/* 超时也醒来，把 current_buffer 的数据写入文件 */
```

muduo 的实现就有 3 秒超时 flush。

### 10.5 日志轮转

```c
/* 检查文件大小，超过阈值就轮转 */
if (file_size > MAX_FILE_SIZE) {
    close(fd);
    rename("server.log", "server.log.1");
    fd = open("server.log", O_WRONLY | O_CREAT | O_APPEND);
    file_size = 0;
}
```

生产环境必须做日志轮转，否则日志文件无限增长。

### 10.6 和其他日志库对比

| 库 | 语言 | 异步 | 吞吐 | 特点 |
|----|------|------|------|------|
| 本章实现 | C | 双缓冲 | 128万/s | 教学简化 |
| spdlog | C++ | 异步 | ~200万/s | 零分配、格式化快 |
| glog | C++ | 同步 | ~50万/s | Google 出品，功能全 |
| log4cxx | C++ | 异步 | ~30万/s | Java log4j 移植 |
| nginx log | C | 同步 | ~100万/s | per-worker 文件 |
| Redis log | C | 同步 | - | 单线程，不需要异步 |

---

## 十一、双缓冲的数学分析

### 11.1 为什么双缓冲足够

假设：
- 前端写入速度：Vf 条/秒
- 后端写入速度：Vb 条/秒
- buffer 大小：B 条

**稳态条件**：Vb >= Vf（后端写得比前端快）

如果 Vb < Vf（前端比后端快）：
- buffer 会不断堆积
- 需要更多 buffer 或限流

**双缓冲的利用率**：
- buffer A 被前端写满时间：B / Vf 秒
- buffer B 被后端写完时间：B / Vb 秒
- 如果 B / Vb < B / Vf（后端快），双缓冲够用
- 如果 B / Vb > B / Vf（后端慢），需要更多 buffer

### 11.2 实测验证

```
4 线程 × 100000 条 = 400000 条
buffer 大小 = 4MB ≈ 20000 条/buffer
需要 buffer 数 = 400000 / 20000 = 20 个

实测：buffer 数稳定在 2-3 个（后端写得够快）
```

---

## 十二、完整代码结构

```
phase2/stage10_mempool_log/
├── mempool.h          # 内存池接口（~100 行）
├── mempool.c          # 内存池实现（~150 行）
├── async_log.h        # 异步日志接口（~60 行）
├── async_log.c        # 异步日志实现（~250 行）
├── demo.c             # 演示程序（~120 行）
└── CMakeLists.txt
```

### 12.1 API 总览

```c
/* 内存池 */
pool_t *pool_create(size_t block_size);
void   pool_destroy(pool_t *pool);
void  *pool_alloc(pool_t *pool, size_t size);
void  *pool_calloc(pool_t *pool, size_t size);
void   pool_reset(pool_t *pool);
void   pool_stats(pool_t *pool, pool_stats_t *stats);

/* 异步日志 */
int    async_log_init(const char *filename, log_level_t level);
void   async_log_stop(void);
void   async_log_write(log_level_t level, const char *fmt, ...);

/* 便捷宏 */
alog_info("message %d", i);
alog_error("error: %s", strerror(errno));
```

### 12.2 性能对比总结

| 指标 | 传统方式 | 本章实现 | 提升 |
|------|---------|---------|------|
| 内存分配 | malloc/free | 内存池 | 2.5x |
| 日志写入 | 同步 write | 异步双缓冲 | 10x+ |
| IO 线程阻塞 | write 可能阻塞 | 零阻塞 | - |
| 内存碎片 | 有 | 无（整池管理） | - |
| 缓存局部性 | 差 | 好（连续内存） | - |

---

## 十三、在服务器架构中的位置

```
┌─────────────────────────────────────────────┐
│              服务器架构                      │
│                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │ Worker 1 │  │ Worker 2 │  │ Worker N │  │
│  │  epoll   │  │  epoll   │  │  epoll   │  │
│  │  pool    │  │  pool    │  │  pool    │  │  ← 每线程一个内存池
│  │  alog    │  │  alog    │  │  alog    │  │  ← 共享异步日志
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  │
│       │              │              │       │
│       └──────────────┼──────────────┘       │
│                      │                      │
│              ┌───────┴───────┐              │
│              │  Async Logger │              │
│              │  (后端线程)    │              │
│              │  → server.log │              │
│              └───────────────┘              │
└─────────────────────────────────────────────┘
```

- **内存池**：per-thread 或 per-request，不共享
- **异步日志**：所有线程共享一个后端，前端线程安全
- **后端线程**：独立于 worker 线程，只做日志 IO

### 13.1 集成示例

```c
/* 服务器启动 */
async_log_init("/var/log/server.log", LOG_LEVEL_INFO);

/* worker 线程 */
void *worker_loop(void *arg) {
    pool_t *thread_pool = pool_create(65536);  // 线程级池

    while (running) {
        epoll_wait(...);
        for each event {
            pool_t *req_pool = pool_create(4096);  // 请求级池

            http_request_t *req = pool_calloc(req_pool, sizeof(*req));
            parse_request(fd, req, req_pool);
            handle_request(fd, req, req_pool);

            alog_info("%s %s → %d", req->method, req->uri, req->status);

            pool_destroy(req_pool);  // 请求结束，整池销毁
        }
    }

    pool_destroy(thread_pool);
}

/* 服务器关闭 */
async_log_stop();
```

### 13.2 内存池的层次

```
全局池（进程级）
  └── 线程池（per-thread）
        └── 请求池（per-request）
              └── 临时分配
```

- **全局池**：配置、路由表等全局数据
- **线程池**：线程局部数据，避免锁竞争
- **请求池**：HTTP 请求相关数据，请求结束销毁

---

## 十四、常见问题

### Q1: 内存池会不会比 malloc 慢？

对于单次分配，pool_alloc 和 malloc 差不多（都是指针推进）。
但 pool 的优势在批量场景：1000 次分配 + 1 次销毁 vs 1000 次 malloc + 1000 次 free。

### Q2: 异步日志会丢日志吗？

如果进程崩溃，current_buffer 里还没写文件的日志会丢。
解决：
1. FATAL 级别同步 flush
2. 定期 fsync（减少丢数据窗口）
3. 用内存数据库做 buffer（崩溃可恢复）

### Q3: 双缓冲 vs 三缓冲？

双缓冲; 三缓冲在极端场景（前端爆发写入）能多一个 buffer 缓冲。
但实测双缓冲足够，三缓冲多浪费 4MB 内存。

### Q4: 内存池如何配合 free？

**不需要 free！** 这就是内存池的核心优势。
整池销毁时所有内存一起释放，不用逐个 free。
如果需要单独释放，用大分配（独立 malloc + free）。

### Q5: 异步日志 vs printf 调试？

开发阶段用 printf（即时看到输出）。
生产阶段用异步日志（不阻塞、有级别、可轮转）。
可以用宏在 debug 模式用 printf，release 模式用 alog。