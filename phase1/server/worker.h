#ifndef SERVER_WORKER_H
#define SERVER_WORKER_H

/*
 * worker.h —— 工作线程模块（sub reactor）
 *
 * 每个工作线程：
 *   - 有独立的 epoll
 *   - 通过 pipe 接收主线程分发的新连接
 *   - 处理自己负责的连接的读写
 */

#include "router.h"
#include "config.h"
#include <pthread.h>

#define MAX_WORKERS 32
#define MAX_EVENTS  256

typedef struct {
    int       epoll_fd;    /* 自己的 epoll */
    int       notify_fd;   /* pipe 读端（接收主线程通知） */
    int       write_fd;    /* pipe 写端（主线程往这里写 conn_fd） */
    int       thread_id;   /* 线程编号 */
    pthread_t thread;      /* 线程句柄 */
} worker_t;

/*
 * workers_start —— 启动所有工作线程
 *   num: 工作线程数
 *   router: 路由表（工作线程用它来分发请求）
 *   www_root: 静态文件根目录
 */
void workers_start(worker_t *workers, int num,
                   router_t *router, const char *www_root);

/*
 * worker_dispatch —— 主线程把新连接分发给工作线程
 *   round-robin 方式
 */
void worker_dispatch(worker_t *workers, int num, int conn_fd);

#endif /* SERVER_WORKER_H */