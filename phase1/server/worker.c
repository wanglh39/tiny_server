/*
 * worker.c —— 工作线程实现
 */

#include "worker.h"
#include "connection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>

#include "../common/log.h"

/* 工作线程需要访问的共享数据 */
typedef struct {
    worker_t   *self;
    router_t   *router;
    const char *www_root;
} worker_ctx_t;

static worker_ctx_t g_ctx[MAX_WORKERS];

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------- 工作线程主循环 ---------- */
static void *worker_loop(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    worker_t *w = ctx->self;

    log_info("工作线程 #%d 启动 (epoll_fd=%d)", w->thread_id, w->epoll_fd);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        for (int i = 0; i < n; i++) {
            /*
             * 两种事件：
             *   1. notify_fd 就绪 → 主线程发来新连接
             *   2. conn_fd 就绪 → 连接有数据
             *
             * 用 data.fd == notify_fd 区分
             * （notify_fd 是小数字，conn 的 ptr 是堆地址，不冲突）
             */
            if (events[i].data.fd == w->notify_fd) {
                /* 新连接通知 */
                int conn_fd;
                while (read(w->notify_fd, &conn_fd, sizeof(int)) > 0) {
                    set_nonblocking(conn_fd);
                    conn_t *conn = conn_create(conn_fd);

                    struct epoll_event ev;
                    ev.events   = EPOLLIN | EPOLLET;
                    ev.data.ptr = conn;
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);
                }
            } else {
                /* 连接有数据 */
                conn_t *conn = events[i].data.ptr;

                int alive = conn_handle_read(conn, ctx->router,
                                            ctx->www_root, w->thread_id);
                if (!alive) {
                    close(conn->fd);
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    conn_free(conn);
                }
            }
        }
    }

    return NULL;
}

/* ---------- 启动工作线程 ---------- */
void workers_start(worker_t *workers, int num,
                   router_t *router, const char *www_root)
{
    for (int i = 0; i < num; i++) {
        workers[i].thread_id = i;
        workers[i].epoll_fd  = epoll_create(1);

        /* 创建 pipe 用于主线程通知 */
        int pipe_fd[2];
        if (pipe(pipe_fd) < 0) {
            log_error("pipe 创建失败");
            continue;
        }
        set_nonblocking(pipe_fd[0]);
        set_nonblocking(pipe_fd[1]);

        workers[i].notify_fd = pipe_fd[0];
        workers[i].write_fd  = pipe_fd[1];

        /* 把 notify_fd 加入 epoll */
        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = pipe_fd[0];
        epoll_ctl(workers[i].epoll_fd, EPOLL_CTL_ADD, pipe_fd[0], &ev);

        /* 准备线程上下文 */
        g_ctx[i].self      = &workers[i];
        g_ctx[i].router    = router;
        g_ctx[i].www_root  = www_root;

        /* 启动线程 */
        pthread_create(&workers[i].thread, NULL, worker_loop, &g_ctx[i]);
    }
}

/* ---------- 分发连接 ---------- */
static int next_worker = 0;

void worker_dispatch(worker_t *workers, int num, int conn_fd)
{
    /* round-robin：轮流分发给不同工作线程 */
    int target = next_worker;
    next_worker = (next_worker + 1) % num;

    /* 通过 pipe 通知工作线程 */
    write(workers[target].write_fd, &conn_fd, sizeof(int));
}