/*
 * wrap_posix.c —— POSIX 系统调用包装实现
 *
 * 这个文件是"看见系统调用"的窗口。
 * 每个包装函数都把参数和返回值打到日志里，
 * 让你像看 strace 一样观察程序在做什么。
 *
 * 但比 strace 更好的是：这是源码层面的，你能看到每个调用的上下文。
 */

#include "wrap_posix.h"
#include "error.h"
#include "log.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>   /* inet_ntop */
#include <unistd.h>

/* ---------- 内部辅助：把 sockaddr 格式化成 "IP:port" 字符串 ---------- */
static void format_addr(const SA *sa, socklen_t salen, char *buf, size_t buflen)
{
    (void)salen;  /* 暂未使用，保留接口以备将来支持 IPv6 */
    char ip[INET6_ADDRSTRLEN];
    uint16_t port = 0;

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        port = ntohs(sin->sin_port);
    } else {
        snprintf(ip, sizeof(ip), "?");
    }

    snprintf(buf, buflen, "%s:%u", ip, port);
}

/* ============================================================ */
/*  Socket 基础                                                  */
/* ============================================================ */

int Socket(int family, int type, int protocol)
{
    int fd = socket(family, type, protocol);
    if (fd < 0) {
        err_sys("socket(%d, %d, %d) error", family, type, protocol);
    }
    /*
     * 打印参数和返回值：
     * family=2(AF_INET) type=1(SOCK_STREAM) protocol=0(自动选 TCP)
     * 返回 fd=3（因为 0/1/2 被标准 IO 占了）
     */
    log_debug("socket(family=%d, type=%d, protocol=%d) = %d",
              family, type, protocol, fd);
    return fd;
}

void Bind(int fd, const SA *sa, socklen_t salen)
{
    char addr_buf[64];
    format_addr(sa, salen, addr_buf, sizeof(addr_buf));

    if (bind(fd, sa, salen) < 0) {
        err_sys("bind(%d, %s) error", fd, addr_buf);
    }
    log_debug("bind(%d, %s) = 0", fd, addr_buf);
}

void Listen(int fd, int backlog)
{
    /*
     * backlog 的含义（Linux 实现）：
     *   - 内核维护两个队列：未完成握手(SYN_RCVD) + 已完成(ESTABLISHED)
     *   - Linux 2.2+：backlog 只限制"已完成队列"
     *   - 未完成队列长度由 /proc/sys/net/ipv4/tcp_max_syn_backlog 控制
     *   - 如果已完成队列满了，新的 ACK 会被丢弃，对端会重传
     *
     * 常见值：
     *   128：教学/小服务够用
     *   SOMAXCONN：系统上限（通常 128 或 4096）
     */
    if (listen(fd, backlog) < 0) {
        err_sys("listen(%d, backlog=%d) error", fd, backlog);
    }
    log_debug("listen(%d, backlog=%d) = 0", fd, backlog);
}

int Accept(int fd, SA *sa, socklen_t *salen)
{
    int connfd;
    for (;;) {
        connfd = accept(fd, sa, salen);
        if (connfd >= 0) {
            break;
        }
        /*
         * EAGAIN/EWOULDBLOCK：非阻塞 fd 没有新连接，不是错误
         * EINTR：被信号打断，重试
         * 其他：真错误
         *
         * 这里我们简单处理：非阻塞模式返回 -1 让调用者处理
         * 阻塞模式下只有 EINTR 需要重试
         */
        if (errno == EINTR) {
            continue;
        }
        err_sys("accept(%d) error", fd);
    }

    char addr_buf[64] = "?";
    if (sa != NULL) {
        format_addr(sa, *salen, addr_buf, sizeof(addr_buf));
    }
    /*
     * 打印新连接的 fd 和来源地址：
     *   accept(3) = 4  from 127.0.0.1:54321
     *
     * 关键观察：
     *   listen_fd=3 不变，每次 accept 返回递增的 connfd
     *   客户端端口 54321 是内核随机分配的临时端口（ephemeral port）
     */
    log_debug("accept(%d) = %d  from %s", fd, connfd, addr_buf);
    return connfd;
}

void Connect(int fd, const SA *sa, socklen_t salen)
{
    char addr_buf[64];
    format_addr(sa, salen, addr_buf, sizeof(addr_buf));

    if (connect(fd, sa, salen) < 0) {
        err_sys("connect(%d, %s) error", fd, addr_buf);
    }
    log_debug("connect(%d, %s) = 0", fd, addr_buf);
}

void Close(int fd)
{
    if (close(fd) < 0) {
        err_sys("close(%d) error", fd);
    }
    /*
     * 对 TCP fd：close 触发 FIN，开始四次挥手
     * 主动 close 的一方会进入 TIME_WAIT，持续 2*MSL（通常 60 秒）
     * 这就是为什么服务器重启会报 "Address already in use"
     * 解决：设置 SO_REUSEADDR
     */
    log_debug("close(%d) = 0  (TCP: 触发 FIN, 开始四次挥手)", fd);
}

