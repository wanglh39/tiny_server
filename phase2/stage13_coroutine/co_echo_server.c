/*
 * co_echo_server.c —— 阶段 13：协程版 echo 服务器
 *
 * ============================================================
 * 对比 phase1/stage4 的 epoll echo 服务器：
 *
 * 回调版（stage4）:
 *   epoll 事件 → 回调 handle_read() → read → write
 *   逻辑分散在回调中，复杂流程需要状态机
 *
 * 协程版（本文件）:
 *   每个连接一个协程，协程函数是线性代码:
 *     while (1) {
 *         n = co_read(fd, buf);    // 等 IO 时自动 yield
 *         co_write(fd, buf, n);    // 等 IO 时自动 yield
 *     }
 *   看起来是同步代码，实际是异步执行
 *
 * 协程 + epoll 工作流程:
 *   1. epoll_wait 返回就绪 fd
 *   2. 找到 fd 对应的协程
 *   3. co_resume(协程)
 *   4. 协程执行 co_read → read 返回 EAGAIN → co_yield
 *   5. 回到 epoll_wait 等下次就绪
 *   6. fd 再次就绪 → co_resume → co_read 成功 → co_write
 *
 * 运行：
 *   build/bin/co_echo_server 8080
 *   echo "hello coroutine" | nc localhost 8080
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#include "coroutine.h"
#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define MAX_EVENTS  256
#define BACKLOG     512
#define BUF_SIZE    4096
#define MAX_CO_CONNS 4096
#define CO_STACK     (64 * 1024)

/* ---------- 协程版 IO ---------- */

/*
 * 协程连接结构
 * 每个连接有自己的协程和缓冲区
 */
typedef struct co_conn {
    int             fd;
    coroutine_t    *co;
    char            buf[BUF_SIZE];
    int             buf_len;     /* 缓冲区已有数据量 */
    int             want_read;   /* 是否需要读 */
    int             want_write;  /* 是否需要写 */
} co_conn_t;

static int   g_epfd;
static co_conn_t *g_conns[MAX_CO_CONNS];

/*
 * co_read —— 协程版 read
 *
 * 如果 read 返回 EAGAIN，yield 让出 CPU。
 * 调度器在 fd 可读后 resume 本协程，重试 read。
 */
static int co_read(coroutine_t *co, int fd, char *buf, int size)
{
    for (;;) {
        int n = read(fd, buf, size);
        if (n > 0) return n;

        if (n == 0) return 0;  /* 对端关闭 */

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 没数据，挂起协程，等调度器在可读时恢复 */
            co_yield(co);
            continue;
        }

        /* 其他错误 */
        return -1;
    }
}

/*
 * co_write —— 协程版 write
 *
 * 如果 write 返回 EAGAIN，yield 让出 CPU。
 */
static int co_write(coroutine_t *co, int fd, const char *buf, int size)
{
    int written = 0;
    while (written < size) {
        int n = write(fd, buf + written, size - written);
        if (n > 0) {
            written += n;
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_yield(co);
            continue;
        }

        return -1;  /* 错误 */
    }
    return written;
}

/* ---------- 连接协程函数 ---------- */

/*
 * echo_routine —— 每个连接的协程函数
 *
 * 这就是协程的魔力：看起来是普通的同步代码，
 * 但 co_read/co_write 内部会在 IO 不就绪时 yield。
 */
static void echo_routine(void *arg)
{
    co_conn_t *conn = (co_conn_t *)arg;
    coroutine_t *self = conn->co;
    char buf[BUF_SIZE];

    log_debug("协程启动 fd=%d", conn->fd);

    for (;;) {
        /* 读数据（IO 不就绪时自动 yield） */
        int n = co_read(self, conn->fd, buf, sizeof(buf));
        if (n <= 0) {
            log_debug("连接关闭 fd=%d n=%d", conn->fd, n);
            break;
        }

        /* Echo：原样写回 */
        co_write(self, conn->fd, buf, n);
    }

    log_debug("协程结束 fd=%d", conn->fd);
}

/* ---------- 工具函数 ---------- */

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void handle_conn(int epfd, co_conn_t *conn)
{
    /*
     * 恢复协程，协程会执行到下次 yield（等 IO）或结束。
     * 协程结束后更新 epoll 监听：
     *   - 如果协程在 co_read 中 yield → 监听 EPOLLIN
     *   - 如果协程在 co_write 中 yield → 监听 EPOLLOUT
     *   - 如果协程结束 → 关闭连接
     */
    co_resume(conn->co);

    if (co_state(conn->co) == CO_DEAD) {
        /* 协程结束，清理连接 */
        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
        close(conn->fd);
        if (conn->fd >= 0 && conn->fd < MAX_CO_CONNS)
            g_conns[conn->fd] = NULL;
        co_free(conn->co);
        free(conn);
    }
    /* 如果协程还活着（SUSPENDED），什么都不做。
       epoll 会在 fd 就绪时再次调用 handle_conn。 */
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 13：协程版 echo 服务器 ===");
    log_info("端口: %d", port);

    /* 创建监听 socket */
    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    set_nonblocking(listen_fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    Bind(listen_fd, (SA *)&addr, sizeof(addr));
    Listen(listen_fd, BACKLOG);

    /* epoll */
    g_epfd = epoll_create(1);
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    log_info("协程 echo server 监听 0.0.0.0:%d", port);
    printf("\n>>> echo \"hello coroutine\" | nc localhost %d <<<\n\n", port);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(g_epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            err_sys("epoll_wait error");
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                /* 新连接：ET 模式循环 accept */
                for (;;) {
                    int conn_fd = accept(listen_fd, NULL, NULL);
                    if (conn_fd < 0) break;
                    set_nonblocking(conn_fd);

                    /* 为每个连接创建协程 */
                    co_conn_t *conn = calloc(1, sizeof(co_conn_t));
                    conn->fd = conn_fd;
                    conn->co = co_create(echo_routine, conn);
                    g_conns[conn_fd] = conn;

                    /* 加入 epoll 监听 */
                    struct epoll_event cev;
                    cev.events  = EPOLLIN | EPOLLET;
                    cev.data.fd = conn_fd;
                    epoll_ctl(g_epfd, EPOLL_CTL_ADD, conn_fd, &cev);

                    log_debug("新连接 fd=%d, 协程 id=%d", conn_fd, conn->co->id);
                }
            } else {
                /* 数据到达：恢复对应协程 */
                int fd = events[i].data.fd;
                if (fd >= 0 && fd < MAX_CO_CONNS && g_conns[fd]) {
                    handle_conn(g_epfd, g_conns[fd]);
                }
            }
        }
    }

    Close(listen_fd);
    Close(g_epfd);
    return 0;
}