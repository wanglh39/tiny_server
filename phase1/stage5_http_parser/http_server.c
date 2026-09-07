/*
 * http_server.c —— 阶段 5：用状态机解析 HTTP 请求的 server
 *
 * ============================================================
 * 组合 stage4 的 epoll ET + stage5 的 http_parser
 * ============================================================
 *
 * 流程：
 *   epoll_wait → 有数据 → read → 喂给 http_parser
 *     → NEED_MORE：继续 epoll_wait
 *     → DONE：构造 HTTP 响应，write 回去
 *     → ERROR：关闭连接
 *
 * 教学要点：
 *   1. 增量解析：read 多少就喂多少，不用等"完整请求"
 *   2. 粘包处理：解析完一个请求后，剩余数据是下一个请求的
 *   3. keep-alive：HTTP/1.1 默认复用连接，不关 fd
 *
 * 运行：
 *   build/bin/http_server 8080
 *   浏览器访问 http://localhost:8080/
 *   或 curl http://localhost:8080/
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
#include "http_parser.h"

#define MAX_EVENTS 1024
#define BACKLOG    128
#define READ_BUF   4096

/* 每个连接关联一个 http_parser */
/*
 * 为什么需要 per-connection 的 parser？
 *   每个连接的请求是独立解析的
 *   一个连接可能分多次 read 才收完一个请求
 *   parser 需要记住"上次解析到哪了"
 *
 * 怎么关联？
 *   epoll_event.data 是一个 union，可以存 fd 或 ptr
 *   我们存 ptr，指向一个 conn_t 结构体
 */
typedef struct {
    int             fd;
    http_parser_t   parser;
} conn_t;

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * build_response —— 构造 HTTP 响应
 *   把请求的信息回显给客户端，方便观察
 */
static int build_response(const http_request_t *req, char *buf, int bufsize)
{
    const char *method = http_method_str(req->method);

    /*
     * HTTP 响应格式：
     *   HTTP/1.1 200 OK\r\n
     *   Content-Type: text/plain\r\n
     *   Content-Length: N\r\n
     *   \r\n
     *   body
     */
    char body[2048];
    int body_len = snprintf(body, sizeof(body),
        "Hello from tiny_server (stage5)!\r\n"
        "\r\n"
        "=== 你发送的请求 ===\r\n"
        "Method:         %s\r\n"
        "URI:            %s\r\n"
        "Version:        %s\r\n"
        "Host:           %s\r\n"
        "Content-Length: %d\r\n"
        "Body:           %.*s\r\n",
        method,
        req->uri,
        req->version,
        req->host,
        req->content_length,
        req->body_len, req->body);

    int total = snprintf(buf, bufsize,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        body_len, body);

    return total;
}

int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 5：HTTP 状态机 server ===");
    log_info("端口: %d", port);

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

    int epfd = epoll_create(1);

    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    log_info("HTTP server 监听 0.0.0.0:%d", port);
    printf("\n>>> curl http://localhost:%d/ 测试 <<<\n\n", port);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            err_sys("epoll_wait error");
        }

        for (int i = 0; i < n; i++) {
            /*
             * data 是 union：listen_fd 事件存 fd，conn 事件存 ptr
             * 用 data.fd == listen_fd 区分（listen_fd=3，堆指针远大于此）
             */
            if (events[i].data.fd == listen_fd) {
                int fd = listen_fd;
                /* accept 循环（ET 模式） */
                for (;;) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(listen_fd, (SA *)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }

                    set_nonblocking(conn_fd);

                    /* 为每个连接创建 conn_t */
                    conn_t *conn = calloc(1, sizeof(conn_t));
                    conn->fd = conn_fd;
                    http_parser_init(&conn->parser);

                    ev.events  = EPOLLIN | EPOLLET;
                    ev.data.ptr = conn;  /* 存指针而不是 fd */
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev);
                }

            } else {
                /* 数据到达 */
                conn_t *conn = events[i].data.ptr;
                int fd = conn->fd;  /* 从 conn 获取真正的 fd */

                char buf[READ_BUF];
                for (;;) {
                    ssize_t nread = read(fd, buf, sizeof(buf));
                    if (nread > 0) {
                        /* 喂给解析器 */
                        http_parse_result_t result;
                        result = http_parser_feed(&conn->parser, buf, nread);

                        if (result == HTTP_PARSE_DONE) {
                            /* 解析完成，构造响应 */
                            char response[4096];
                            int resp_len = build_response(
                                &conn->parser.request,
                                response, sizeof(response));

                            write(fd, response, resp_len);

                            /*
                             * keep-alive：不关连接，重置解析器等下一个请求
                             * 剩余数据（粘包）保留在 parser 的缓冲区里
                             */
                            http_parser_reset(&conn->parser);

                            /*
                             * 可能还有完整的请求在缓冲区里
                             * 尝试再解析一次
                             */
                            if (conn->parser.buf_len > 0) {
                                http_parse_result_t r2;
                                r2 = http_parser_feed(&conn->parser, "", 0);
                                if (r2 == HTTP_PARSE_DONE) {
                                    char resp2[4096];
                                    int len2 = build_response(
                                        &conn->parser.request,
                                        resp2, sizeof(resp2));
                                    write(fd, resp2, len2);
                                    http_parser_reset(&conn->parser);
                                }
                            }

                        } else if (result == HTTP_PARSE_ERROR) {
                            /* 解析错误，关闭连接 */
                            log_warn("HTTP 解析错误，关闭 fd=%d", fd);
                            Close(fd);
                            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                            free(conn);
                            break;
                        }
                        /* NEED_MORE：继续 read */

                    } else if (nread == 0) {
                        /* 客户端关闭 */
                        Close(fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        free(conn);
                        break;

                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        Close(fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        free(conn);
                        break;
                    }
                }
            }
        }
    }

    Close(listen_fd);
    Close(epfd);
    return 0;
}