/* ============================================================ */
/*  读写                                                         */
/* ============================================================ */

ssize_t Read(int fd, void *buf, size_t nbytes)
{
    ssize_t n;
    for (;;) {
        n = read(fd, buf, nbytes);
        if (n >= 0) {
            break;
        }
        if (errno == EINTR) {
            /*
             * EINTR：read 阻塞时被信号打断
             * 不是错误，自动重试
             * （在 echo server 里如果你 Ctrl-C，就会看到这个）
             */
            continue;
        }
        /*
         * EAGAIN/EWOULDBLOCK：非阻塞 fd 暂时没数据
         * 这个我们返回 -1 让调用者处理，不在这里重试
         * （epoll ET 模式下这是正常情况）
         */
        err_sys("read(%d) error", fd);
    }
    log_debug("read(%d, %zu) = %zd", fd, nbytes, n);
    return n;
}

ssize_t Write(int fd, const void *buf, size_t nbytes)
{
    ssize_t n;
    for (;;) {
        n = write(fd, buf, nbytes);
        if (n >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        err_sys("write(%d) error", fd);
    }
    log_debug("write(%d, %zu) = %zd", fd, nbytes, n);
    return n;
}

ssize_t Readn(int fd, void *buf, size_t nbytes)
{
    /*
     * 为什么需要 Readn？
     *
     *   read(fd, buf, 100) 可能只返回 50，即使对端会发 100 字节！
     *   原因：内核接收缓冲区此刻只有 50 字节，read 不会等。
     *   （TCP 是字节流，没有"消息边界"的概念）
     *
     *   所以要读满 100 字节，必须循环 read：
     *     while (已读 < 100) { n = read(...); 已读 += n; }
     *
     *   这是 TCP 网络编程最基础的坑：粘包/分包。
     *   HTTP 用 Content-Length 或 \r\n\r\n 来界定边界，
     *   但底层的 read 不保证一次返回完整请求。
     */
    size_t nleft = nbytes;
    ssize_t nread;
    char *p = (char *)buf;

    while (nleft > 0) {
        nread = read(fd, p, nleft);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;  /* 信号打断，重试 */
            }
            err_sys("readn: read error");
        } else if (nread == 0) {
            break;  /* 对端关闭连接，提前结束 */
        }
        nleft -= nread;
        p      += nread;
    }

    log_debug("readn(%d, want=%zu, got=%zd)", fd, nbytes, (ssize_t)(nbytes - nleft));
    return nbytes - nleft;  /* 实际读到的字节数 */
}

ssize_t Writen(int fd, const void *buf, size_t nbytes)
{
    /*
     * write 也可能只写一部分（内核发送缓冲区满了），
     * 需要循环写完。
     */
    size_t nleft = nbytes;
    ssize_t nwritten;
    const char *p = (const char *)buf;

    while (nleft > 0) {
        nwritten = write(fd, p, nleft);
        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            err_sys("writen: write error");
        }
        nleft -= nwritten;
        p      += nwritten;
    }

    log_debug("writen(%d, %zu) = %zu", fd, nbytes, nbytes);
    return nbytes;
}

ssize_t Readline(int fd, void *buf, size_t maxlen)
{
    /*
     * 逐字节读直到遇到 \n
     *
     * 为什么逐字节？
     *   这是教学版，简单直观。生产版会用更高效的方式
     *   （比如自己维护一个读缓冲区，避免每个字节一次 read 系统调用）。
     *
     * HTTP 请求格式：
     *   GET / HTTP/1.1\r\n        ← 请求行
     *   Host: localhost\r\n       ← 头部
     *   \r\n                      ← 空行表示结束
     *
     * 每行以 \r\n 结尾，Readline 读到 \n 就停。
     */
    ssize_t n, rc;
    char c, *p = (char *)buf;

    for (n = 1; n < (ssize_t)maxlen; n++) {
        rc = read(fd, &c, 1);
        if (rc == 1) {
            *p++ = c;
            if (c == '\n') {
                break;  /* 读到换行，结束 */
            }
        } else if (rc == 0) {
            *p = '\0';
            return n - 1;  /* 对端关闭，返回已读字节数 */
        } else {
            if (errno == EINTR) {
                continue;
            }
            err_sys("readline: read error");
        }
    }

    *p = '\0';
    log_debug("readline(%d) = %zd bytes", fd, n);
    return n;
}

/* ============================================================ */
/*  Socket 选项                                                  */
/* ============================================================ */

void Setsockopt(int fd, int level, int optname,
                const void *optval, socklen_t optlen)
{
    if (setsockopt(fd, level, optname, optval, optlen) < 0) {
        err_sys("setsockopt(%d, level=%d, optname=%d) error",
                fd, level, optname);
    }
    log_debug("setsockopt(%d, level=%d, optname=%d) = 0",
              fd, level, optname);
}