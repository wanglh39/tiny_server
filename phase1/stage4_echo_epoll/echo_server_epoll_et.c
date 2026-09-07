/*
 * echo_server_epoll_et.c —— 阶段 4b：epoll 边沿触发（ET）echo server
 *
 * ============================================================
 * ET（Edge Triggered，边沿触发）—— epoll 的高性能模式
 * ============================================================
 *
 * ET vs LT 的核心区别：
 *
 *   LT（水平触发）：
 *     只要 fd 有数据可读，每次 epoll_wait 都返回它
 *     可以只读一部分，下次还会通知
 *
 *   ET（边沿触发）：
 *     只在 fd 从"无数据"变"有数据"时通知一次
 *     如果不读完，剩余数据不会再通知！
 *     必须循环 read 直到 EAGAIN
 *
 * 为什么要 ET？
 *   - 减少 epoll_wait 的调用次数（每个事件只通知一次）
 *   - 高并发下吞吐量更好
 *   - 但编程更复杂，必须配合非阻塞 fd
 *
 * ET 的三个必须：
 *   1. fd 必须设为非阻塞（O_NONBLOCK）
 *   2. read 必须循环到 EAGAIN
 *   3. 不能漏掉任何一次通知（否则数据永远卡着）
 *
 * 运行：
 *   build/bin/echo_server_epoll_et 8080
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define MAX_LINE   4096
#define BACKLOG    128
#define MAX_EVENTS 1024

/* ---------- 设置 fd 为非阻塞 ---------- */
/*
 * 非阻塞 fd 的行为：
 *   read 没数据时立即返回 -1，errno=EAGAIN（不会卡住）
 *   这样我们才能循环 read 到 EAGAIN 来"榨干"所有数据
 *
 * 为什么 ET 必须非阻塞？
 *   如果用阻塞 fd，循环 read 最后一次会卡住（没数据了但还在等）
 *   非阻塞 fd 没数据时立即返回 EAGAIN，我们才知道"读完了"
 */
static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        err_sys("fcntl F_GETFL error");
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        err_sys("fcntl F_SETFL O_NONBLOCK error");
    }
    log_debug("fd=%d 设为非阻塞", fd);
}

int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 4b：epoll ET echo server ===");
    log_info("端口: %d", port);

    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* listen_fd 也设为非阻塞（ET 模式下所有 fd 都要非阻塞） */
    set_nonblocking(listen_fd);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    Bind(listen_fd, (SA *)&server_addr, sizeof(server_addr));
    Listen(listen_fd, BACKLOG);

    int epfd = epoll_create(1);
    if (epfd < 0) {
        err_sys("epoll_create error");
    }

    /*
     * EPOLLET 标志：启用边沿触发模式
     * EPOLLIN：关心可读事件
     *
     * ET 模式下，epoll_wait 只在状态变化时通知一次
     */
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;  /* ET 模式 */
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    log_info("服务器开始监听 0.0.0.0:%d (ET 模式)", port);
    printf("\n>>> epoll ET 模式，高性能但需循环读到 EAGAIN <<<\n\n");

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err_sys("epoll_wait error");
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                /*
                 * ET 模式下 listen_fd 也只通知一次！
                 * 可能同时有多个连接完成三次握手
                 * 但 epoll_wait 只通知一次，必须循环 accept 到 EAGAIN
                 * 否则后续的连接不会被处理（饥饿）
                 */
                for (;;) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(listen_fd, (SA *)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            /*
                             * EAGAIN：没有更多连接了，accept 读完
                             * 这是循环退出的条件
                             */
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        err_sys("accept error");
                    }

                    /* 新 conn_fd 必须设为非阻塞 */
                    set_nonblocking(conn_fd);

                    /* 加入 epoll，ET 模式 */
                    ev.events  = EPOLLIN | EPOLLET;
                    ev.data.fd = conn_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev);
                }

            } else {
                /*
                 * ET 模式下 conn_fd 也只通知一次！
                 * 必须循环 read 到 EAGAIN，把所有数据读完
                 * 否则剩余数据不会再通知，造成数据丢失/饥饿
                 */
                char buf[MAX_LINE];
                for (;;) {
                    ssize_t nread = read(fd, buf, sizeof(buf));

                    if (nread > 0) {
                        /* 有数据，echo 回去 */
                        Writen(fd, buf, nread);
                        continue;  /* 继续读，看还有没有 */
                    }

                    if (nread == 0) {
                        /* 客户端关闭连接 */
                        log_debug("客户端 fd=%d 关闭", fd);
                        Close(fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        break;
                    }

                    /* nread < 0 */
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        /*
                         * EAGAIN：数据读完了，没有更多了
                         * 这是 ET 模式下循环 read 的正常退出条件
                         */
                        break;
                    }

                    if (errno == EINTR) {
                        continue;  /* 被信号打断，重试 */
                    }

                    /* 其他错误 */
                    log_error("read fd=%d error: %s", fd, strerror(errno));
                    Close(fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    break;
                }
            }
        }
    }

    Close(listen_fd);
    Close(epfd);
    return 0;
}