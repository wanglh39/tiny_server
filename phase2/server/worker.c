/*
 * worker.c —— 工作线程实现
 *
 * ============================================================
 *  每个工作线程：
 *    1. 创建自己的 listen_fd（SO_REUSEPORT，内核负载均衡）
 *    2. 创建独立 epoll，监听 listen_fd
 *    3. 事件循环：accept 新连接 → 加入 epoll
 *    4. 连接就绪 → conn_handle_read（协议检测 + 分发）
 *
 *  SO_REUSEPORT 的优势：
 *    - 无主线程瓶颈（每个线程自己 accept）
 *    - 无跨线程 pipe 通知
 *    - 内核负载均衡（连接均匀分配到各线程）
 *    - 无惊群问题（内核保证只有一个线程被唤醒）
 * ============================================================
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ---------- 创建监听 socket（SO_REUSEPORT） ---------- */

static int create_listen_socket(int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    set_reuseaddr(fd);
    set_reuseport(fd);
    set_nonblocking(fd);

    /* 关闭 Nagle 算法（小包立即发送） */
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, backlog) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* ---------- 工作线程主循环 ---------- */

void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    const server_config_t *cfg = w->cfg;

    /* 1. 创建自己的监听 socket */
    w->listen_fd = create_listen_socket(cfg->port, cfg->backlog);
    if (w->listen_fd < 0) {
        fprintf(stderr, "线程 #%d: 创建监听 socket 失败\n", w->thread_id);
        return NULL;
    }

    /* 2. 创建 epoll */
    w->epoll_fd = epoll_create1(0);
    if (w->epoll_fd < 0) {
        perror("epoll_create1");
        close(w->listen_fd);
        return NULL;
    }

    /* 3. 监听 listen_fd（ET 模式） */
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = w->listen_fd;
    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, w->listen_fd, &ev);

    if (cfg->log_file[0]) {
        alog_info("线程 #%d 启动: listen_fd=%d epoll_fd=%d 端口=%d",
                  w->thread_id, w->listen_fd, w->epoll_fd, cfg->port);
    }
    printf("  线程 #%d 就绪 (listen_fd=%d)\n", w->thread_id, w->listen_fd);

    /* 4. 事件循环 */
    struct epoll_event events[MAX_EVENTS];

    while (g_server.running) {
        int n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == w->listen_fd) {
                /* listen_fd 就绪 → 循环 accept（ET 模式） */
                for (;;) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(w->listen_fd,
                                         (struct sockaddr *)&client_addr,
                                         &client_len);
                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }

                    w->accept_count++;

                    /* 创建连接并加入 epoll */
                    set_nonblocking(conn_fd);

                    conn_t *conn = conn_create(conn_fd, w->thread_id);
                    if (!conn) {
                        close(conn_fd);
                        continue;
                    }

                    struct epoll_event cev;
                    cev.events   = EPOLLIN | EPOLLET;
                    cev.data.ptr = conn;
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, conn_fd, &cev);
                }
            } else {
                /* 连接就绪 → 处理读事件 */
                conn_t *conn = events[i].data.ptr;
                if (!conn) continue;

                int alive = conn_handle_read(conn, w);
                if (!alive) {
                    close(conn->fd);
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    conn_free(conn);
                }
            }
        }
    }

    /* 5. 清理 */
    close(w->listen_fd);
    close(w->epoll_fd);

    if (cfg->log_file[0]) {
        alog_info("线程 #%d 退出: accept=%ld request=%ld",
                  w->thread_id, w->accept_count, w->request_count);
    }
    printf("  线程 #%d 退出 (accept=%ld request=%ld)\n",
           w->thread_id, w->accept_count, w->request_count);

    return NULL;
}
