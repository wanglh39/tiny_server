/*
 * echo_server_epoll_lt.c —— 阶段 4a：epoll 水平触发（LT）echo server
 *
 * ============================================================
 * 阶段 3 的问题：select 每次都要 O(n) 遍历所有 fd
 * 解决方案：epoll，内核维护就绪队列，直接返回就绪的 fd
 * ============================================================
 *
 * epoll 的三个核心 API：
 *   epoll_create(size)  → 创建 epoll 实例，返回 fd
 *   epoll_ctl(epfd, op, fd, event) → 添加/修改/删除关心的 fd
 *   epoll_wait(epfd, events, maxevents, timeout) → 等待就绪事件
 *
 * LT（Level Triggered，水平触发）：
 *   只要 fd 有数据可读，每次 epoll_wait 都会返回它
 *   类似 select 的行为，但不需要遍历全集
 *   可以不读完（下次 epoll_wait 还会通知）
 *   编程简单，但可能多几次 epoll_wait 调用
 *
 * 对比 select 的优势：
 *   1. 没有 FD_SETSIZE 1024 上限
 *   2. 不需要每次重建 fd_set（epoll_ctl 只在增删时调用）
 *   3. epoll_wait 直接返回就绪列表，O(就绪数) 而非 O(总数)
 *
 * 运行：
 *   build/bin/echo_server_epoll_lt 8080
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

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

int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 4a：epoll LT echo server ===");
    log_info("端口: %d", port);

    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    Bind(listen_fd, (SA *)&server_addr, sizeof(server_addr));
    Listen(listen_fd, BACKLOG);

    /* ---------- 创建 epoll 实例 ---------- */
    /*
     * epoll_create(1) 创建一个 epoll 实例
     *   参数 size 在旧内核是提示大小，现在忽略（只要 > 0）
     *   返回一个 fd（epfd），操作 epoll 就用这个 fd
     *
     * 内核里创建了一个 eventpoll 对象，包含：
     *   - 红黑树：存储所有注册的 fd（epoll_ctl 增删改）
     *   - 就绪链表：存储已就绪的 fd（epoll_wait 取这里）
     */
    int epfd = epoll_create(1);
    if (epfd < 0) {
        err_sys("epoll_create error");
    }
    log_debug("epoll_create() = %d", epfd);

    /* ---------- 把 listen_fd 加入 epoll ---------- */
    /*
     * struct epoll_event：
     *   events：关心的事件类型（EPOLLIN = 可读）
     *   data：用户数据，通常存 fd，epoll_wait 返回时带回来
     *
     * EPOLLIN：关心可读事件（有数据 / 新连接）
     * LT 模式是默认的，不需要额外标志
     */
    struct epoll_event ev;
    ev.events  = EPOLLIN;       /* LT 模式（默认） */
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);
    log_debug("epoll_ctl(ADD, listen_fd=%d)", listen_fd);

    log_info("服务器开始监听 0.0.0.0:%d", port);
    printf("\n>>> epoll LT 模式，单进程高并发 <<<\n\n");

    /* ---------- 事件循环 ---------- */
    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        /*
         * epoll_wait(epfd, events, maxevents, timeout)
         *   epfd：epoll 实例
         *   events：输出参数，就绪的事件数组
         *   maxevents：events 数组大小（一次最多返回多少个）
         *   timeout：-1 无限等待
         *
         * 返回值 n：就绪事件数
         *
         * 关键区别 vs select：
         *   select 返回后要 O(总数) 遍历检查每个 fd
         *   epoll_wait 直接把就绪的放在 events[0..n-1]
         *   只需 O(就绪数) 遍历，通常就绪数远小于总数
         */
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err_sys("epoll_wait error");
        }

        /* ---------- 处理就绪事件 ---------- */
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                /* listen_fd 就绪 = 有新连接 */
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int conn_fd = Accept(listen_fd, (SA *)&client_addr, &client_len);

                /* 把新 conn_fd 加入 epoll */
                ev.events  = EPOLLIN;       /* LT 模式 */
                ev.data.fd = conn_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev);

            } else {
                /* conn_fd 就绪 = 有数据可读 */
                char buf[MAX_LINE];
                ssize_t nread = read(fd, buf, sizeof(buf));

                if (nread <= 0) {
                    /* 客户端关闭或出错 */
                    log_debug("客户端 fd=%d 关闭", fd);
                    Close(fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                } else {
                    /*
                     * LT 模式下可以不读完：
                     * 如果 buf 不够大，只读了一部分，
                     * 下次 epoll_wait 还会通知这个 fd（因为还有数据）
                     *
                     * 但实际应用中还是建议尽量读完
                     */
                    Writen(fd, buf, nread);
                }
            }
        }
    }

    Close(listen_fd);
    Close(epfd);
    return 0;
}