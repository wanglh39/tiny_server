/*
 * server.c —— 聚合版服务器主程序
 *
 * ============================================================
 * 把所有阶段整合成一个完整的服务器：
 *   stage4 epoll ET + stage5 HTTP 解析 + stage6 Reactor + stage7 静态文件
 * ============================================================
 *
 * 架构：
 *   main → 解析配置 → 初始化日志 → 设置路由 → 启动工作线程
 *        → 创建监听 socket → 主 reactor 循环（accept + 分发）
 *
 * 信号处理：
 *   SIGINT/SIGTERM → 优雅退出（停止 accept，等待工作线程）
 *
 * 用法：
 *   build/bin/server -p 8080 -w 4 -r ./www
 *   build/bin/server -f server.conf
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

#include "../common/error.h"
#include "../common/log.h"
#include "../common/wrap_posix.h"

#include "config.h"
#include "router.h"
#include "worker.h"
#include "connection.h"
#include "http_parser.h"

/* ---------- 优雅退出 ---------- */
static volatile int g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ---------- 内置 API handler ---------- */

/* /api/info → JSON 格式的服务器信息 */
static void handle_info(int fd, const http_request_t *req, void *ud)
{
    int worker_id = (int)(long)ud;
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"worker\":%d,\"method\":\"%s\",\"uri\":\"%s\"}",
        worker_id, http_method_str(req->method), req->uri);

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: keep-alive\r\n\r\n", body_len);
    write(fd, header, hlen);
    write(fd, body, body_len);
}

/* /api/echo → 回显请求信息（HTML） */
static void handle_echo(int fd, const http_request_t *req, void *ud)
{
    (void)ud;
    char body[2048];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><body>"
        "<h1>Echo</h1><table>"
        "<tr><td>Method</td><td>%s</td></tr>"
        "<tr><td>URI</td><td>%s</td></tr>"
        "<tr><td>Host</td><td>%s</td></tr>"
        "<tr><td>Content-Length</td><td>%d</td></tr>"
        "</table></body></html>",
        http_method_str(req->method), req->uri, req->host, req->content_length);

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        "Content-Length: %d\r\nConnection: keep-alive\r\n\r\n", body_len);
    write(fd, header, hlen);
    write(fd, body, body_len);
}

/* ---------- 设置路由表 ---------- */
static void setup_routes(router_t *router)
{
    router_init(router);

    /*
     * 路由规则：
     *   /api/info  → 精确匹配，返回 JSON
     *   /api/echo  → 精确匹配，回显请求
     *   /api/*     → 前缀匹配，默认 API 响应
     *   其他       → 静态文件
     */
    router_add(router, "/api/info", ROUTE_EXACT,  handle_info);
    router_add(router, "/api/echo", ROUTE_EXACT,  handle_echo);
    router_add(router, "/api/",    ROUTE_PREFIX, handle_info);
}

/* ---------- main ---------- */
int main(int argc, char *argv[])
{
    /* 1. 解析配置 */
    server_config_t cfg;
    config_init(&cfg);
    config_parse_args(&cfg, argc, argv);

    /* 2. 初始化日志 */
    if (cfg.log_file[0]) {
        log_open_file(cfg.log_file);
    }
    log_set_level(LOG_INFO);  /* 生产时用 INFO，调试时用 DEBUG */

    /* 3. 打印启动信息 */
    printf("\n");
    printf("==========================================\n");
    printf("  tiny_server - 聚合版 Web 服务器\n");
    printf("==========================================\n");
    config_print(&cfg);
    printf("==========================================\n\n");

    log_info("服务器启动");

    /* 4. 设置信号处理 */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);  /* 忽略 SIGPIPE（对端关闭时 write 不崩溃） */

    /* 5. 设置路由表 */
    router_t router;
    setup_routes(&router);

    /* 6. 启动工作线程 */
    worker_t workers[MAX_WORKERS];
    workers_start(workers, cfg.num_workers, &router, cfg.www_root);

    /* 7. 创建监听 socket */
    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* 非阻塞（ET 模式要求） */
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(cfg.port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    Bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    Listen(listen_fd, cfg.backlog);

    /* 8. 主 reactor：epoll 只监听 listen_fd */
    int epfd = epoll_create(1);

    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    log_info("主 reactor 监听 0.0.0.0:%d", cfg.port);
    printf(">>> curl http://localhost:%d/ <<<\n", cfg.port);
    printf(">>> Ctrl-C 优雅退出 <<<\n\n");

    /* 9. 主循环：只负责 accept + 分发 */
    struct epoll_event events[64];

    while (g_running) {
        int n = epoll_wait(epfd, events, 64, 1000);  /* 1 秒超时，检查 g_running */
        if (n < 0) {
            if (errno == EINTR) continue;
            err_sys("epoll_wait error");
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd != listen_fd) continue;

            /* ET 模式：循环 accept */
            for (;;) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int conn_fd = accept(listen_fd,
                                     (struct sockaddr *)&client_addr,
                                     &client_len);

                if (conn_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    log_error("accept error: %s", strerror(errno));
                    break;
                }

                /* 分发给工作线程 */
                worker_dispatch(workers, cfg.num_workers, conn_fd);
            }
        }
    }

    /* 10. 优雅退出 */
    printf("\n>>> 收到退出信号，正在关闭... <<<\n");
    Close(listen_fd);
    Close(epfd);

    /* 等待工作线程（实际上它们是无限循环，这里简单关闭） */
    for (int i = 0; i < cfg.num_workers; i++) {
        close(workers[i].notify_fd);
        close(workers[i].write_fd);
    }

    log_info("服务器已关闭");
    printf(">>> 再见 <<<\n");
    return 0;
}