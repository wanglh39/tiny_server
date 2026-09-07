/*
 * error.c —— 错误处理模块实现
 *
 * 核心知识点：
 *   1. errno 是线程局部存储（thread-local），每次系统调用后会被设置
 *   2. strerror(errno) 把错误码转成人能读的字符串
 *   3. vfprintf 处理可变参数（... 和 va_list）
 *
 * 教学要点：
 *   读完这个文件，你应该理解 perror() 背后做了什么，
 *   以及为什么 errno 要在系统调用后"立即"读取（下一个调用会覆盖它）。
 */

#include "error.h"

#include <errno.h>    /* errno 全局变量 */
#include <stdarg.h>   /* va_list / va_start / va_end */
#include <stdlib.h>   /* exit / abort */
#include <string.h>   /* strerror */

/* ---------- 内部辅助函数 ---------- */

/*
 * do_it —— 打印错误消息的公共部分
 * oflag=1 表示要附加 errno 的字符串（系统调用错误）
 * oflag=0 表示不附加（逻辑错误）
 *
 * 为什么用 vfprintf？
 *   fmt 是 "bind error on port %d" 这样的格式串，
 *   后面跟着可变参数。vfprintf 能处理 va_list 类型的可变参数。
 */
static void do_it(int oflag, const char *fmt, va_list ap)
{
    char buf[1024];

    /* vsnprintf：把格式化的可变参数写入 buf，最多 1024 字节，防止溢出 */
    vsnprintf(buf, sizeof(buf), fmt, ap);

    if (oflag) {
        /* 系统调用错误：追加 ": " + errno 对应的字符串 */
        int save_errno = errno;  /* 先保存 errno，防止后面的操作覆盖它 */
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
                 ": %s", strerror(save_errno));
    }

    /* 输出到 stderr（不是 stdout，因为 stdout 可能有缓冲区未刷新） */
    fflush(stdout);  /* 先把 stdout 的缓冲刷掉，防止输出顺序乱 */
    fputs(buf, stderr);
    fputs("\n", stderr);
    fflush(stderr);  /* 立即刷新 stderr，确保错误消息可见 */
}

/* ---------- 公开接口 ---------- */

void err_sys(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    do_it(1, fmt, ap);   /* oflag=1：附加 errno 字符串 */
    va_end(ap);
    exit(1);
}

void err_quit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    do_it(0, fmt, ap);   /* oflag=0：不附加 errno */
    va_end(ap);
    exit(1);
}

void err_ret(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    do_it(1, fmt, ap);
    va_end(ap);
    /* 不 exit，返回让调用者处理 */
}

void err_dump(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    do_it(1, fmt, ap);
    va_end(ap);
    abort();  /* 触发 SIGABRT，生成 core dump（如果系统开启了 core） */
}