/*
 * reactor_server.c —— 阶段 6：主从 Reactor + 线程池
 *
 * ============================================================
 * 阶段 5 的问题：单线程处理所有 IO，无法利用多核
 * 解决方案：主从 Reactor 模式
 * ============================================================
 *
 * 架构：
 *   主线程（main reactor）：
 *     - epoll 监听 listen_fd
 *     - accept 新连接
 *     - round-robin 分发给工作线程
 *
 *   工作线程（sub reactor）× N：
 *     - 各自有一个 epoll
 *     - 处理分配给自己的连接的读写
 *     - 用 http_parser 解析请求
 *
 * 线程间通信：
 *   主线程通过 pipe 把新 conn_fd 通知给工作线程
 *   工作线程 epoll 监听 pipe，收到通知后把 conn_fd 加入自己的 epoll
 *
 * 为什么不用互斥锁共享一个 epoll？
 *   - 多线程竞争同一个 epoll，锁开销大
 *   - 主从 Reactor 每个线程独立 epoll，无锁
 *
 * 运行：
 *   build/bin/reactor_server 8080 4   # 4 个工作线程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"
#include "http_parser.h"

#define MAX_EVENTS 256
#define BACKLOG    512
#define READ_BUF   4096
#define MAX_WORKERS 32

/* ---------- 连接结构体 ---------- */
typedef struct {
    int            fd;
    http_parser_t  parser;
} conn_t;

/* ---------- 工作线程结构体 ---------- */
typedef struct {
    int       epoll_fd;    /* 自己的 epoll */
    int       notify_fd;   /* pipe 读端，接收主线程通知 */
    int       thread_id;   /* 线程编号 */
    pthread_t thread;      /* 线程句柄 */
} worker_t;

static worker_t workers[MAX_WORKERS];
static int      num_workers = 4;
static int      next_worker = 0;  /* round-robin 分发 */

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------- 构造 HTTP 响应 ---------- */
static int build_response(const http_request_t *req, char *buf, int bufsize,
                          int worker_id)
{
    const char *method = http_method_str(req->method);

    char body[2048];
    int body_len = snprintf(body, sizeof(body),
        "Hello from tiny_server (stage6 Reactor)!\r\n"
        "\r\n"
        "Handled by worker thread #%d\r\n"
        "Method: %s\r\n"
        "URI:    %s\r\n"
        "Host:   %s\r\n",
        worker_id, method, req->uri, req->host);

    return snprintf(buf, bufsize,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        body_len, body);
}

/* ---------- 工作线程函数 ---------- */
/*
 * 每个工作线程：
 *   1. 创建自己的 epoll
 *   2. 把 notify_fd（pipe 读端）加入 epoll
 *   3. 循环 epoll_wait：
 *      - notify_fd 就绪：主线程发来新 conn_fd，加入自己的 epoll
 *      - conn_fd 就绪：有数据，read → parse → response
 */
static void *worker_loop(void *arg)
{
    worker_t *w = (worker_t *)arg;

    log_info("工作线程 #%d 启动，epoll_fd=%d", w->thread_id, w->epoll_fd);

    struct epoll_event events[MAX_EVENTS];
    char buf[READ_BUF];

    for (;;) {
        int n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == w->notify_fd) {
                /* 主线程通知：有新连接 */
                int conn_fd;
                while (read(w->notify_fd, &conn_fd, sizeof(int)) > 0) {
                    set_nonblocking(conn_fd);

                    conn_t *conn = calloc(1, sizeof(conn_t));
                    conn->fd = conn_fd;
                    http_parser_init(&conn->parser);

                    struct epoll_event ev;
                    ev.events   = EPOLLIN | EPOLLET;
                    ev.data.ptr = conn;
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);

                    log_debug("worker #%d 接管 conn_fd=%d",
                              w->thread_id, conn_fd);
                }

            } else {
                /* 连接有数据 */
                conn_t *conn = events[i].data.ptr;
                int fd = conn->fd;

                for (;;) {
                    ssize_t nread = read(fd, buf, sizeof(buf));
                    if (nread > 0) {
                        http_parse_result_t result;
                        result = http_parser_feed(&conn->parser, buf, nread);

                        if (result == HTTP_PARSE_DONE) {
                            char response[4096];
                            int resp_len = build_response(
                                &conn->parser.request,
                                response, sizeof(response),
                                w->thread_id);
                            write(fd, response, resp_len);
                            http_parser_reset(&conn->parser);
                        } else if (result == HTTP_PARSE_ERROR) {
                            Close(fd);
                            epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                            free(conn);
                            break;
                        }

                    } else if (nread == 0) {
                        Close(fd);
                        epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        free(conn);
                        break;

                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        Close(fd);
                        epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        free(conn);
                        break;
                    }
                }
            }
        }
    }

    return NULL;
}

