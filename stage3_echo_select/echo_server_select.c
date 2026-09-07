/*
 * echo_server_select.c —— 阶段 3：select 多路复用 echo server
 *
 * ============================================================
 * 阶段 2 的问题：每个客户端 fork 一个进程，开销大（进程是重资源）
 * 解决方案：单进程 + select 同时监控多个 fd
 * ============================================================
 *
 * 核心思想：
 *   不再阻塞在一个 fd 上，而是告诉内核"我关心这些 fd"
 *   内核帮你盯着，任一 fd 有数据了就通知你
 *   你轮询检查哪些就绪了，处理它们
 *
 * select 的工作方式：
 *   1. 构建 fd_set（位图），标记所有关心的 fd
 *   2. 调用 select，阻塞直到有 fd 就绪
 *   3. select 返回，fd_set 被改成"就绪 fd 的集合"
 *   4. 遍历所有 fd，用 FD_ISSET 检查是否就绪
 *
 * select 的缺点（stage4 用 epoll 解决）：
 *   - FD_SETSIZE 上限 1024（fd > 1024 会内存越界！）
 *   - 每次都要重新构建 fd_set（select 会修改它）
 *   - 就绪检查是 O(n) 全集遍历
 *   - 内核也要 O(n) 遍历检查每个 fd
 *
 * 运行：
 *   build/bin/echo_server_select 8080
 *   多个终端： nc localhost 8080  ← 都能连，单进程！
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

#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define MAX_LINE 4096
#define BACKLOG  128
#define MAX_CLIENTS (FD_SETSIZE - 1)  /* 减 1 因为 listen_fd 也占一个 */

int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 3：select echo server ===");
    log_info("端口: %d, 最大客户端: %d", port, MAX_CLIENTS);

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

    /*
     * client[] 数组：记录所有已连接的客户端 fd
     * 初始化为 -1 表示空槽
     *
     * 为什么需要这个数组？
     *   select 返回后，我们要遍历所有关心的 fd 检查是否就绪
     *   需要知道"有哪些 fd"——就是从这个数组来的
     *   每次调 select 前也要从这个数组重建 fd_set
     */
    int client[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client[i] = -1;
    }

    int max_fd = listen_fd;  /* select 需要知道最大的 fd + 1 */
    fd_set all_set;          /* 所有关心的 fd 集合 */
    FD_ZERO(&all_set);
    FD_SET(listen_fd, &all_set);  /* 把 listen_fd 加入集合 */

    log_info("服务器开始监听 0.0.0.0:%d", port);
    printf("\n>>> 单进程同时服务多个客户端 <<<\n\n");

    /* ---------- 事件循环 ---------- */
    for (;;) {
        /*
         * 每次 select 前必须重建 fd_set！
         * 因为 select 会修改 fd_set（只保留就绪的 fd）
         * 所以我们维护一个 all_set 作为"全集"，每次拷贝给 rset
         */
        fd_set rset = all_set;  /* 结构体拷贝 */

        /*
         * select(nfds, readfds, writefds, exceptfds, timeout)
         *   nfds：最大 fd + 1（内核只检查 0 ~ nfds-1 的位）
         *   readfds：关心可读的 fd 集合（有数据可读 = 就绪）
         *   writefds/exceptfds：NULL 表示不关心
         *   timeout：NULL 表示无限等待
         *
         * 返回值：就绪 fd 的总数（可能多个同时就绪）
         *
         * 阻塞行为：
         *   没有任何 fd 就绪时，select 卡住等待
         *   任一 fd 就绪（有数据/新连接），select 返回
         */
        int nready = select(max_fd + 1, &rset, NULL, NULL, NULL);
        if (nready < 0) {
            if (errno == EINTR) {
                continue;  /* 被信号打断，重试 */
            }
            err_sys("select error");
        }

        /* ---------- 检查 listen_fd 是否就绪（有新连接） ---------- */
        /*
         * FD_ISSET(fd, &set)：检查 fd 是否在集合中（即是否就绪）
         *
         * listen_fd 就绪 = 内核已完成队列有新连接 = 可以 accept
         */
        if (FD_ISSET(listen_fd, &rset)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int conn_fd = Accept(listen_fd, (SA *)&client_addr, &client_len);

            /* 找一个空槽存放新 conn_fd */
            int i;
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (client[i] < 0) {
                    client[i] = conn_fd;
                    break;
                }
            }
            if (i == MAX_CLIENTS) {
                log_warn("客户端太多，拒绝连接");
                Close(conn_fd);
                continue;
            }

            /* 把新 conn_fd 加入 select 监控集合 */
            FD_SET(conn_fd, &all_set);
            if (conn_fd > max_fd) {
                max_fd = conn_fd;  /* 更新 max_fd */
            }

            log_info("新连接 conn_fd=%d，存入 client[%d]", conn_fd, i);

            if (--nready <= 0) {
                continue;  /* 没有更多就绪的 fd，回到 select */
            }
        }

        /* ---------- 检查所有客户端 fd 是否就绪（有数据） ---------- */
        /*
         * 这里是 select 的 O(n) 代价：
         *   即使只有 1 个 fd 就绪，也要遍历所有客户端
         *   10000 个客户端时，每次都要遍历 10000 次
         *   这就是 select 在高并发下慢的原因
         *
         * epoll 的改进：
         *   epoll_wait 直接返回就绪的 fd 列表，不需要遍历全集
         */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = client[i];
            if (fd < 0) {
                continue;
            }

            if (FD_ISSET(fd, &rset)) {
                char buf[MAX_LINE];
                ssize_t n = Read(fd, buf, sizeof(buf));

                if (n == 0) {
                    /* 客户端关闭连接 */
                    log_info("客户端 fd=%d 关闭", fd);
                    Close(fd);
                    FD_CLR(fd, &all_set);  /* 从集合中移除 */
                    client[i] = -1;        /* 释放槽 */
                } else {
                    Writen(fd, buf, n);    /* echo 回去 */
                }

                if (--nready <= 0) {
                    break;  /* 没有更多就绪的 fd */
                }
            }
        }
    }

    Close(listen_fd);
    return 0;
}