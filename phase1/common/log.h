#ifndef TINY_LOG_H
#define TINY_LOG_H

/*
 * log.h —— 分级日志模块
 *
 * 为什么需要日志而不是 printf？
 *
 *   1. 分级过滤：开发时看 DEBUG，生产时只看 ERROR
 *   2. 带时间戳：知道每件事什么时候发生（对网络调试至关重要）
 *   3. 统一格式：[时间] [级别] 消息，方便 grep 和分析
 *   4. 可重定向：日志可以输出到文件，不污染终端
 *
 * 教学要点：
 *   这个日志模块是同步的（直接 write 到 stderr）。
 *   到 stage7 我们会改成异步双缓冲版（muduo 风格），
 *   那时你会理解为什么 IO 线程不能被日志的磁盘写入阻塞。
 */

/* 日志级别枚举，从低到高 */
typedef enum {
    LOG_DEBUG = 0,  /* 调试信息：每个 syscall 的参数和返回值 */
    LOG_INFO  = 1,  /* 一般信息：连接建立、请求到达 */
    LOG_WARN  = 2,  /* 警告：非预期但可恢复（如 EAGAIN、EINTR） */
    LOG_ERROR = 3,  /* 错误：系统调用失败 */
    LOG_FATAL = 4,  /* 致命：直接退出 */
} log_level_t;

/*
 * log_set_level —— 设置最低输出级别
 * 低于此级别的日志被丢弃
 * 用法： log_set_level(LOG_INFO);  // 不输出 DEBUG
 */
void log_set_level(log_level_t level);

/*
 * log_open_file —— 把日志输出重定向到文件
 * 传 NULL 则输出到 stderr
 * 返回 0 成功，-1 失败
 */
int log_open_file(const char *path);

/*
 * log_write —— 日志写入的核心函数
 * 一般不直接调用，而是用下面的宏
 */
void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...);

/* ---------- 便捷宏，自动填入源码位置 ---------- */
/*
 * __FILE__ 和 __LINE__ 是编译器预定义宏，
 * 能让日志显示"这条日志来自哪个文件第几行"，调试时非常有用。
 *
 * 输出示例：
 *   [12:34:56.789] [INFO] [sniff.c:42] listening on interface lo
 */
#define log_debug(fmt, ...) log_write(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  log_write(LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  log_write(LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) log_write(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_fatal(fmt, ...) log_write(LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/*
 * ##__VA_ARGS__ 是 GCC 扩展：
 * 当没有额外参数时，## 会吃掉前面的逗号，避免编译错误。
 * 例如 log_info("listening") 展开为 log_write(..., "listening")
 * 而不是 log_write(..., "listening",)  ← 尾逗号会报错
 */

#endif /* TINY_LOG_H */