/* ---------- 初始化工作线程 ---------- */
static void workers_init()
{
    for (int i = 0; i < num_workers; i++) {
        workers[i].thread_id = i;
        workers[i].epoll_fd  = epoll_create(1);

        /*
         * 创建 pipe 用于主线程通知工作线程
         *   pipe_fd[0]：读端（工作线程 epoll 监听这个）
         *   pipe_fd[1]：写端（主线程往这里写 conn_fd）
         *
         * 为什么用 pipe 而不是 eventfd？
         *   eventfd 只能传一个 uint64，pipe 可以传任意数据
         *   pipe 更直观，教学更容易理解
         *   eventfd 更高效（不涉及文件 IO），生产环境用它
         */
        int pipe_fd[2];
        if (pipe(pipe_fd) < 0) {
            err_sys("pipe error");
        }
        set_nonblocking(pipe_fd[0]);
        set_nonblocking(pipe_fd[1]);

        workers[i].notify_fd = pipe_fd[0];
        /* pipe_fd[1] 存在哪里？我们用一个全局数组 */
        /* 简化：直接存在 notify_fd 的对端，用 fcntl 获取不了
           所以我们改用结构体里多存一个字段 */

        /* 把 notify_fd 加入工作线程的 epoll */
        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = pipe_fd[0];
        epoll_ctl(workers[i].epoll_fd, EPOLL_CTL_ADD, pipe_fd[0], &ev);

        /* 启动工作线程 */
        pthread_create(&workers[i].thread, NULL, worker_loop, &workers[i]);
    }
}

/* 简化：pipe 写端全局存储 */
static int worker_write_fds[MAX_WORKERS];

static void workers_init_v2()
{
    for (int i = 0; i < num_workers; i++) {
        workers[i].thread_id = i;
        workers[i].epoll_fd  = epoll_create(1);

        int pipe_fd[2];
        if (pipe(pipe_fd) < 0) {
            err_sys("pipe error");
        }
        set_nonblocking(pipe_fd[0]);
        set_nonblocking(pipe_fd[1]);

        workers[i].notify_fd      = pipe_fd[0];
        worker_write_fds[i]       = pipe_fd[1];

        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = pipe_fd[0];
        epoll_ctl(workers[i].epoll_fd, EPOLL_CTL_ADD, pipe_fd[0], &ev);

        pthread_create(&workers[i].thread, NULL, worker_loop, &workers[i]);
    }
}

/* ---------- 分发连接给工作线程 ---------- */
static void dispatch_conn(int conn_fd)
{
    /*
     * round-robin：轮流分发给不同工作线程
     * 负载均衡：每个工作线程处理的连接数大致相等
     */
    int target = next_worker;
    next_worker = (next_worker + 1) % num_workers;

    /*
     * 通过 pipe 通知工作线程
     * 写入 conn_fd（4 字节），工作线程读到后加入自己的 epoll
     */
    write(worker_write_fds[target], &conn_fd, sizeof(int));
}

/* ============================================================ */
/*  main —— 主 reactor                                          */
/* ============================================================ */
int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) port = atoi(argv[1]);
    if (argc >= 3) num_workers = atoi(argv[2]);
    if (num_workers > MAX_WORKERS) num_workers = MAX_WORKERS;

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 6：主从 Reactor ===");
    log_info("端口: %d, 工作线程: %d", port, num_workers);

    /* 初始化工作线程 */
    workers_init_v2();

    /* 创建监听 socket */
    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    set_nonblocking(listen_fd);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    Bind(listen_fd, (SA *)&server_addr, sizeof(server_addr));
    Listen(listen_fd, BACKLOG);

    /* 主 reactor 的 epoll，只监听 listen_fd */
    int epfd = epoll_create(1);

    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    log_info("主 reactor 监听 0.0.0.0:%d", port);
    printf("\n>>> %d 个工作线程就绪 <<<\n\n", num_workers);

    struct epoll_event events[MAX_EVENTS];

    /* ---------- 主 reactor 循环：只负责 accept ---------- */
    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            err_sys("epoll_wait error");
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                /* ET 模式：循环 accept */
                for (;;) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(listen_fd,
                                         (SA *)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }

                    /* 分发给工作线程 */
                    dispatch_conn(conn_fd);
                }
            }
        }
    }

    Close(listen_fd);
    Close(epfd);
    return 0;
}