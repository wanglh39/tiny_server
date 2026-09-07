/*
 * reuseport_server.c —— 阶段 11：SO_REUSEPORT 多核利用
 *
 * ============================================================
 * 传统模式（phase1/stage6）：
 *   主线程 accept → pipe 通知 → 工作线程处理
 *   问题：主线程 accept 是瓶颈，跨线程通知有开销
 *
 * SO_REUSEPORT 模式（本章）：
 *   每个线程各自创建 listen socket（设 SO_REUSEPORT）
 *   每个线程自己 epoll + accept + 处理
 *   内核负责把新连接负载均衡到不同线程
 *
 * 优势：
 *   1. 无主线程瓶颈——accept 分散到所有线程
 *   2. 无跨线程通知——不需要 pipe/eventfd
 *   3. 无惊群问题——内核保证只有一个线程被唤醒
 *   4. 更好的缓存局部性——连接在同一个线程内处理
 *
 * 运行：
 *   build/bin/reuseport_server 8080 4
 *   另一个终端: echo "hello" | nc localhost 8080
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define MAX_EVENTS 256
#define BACKLOG    512
#define BUF_SIZE   4096
#define MAX_THREADS 32

/*
 * 工作线程参数
 * 每个线程有自己的 listen_fd 和 epoll_fd
 */
typedef struct {
    int thread_id;
    int listen_fd;     /* 自己的监听 socket */
    int epoll_fd;      /* 自己的 epoll */
    int port;
    int num_threads;
    /* 统计 */
    long accept_count;
    long read_count;
} worker_t;

static int g_running = 1;

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * create_listen_socket —— 创建监听 socket（设 SO_REUSEPORT）
 *
 * 关键：多个线程调用此函数创建各自的 listen socket，
 * 都绑定到同一个端口，内核会负载均衡。
 */
static int create_listen_socket(int port)
{
    int fd = Socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    Setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /*
     * SO_REUSEPORT：允许多个 socket 绑定同一端口
     * 内核收到 SYN 时，从所有绑定该端口的 socket 中
     * 选一个（哈希/轮询）唤醒它的 accept
     */
#ifdef SO_REUSEPORT
    Setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    set_nonblocking(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    Bind(fd, (SA *)&addr, sizeof(addr));
    Listen(fd, BACKLOG);

    return fd;
}

/* ---------- 工作线程主循环 ---------- */
static void *worker_loop(void *arg)
{
    worker_t *w = (worker_t *)arg;

    log_info("线程 #%d 启动 (listen_fd=%d, epoll_fd=%d)",
             w->thread_id, w->listen_fd, w->epoll_fd);

    struct epoll_event events[MAX_EVENTS];
    char buf[BUF_SIZE];

    /*
     * 每个线程的 epoll 同时监听：
     *   1. listen_fd → 新连接
     *   2. conn_fd   → 数据
     *
     * 和 phase1/stage6 的区别：
     *   stage6: 主线程 accept → pipe 通知工作线程
     *   本章:   工作线程自己 accept，不需要通知
     */
    for (;;) {
        if (!g_running) break;

        int n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == w->listen_fd) {
                /* 新连接：自己 accept */
                for (;;) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(w->listen_fd,
                                        (SA *)&client_addr, &client_len);
                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }

                    w->accept_count++;
                    set_nonblocking(conn_fd);

                    struct epoll_event ev;
                    ev.events   = EPOLLIN | EPOLLET;
                    ev.data.fd  = conn_fd;
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);

                    log_debug("线程 #%d accept fd=%d (第 %ld 个连接)",
                              w->thread_id, conn_fd, w->accept_count);
                }
            } else {
                /* 数据到达：echo */
                int fd = events[i].data.fd;

                for (;;) {
                    ssize_t nread = read(fd, buf, sizeof(buf));
                    if (nread > 0) {
                        w->read_count++;
                        write(fd, buf, nread);  /* echo */
                    } else if (nread == 0) {
                        /* 对端关闭 */
                        Close(fd);
                        epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        Close(fd);
                        epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        break;
                    }
                }
            }
        }
    }

    log_info("线程 #%d 退出 (accept=%ld, read=%ld)",
             w->thread_id, w->accept_count, w->read_count);
    return NULL;
}

/* ---------- 信号处理 ---------- */
static void on_sigint(int sig)
{
    g_running = 0;
}

/* ---------- main ---------- */
int main(int argc, char *argv[])
{
    int port = 8080;
    int num_threads = 4;
    if (argc >= 2) port = atoi(argv[1]);
    if (argc >= 3) num_threads = atoi(argv[2]);
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    log_info("=== 阶段 11：SO_REUSEPORT 多核 ===");
    log_info("端口: %d, 线程: %d", port, num_threads);

#ifndef SO_REUSEPORT
    fprintf(stderr, "警告：系统不支持 SO_REUSEPORT，将退化为 SO_REUSEADDR\n");
    fprintf(stderr, "（多个线程绑定同一端口会失败）\n");
#endif

    /*
     * 每个线程创建自己的 listen socket + epoll
     *
     * 对比 phase1/stage6 的初始化：
     *   stage6: 1 个 listen_fd + N 个 pipe + N 个 epoll
     *   本章:   N 个 listen_fd + N 个 epoll（无 pipe）
     */
    worker_t workers[MAX_THREADS];
    pthread_t threads[MAX_THREADS];

    for (int i = 0; i < num_threads; i++) {
        workers[i].thread_id   = i;
        workers[i].port        = port;
        workers[i].num_threads = num_threads;
        workers[i].accept_count = 0;
        workers[i].read_count   = 0;

        /* 每个线程创建自己的 listen socket */
        workers[i].listen_fd = create_listen_socket(port);
        workers[i].epoll_fd  = epoll_create(1);

        /* 把 listen_fd 加入自己的 epoll */
        struct epoll_event ev;
        ev.events  = EPOLLIN | EPOLLET;
        ev.data.fd = workers[i].listen_fd;
        epoll_ctl(workers[i].epoll_fd, EPOLL_CTL_ADD,
                  workers[i].listen_fd, &ev);

        /* 启动线程 */
        pthread_create(&threads[i], NULL, worker_loop, &workers[i]);
    }

    log_info("所有线程已启动，监听 0.0.0.0:%d", port);
    printf("\n>>> echo \"hello\" | nc localhost %d <<<\n", port);
    printf(">>> Ctrl+C 退出 <<<\n\n");

    /* 等待所有线程退出 */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        close(workers[i].listen_fd);
        close(workers[i].epoll_fd);
    }

    /* 打印统计 */
    printf("\n=== 统计 ===\n");
    long total_accept = 0, total_read = 0;
    for (int i = 0; i < num_threads; i++) {
        printf("线程 #%d: accept=%ld, echo=%ld\n",
               i, workers[i].accept_count, workers[i].read_count);
        total_accept += workers[i].accept_count;
        total_read   += workers[i].read_count;
    }
    printf("总计: accept=%ld, echo=%ld\n", total_accept, total_read);

    return 0;
}