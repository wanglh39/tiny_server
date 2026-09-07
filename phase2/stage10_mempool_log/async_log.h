#ifndef ASYNC_LOG_H
#define ASYNC_LOG_H

/*
 * async_log.h —— 异步日志（双缓冲区方案）
 *
 * 设计参考 muduo 的 AsyncLogging
 *
 * 为什么不能在 IO 线程直接 write 日志？
 *   1. write 是系统调用，可能阻塞（磁盘忙、NFS）
 *   2. 阻塞会卡住整个 EventLoop，所有连接都受影响
 *   3. 高频日志会让 IO 线程花大量时间在 write 上
 *
 * 双缓冲区方案：
 *   前端（IO 线程）              后端（日志线程）
 *   ┌──────────────┐            ┌──────────────┐
 *   │ currentBuffer │            │  写入文件     │
 *   │  [日志消息...] │            │  buffer A    │
 *   └──────────────┘            └──────────────┘
 *
 *   currentBuffer 满了：
 *   1. 把 currentBuffer 加入 buffersToWrite 队列
 *   2. 换上 nextBuffer（备用空 buffer）
 *   3. 唤醒后端线程
 *
 *   后端线程醒来：
 *   1. 交换 buffersToWrite 和本地 buffers（快，锁内操作）
 *   2. 释放锁
 *   3. 逐个 write buffers 到文件（锁外，不阻塞前端）
 *   4. 把写完的 buffer 还给前端做备用
 *
 * 关键：前端和后端不会同时操作同一个 buffer
 *   前端写 currentBuffer，后端写文件——互不干扰
 */

#include <pthread.h>

#define LOG_BUF_SIZE   (4 * 1024 * 1024)  /* 4MB 每个缓冲区 */
#define LOG_MAX_LINE   4096               /* 单条日志最大长度 */

/* 日志级别 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
} log_level_t;

/* ---------- 前端 API ---------- */

/*
 * async_log_init —— 初始化异步日志
 *   filename: 日志文件名
 *   level:    最低输出级别
 * 返回 0 成功，-1 失败
 */
int async_log_init(const char *filename, log_level_t level);

/*
 * async_log_stop —— 停止日志线程，刷出剩余日志
 */
void async_log_stop(void);

/*
 * async_log_write —— 写日志（前端，非阻塞）
 *   线程安全，可在任何线程调用
 */
void async_log_write(log_level_t level, const char *fmt, ...);

/* 便捷宏 */
#define alog_debug(fmt, ...) async_log_write(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define alog_info(fmt, ...)  async_log_write(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define alog_warn(fmt, ...)  async_log_write(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define alog_error(fmt, ...) async_log_write(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define alog_fatal(fmt, ...) async_log_write(LOG_LEVEL_FATAL, fmt, ##__VA_ARGS__)

#endif /* ASYNC_LOG_H */