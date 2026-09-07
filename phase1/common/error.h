#ifndef TINY_ERROR_H
#define TINY_ERROR_H

/*
 * error.h —— 错误处理模块
 *
 * 设计思想（来自 W. Richard Stevens《UNIX 网络编程》UNP 风格）：
 *
 *   系统调用失败时，errno 会被设置。我们不想在每个调用点都写：
 *     if (fd < 0) { perror("socket"); exit(1); }
 *
 *   而是封装成：
 *     err_sys("socket error");   // 自动读 errno 并打印 + exit
 *     err_quit("usage: ...");    // 不读 errno，直接打印 + exit
 *
 *   这样代码主干只关注正常流程，错误处理集中、统一。
 */

#include <stdio.h>   /* va_list 需要 stdio.h 里的 vfprintf */

/*
 * err_sys —— 系统调用出错时调用
 * 打印格式化的错误消息 + errno 对应的字符串，然后 exit(1)
 * 用法： err_sys("bind error on port %d", port);
 */
void err_sys(const char *fmt, ...);

/*
 * err_quit —— 非系统调用错误（如参数错误）时调用
 * 只打印格式化消息，不读 errno，然后 exit(1)
 * 用法： err_quit("usage: %s <port>", argv[0]);
 */
void err_quit(const char *fmt, ...);

/*
 * err_ret —— 系统调用出错但不退出（非致命错误）
 * 打印消息后返回，让调用者决定怎么处理
 * 用法： if (n < 0) err_ret("read interrupted, retry");
 */
void err_ret(const char *fmt, ...);

/*
 * err_dump —— 严重错误，打印消息后 abort() 生成 core dump
 * 用于"理论上不可能发生"的断言式错误
 */
void err_dump(const char *fmt, ...);

#endif /* TINY_ERROR_H */