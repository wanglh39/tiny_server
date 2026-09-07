#ifndef TINY_WRAP_POSIX_H
#define TINY_WRAP_POSIX_H

/*
 * wrap_posix.h —— POSIX 系统调用包装
 *
 * ============================================================
 * 这是整个教学项目的核心道具
 * ============================================================
 *
 * 为什么要包装每个系统调用？
 *
 *   原始代码：
 *     int fd = socket(AF_INET, SOCK_STREAM, 0);
 *     if (fd < 0) { perror("socket"); exit(1); }
 *
 *   问题：你看到了 socket 这个词，但看不到"内核返回了什么"。
 *
 *   包装后：
 *     int fd = Socket(AF_INET, SOCK_STREAM, 0);
 *
 *   日志输出：
 *     [12:34:56.789] [DEBUG] [wrap_posix.c:42] socket(2, 1, 0) = 3
 *
 *   现在你看到了：
 *     - AF_INET=2, SOCK_STREAM=1, protocol=0
 *     - 内核返回 fd=3
 *     - 下一个 socket 会返回 fd=4（fd 是递增分配的）
 *
 *   这让你建立"fd 是什么"的直觉：
 *     fd 就是内核进程文件描述符表的一个下标，
 *     0/1/2 被 stdin/stdout/stderr 占了，所以从 3 开始。
 *
 * 命名约定（UNP 风格）：
 *   原始小写 → 包装首字母大写
 *   socket → Socket, bind → Bind, accept → Accept ...
 *
 * 每个包装做三件事：
 *   1. 调用真正的系统调用
 *   2. 失败时 err_sys 打印 errno 并退出
 *   3. 成功时 log_debug 打印参数和返回值
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stddef.h>

/* 便捷别名：通用的 sockaddr 结构体指针 */
typedef struct sockaddr     SA;
typedef struct sockaddr_in  SAI;

/* ---------- Socket 基础 ---------- */

/*
 * Socket —— 创建套接字
 * 返回：新的 fd（>= 3）
 * 日志： socket(family=2, type=1, protocol=0) = 3
 */
int Socket(int family, int type, int protocol);

/*
 * Bind —— 绑定本地地址
 * 日志： bind(3, 127.0.0.1:8080) = 0
 */
void Bind(int fd, const SA *sa, socklen_t salen);

/*
 * Listen —— 监听，fd 从主动连接变为被动监听
 * backlog：内核维护两个队列
 *   - 未完成队列：收到 SYN 但还没完成三次握手的连接
 *   - 已完成队列：三次握手完成，等待 accept 的连接
 *   backlog 通常是两个队列长度之和（不同系统实现不同）
 * 日志： listen(3, backlog=128) = 0
 */
void Listen(int fd, int backlog);

/*
 * Accept —— 从已完成队列取出一个连接，返回新的 connfd
 * 返回：新的已连接 fd
 * 日志： accept(3) = 4  from 127.0.0.1:54321
 *
 * 关键理解：
 *   listen_fd（这里是 3）不真正参与数据传输，它只是一个"门口"
 *   accept 返回的 connfd（这里是 4）才是和客户端通信的 fd
 *   一个 listen_fd 可以 accept 出无数个 connfd
 */
int Accept(int fd, SA *sa, socklen_t *salen);

/*
 * Connect —— 主动连接（客户端用，服务器少用）
 */
void Connect(int fd, const SA *sa, socklen_t salen);

/*
 * Close —— 关闭 fd，内核引用计数减 1
 * 当引用计数归零时，才真正释放资源
 * 对 TCP：触发 FIN，开始四次挥手
 */
void Close(int fd);

/* ---------- 读写 ---------- */

/*
 * Read —— 包装 read，处理 EINTR（被信号中断）
 * 返回：读到的字节数，0 表示对端关闭
 *
 * 为什么要处理 EINTR？
 *   read 正在阻塞等待数据时，如果收到信号，read 会被打断返回 -1，
 *   errno=EINTR。这不是真正的错误，应该重试。
 *   但教学阶段我们先简单处理：返回 -1 让调用者决定。
 */
ssize_t Read(int fd, void *buf, size_t nbytes);

/*
 * Write —— 包装 write，处理 EINTR
 */
ssize_t Write(int fd, const void *buf, size_t nbytes);

/*
 * Readn —— 读取恰好 n 字节
 * 网络编程中 read 可能只返回部分数据（内核缓冲区不够），
 *   需要循环读直到读满 n 字节或对端关闭。
 * 返回：成功读到的字节数（< n 表示对端提前关闭）
 */
ssize_t Readn(int fd, void *buf, size_t nbytes);

/*
 * Writen —— 写入恰好 n 字节
 * write 也可能只写一部分，需要循环写。
 */
ssize_t Writen(int fd, const void *buf, size_t nbytes);

/*
 * Readline —— 读到 \n 为止（HTTP 请求行/头部按行解析）
 * 返回：读到的字节数（含 \n），0 表示对端关闭
 */
ssize_t Readline(int fd, void *buf, size_t maxlen);

/* ---------- Socket 选项 ---------- */

/*
 * Setsockopt —— 设置 socket 选项
 * 教学重点选项：
 *   SO_REUSEADDR：允许绑定 TIME_WAIT 状态的端口（服务器重启必备）
 *   SO_KEEPALIVE：开启 TCP 保活探测
 *   TCP_NODELAY：关闭 Nagle 算法（小包立即发送）
 */
void Setsockopt(int fd, int level, int optname,
                const void *optval, socklen_t optlen);

#endif /* TINY_WRAP_POSIX_H */