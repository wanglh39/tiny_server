/*
 * log.c —— 分级日志模块实现
 *
 * 核心知识点：
 *   1. localtime_r：线程安全的本地时间转换（vs localtime）
 *   2. strftime：把 struct tm 格式化成时间字符串
 *   3. gettimeofday / clock_gettime：获取毫秒级时间戳
 *   4. write vs fprintf：write 是系统调用，无缓冲；fprintf 有用户态缓冲
 *
 * 教学要点：
 *   为什么日志用 write() 而不是 fprintf()？
 *   —— fprintf 有用户态缓冲区，程序崩溃时未刷新的日志会丢失。
 *      write 直接走系统调用，写完就落盘（对 O_APPEND 文件），
 *      崩溃前最后几条日志能保住，对调试至关重要。
 */

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>   /* gettimeofday */
#include <unistd.h>
#include <fcntl.h>

/* ---------- 模块内部状态 ---------- */

static log_level_t g_level = LOG_DEBUG;  /* 默认输出所有级别 */
static int          g_fd    = 2;          /* 默认输出到 stderr（fd=2） */

/* 级别对应的字符串（用于输出） */
static const char *level_str[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

/* ---------- 公开接口 ---------- */

void log_set_level(log_level_t level)
{
    g_level = level;
}

int log_open_file(const char *path)
{
    if (path == NULL) {
        g_fd = 2;  /* 恢复到 stderr */
        return 0;
    }

    /*
     * O_WRONLY：只写
     * O_CREAT：不存在则创建
     * O_APPEND：追加写（多进程写同一日志文件时不会互相覆盖）
     * 0644：权限 -rw-r--r--（拥有者可读写，其他人只读）
     */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return -1;
    }
    g_fd = fd;
    return 0;
}

void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
{
    /* 级别过滤：低于设定级别的日志直接丢弃 */
    if (level < g_level) {
        return;
    }

    /* ---------- 1. 构建时间戳 ---------- */
    /*
     * gettimeofday 获取微秒级时间
     * tv_sec：自 1970-01-01 的秒数（Unix 时间戳）
     * tv_usec：微秒部分（0 ~ 999999）
     */
    struct timeval tv;
    gettimeofday(&tv, NULL);

    /* localtime_r：把 time_t 转成本地时间的 struct tm（线程安全） */
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    /* 格式化时间：HH:MM:SS.mmm */
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);
    int ms = tv.tv_usec / 1000;  /* 微秒转毫秒 */

    /* ---------- 2. 提取文件名（去掉路径前缀） ---------- */
    /*
     * __FILE__ 通常是 "common/log.c" 这样的路径，
     * 我们只想要 "log.c"，所以找最后一个 '/'
     */
    const char *short_file = file;
    const char *slash = strrchr(file, '/');
    if (slash) {
        short_file = slash + 1;
    }

    /* ---------- 3. 组装日志行 ---------- */
    /*
     * 格式：[12:34:56.789] [INFO] [log.c:42] 消息内容
     *
     * 为什么用固定大小的 buf 而不是动态分配？
     * —— 日志在错误路径上经常被调用，那时堆可能已经坏了，
     *    用栈上固定缓冲更安全。1024 对教学项目足够。
     */
    char buf[1024];
    int pos = snprintf(buf, sizeof(buf),
                       "[%s.%03d] [%s] [%s:%d] ",
                       time_buf, ms, level_str[level], short_file, line);

    /* 追加用户的消息 */
    va_list ap;
    va_start(ap, fmt);
    pos += vsnprintf(buf + pos, sizeof(buf) - pos, fmt, ap);
    va_end(ap);

    /* 追加换行 */
    if (pos < (int)sizeof(buf) - 1) {
        buf[pos++] = '\n';
    }

    /* ---------- 4. 输出 ---------- */
    /*
     * write：直接系统调用，无用户态缓冲
     * 崩溃时也能保证已 write 的内容落盘
     *
     * 为什么不用 fwrite/fprintf？
     *   它们有用户态缓冲区，崩溃时未刷新的会丢失。
     */
    write(g_fd, buf, pos);

    /* FATAL 级别直接退出 */
    if (level == LOG_FATAL) {
        exit(1);
    }
}