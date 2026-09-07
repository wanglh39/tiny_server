/*
 * connection.c —— 连接管理实现
 */

#include "connection.h"
#include "static_file.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#include "../common/log.h"

#define READ_BUF 4096

/* ---------- 连接生命周期 ---------- */

conn_t *conn_create(int fd)
{
    conn_t *conn = calloc(1, sizeof(conn_t));
    conn->fd = fd;
    http_parser_init(&conn->parser);
    return conn;
}

void conn_free(conn_t *conn)
{
    free(conn);
}

/* ---------- 请求分发 ---------- */

static void dispatch_request(int fd, const http_request_t *req,
                             router_t *router, const char *www_root,
                             int worker_id)
{
    /* 1. 尝试路由匹配 */
    route_t *route = router_match(router, req->uri);

    if (route) {
        /* 匹配到路由，调用 handler */
        route->handler(fd, req, (void *)(long)worker_id);
        return;
    }

    /* 2. 没匹配到路由，交给静态文件 */
    serve_static_file(fd, req->uri, www_root);
}

/* ---------- 处理读事件 ---------- */

int conn_handle_read(conn_t *conn, router_t *router,
                     const char *www_root, int worker_id)
{
    char buf[READ_BUF];

    for (;;) {
        ssize_t nread = read(conn->fd, buf, sizeof(buf));

        if (nread > 0) {
            /* 喂给解析器 */
            http_parse_result_t result;
            result = http_parser_feed(&conn->parser, buf, nread);

            if (result == HTTP_PARSE_DONE) {
                /* 解析完成，分发请求 */
                dispatch_request(conn->fd, &conn->parser.request,
                                router, www_root, worker_id);

                /* keep-alive：重置解析器，等下一个请求 */
                http_parser_reset(&conn->parser);

                /* 检查缓冲区是否有残留（粘包） */
                if (conn->parser.buf_len > 0) {
                    http_parse_result_t r2;
                    r2 = http_parser_feed(&conn->parser, "", 0);
                    if (r2 == HTTP_PARSE_DONE) {
                        dispatch_request(conn->fd, &conn->parser.request,
                                        router, www_root, worker_id);
                        http_parser_reset(&conn->parser);
                    }
                }
                /* 继续读，看还有没有数据 */
                continue;

            } else if (result == HTTP_PARSE_ERROR) {
                /* 解析错误 */
                const char *err = "HTTP/1.1 400 Bad Request\r\n"
                                  "Content-Length: 0\r\n\r\n";
                write(conn->fd, err, strlen(err));
                return 0;  /* 关闭连接 */
            }
            /* NEED_MORE：继续 read */

        } else if (nread == 0) {
            /* 客户端关闭 */
            return 0;

        } else {
            /* nread < 0 */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;  /* 读完了，连接存活 */
            }
            if (errno == EINTR) {
                continue;  /* 信号打断，重试 */
            }
            return 0;  /* 其他错误，关闭 */
        }
    }
}