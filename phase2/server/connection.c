/*
 * connection.c —— 连接管理与协议分发
 *
 * ============================================================
 *  每个连接的生命周期：
 *    1. accept → conn_create（内存池分配）
 *    2. 读前几字节 → detect_protocol 检测协议
 *    3. 根据协议分发：
 *       - TLS → tls_do_handshake → SSL_read → HTTP 解析
 *       - HTTP/2 → 验证前言 → 帧解析 → HPACK 解码
 *       - HTTP/1.1 → http_parser → 检查 Upgrade: websocket
 *       - WebSocket → ws_parse_frame → ws_handle_message
 *    4. keep-alive 或关闭
 *
 *  协议检测原理：
 *    TLS ClientHello:  buf[0]==0x16 && buf[1]==0x03
 *    HTTP/2 前言:      "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
 *    HTTP/1.1:         "GET / HTTP/1.1\r\n..." 等
 * ============================================================
 */

#define _GNU_SOURCE
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>

#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>

/* HTTP/2 连接前言 */
static const char H2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/* ---------- 连接创建/释放 ---------- */

conn_t *conn_create(int fd, int worker_id)
{
    /* 用内存池分配连接结构 */
    pool_t *pool = pool_create(4096);
    if (!pool) return NULL;

    conn_t *conn = pool_calloc(pool, sizeof(conn_t));
    if (!conn) {
        pool_destroy(pool);
        return NULL;
    }

    conn->fd        = fd;
    conn->proto     = PROTO_DETECTING;
    conn->worker_id = worker_id;
    conn->pool      = pool;

    http_parser_init(&conn->parser);
    return conn;
}

void conn_free(conn_t *conn)
{
    if (!conn) return;

    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }

    if (conn->pool) {
        pool_destroy(conn->pool);
    }
}

/* ---------- 协议检测 ---------- */

proto_type_t detect_protocol(const char *buf, int len)
{
    if (len < 1) return PROTO_DETECTING;

    /*
     * TLS ClientHello 格式：
     *   0x16 = handshake record type
     *   0x03 = TLS 版本 (1.0/1.1/1.2/1.3 都是 0x03xx)
     */
    if (len >= 2 && (uint8_t)buf[0] == 0x16 && (uint8_t)buf[1] == 0x03) {
        return PROTO_TLS;
    }

    /*
     * HTTP/2 连接前言（24 字节）：
     *   "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
     */
    if (len >= H2_PREFACE_LEN &&
        memcmp(buf, H2_PREFACE, H2_PREFACE_LEN) == 0) {
        return PROTO_HTTP2;
    }

    /* 否则按 HTTP/1.1 处理（可能升级到 WebSocket） */
    return PROTO_HTTP1;
}

/* ---------- HTTP/1.1 请求处理 ---------- */

static void send_http_response(int fd, int status, const char *status_str,
                               const char *content_type,
                               const char *body, int body_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "Server: advanced_server\r\n"
        "\r\n",
        status, status_str, content_type, body_len);
    write(fd, header, hlen);
    if (body && body_len > 0) {
        write(fd, body, body_len);
    }
}

static void send_http_error(int fd, int status, const char *status_str,
                            const char *message)
{
    char body[512];
    int blen = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><title>%d %s</title></head>"
        "<body><h1>%d %s</h1><p>%s</p>"
        "<p><a href=\"/\">返回首页</a></p>"
        "</body></html>",
        status, status_str, status, status_str, message);
    send_http_response(fd, status, status_str, "text/html", body, blen);
}

/* 检查 HTTP 请求是否是 WebSocket 升级请求 */
static int check_websocket_upgrade(const http_parser_t *parser)
{
    /* 从原始缓冲区中搜索 Upgrade: websocket */
    char *p = strcasestr(parser->buf, "Upgrade:");
    if (!p) return 0;
    p += 8;  /* 跳过 "Upgrade:" */
    while (*p == ' ' || *p == '\t') p++;
    return strncasecmp(p, "websocket", 9) == 0;
}

/* 从原始缓冲区提取 Sec-WebSocket-Key */
static int extract_ws_key(const http_parser_t *parser, char *key, int key_size)
{
    char *p = strcasestr(parser->buf, "Sec-WebSocket-Key:");
    if (!p) return 0;
    p += 18;  /* 跳过 "Sec-WebSocket-Key:" */
    while (*p == ' ' || *p == '\t') p++;
    char *end = strstr(p, "\r\n");
    if (!end) end = p + strlen(p);
    int len = end - p;
    if (len >= key_size) len = key_size - 1;
    memcpy(key, p, len);
    key[len] = '\0';
    return 1;
}

/* HTTP/1.1 请求分发 */
static void handle_http1_request(conn_t *conn, worker_t *w)
{
    http_request_t *req = &conn->parser.request;
    w->request_count++;

    /* 检查 WebSocket 升级 */
    if (check_websocket_upgrade(&conn->parser)) {
        /* 提取 Sec-WebSocket-Key 并完成握手 */
        char ws_key[256] = "";
        if (extract_ws_key(&conn->parser, ws_key, sizeof(ws_key))) {
            /* 完成 WebSocket 握手 */
            char response[512];
            /* compute_accept_key 在 ws_conn.c 中实现 */
            extern char *ws_compute_accept_key(const char *client_key);
            char *accept_key = ws_compute_accept_key(ws_key);
            if (accept_key) {
                int len = snprintf(response, sizeof(response),
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: %s\r\n"
                    "\r\n", accept_key);
                write(conn->fd, response, len);
                free(accept_key);

                conn->proto     = PROTO_WEBSOCKET;
                conn->ws_state  = WS_STATE_CONNECTED;

                if (w->cfg->log_file[0]) {
                    alog_info("WebSocket 握手完成 fd=%d", conn->fd);
                }
                return;
            }
        }
    }

    /* 普通 HTTP/1.1 请求 */
    if (strcmp(req->uri, "/api/info") == 0) {
        char body[512];
        int blen = snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"proto\":\"http1\",\"worker\":%d,"
            "\"method\":\"%s\",\"uri\":\"%s\"}",
            conn->worker_id, http_method_str(req->method), req->uri);
        send_http_response(conn->fd, 200, "OK", "application/json", body, blen);
    } else if (strcmp(req->uri, "/api/echo") == 0) {
        char body[1024];
        int blen = snprintf(body, sizeof(body),
            "<!DOCTYPE html><html><body>"
            "<h1>Echo (HTTP/1.1)</h1>"
            "<p>Method: %s</p><p>URI: %s</p>"
            "<p>Worker: %d</p>"
            "</body></html>",
            http_method_str(req->method), req->uri, conn->worker_id);
        send_http_response(conn->fd, 200, "OK", "text/html", body, blen);
    } else {
        /* 静态文件 */
        serve_static_file(conn->fd, req->uri, w->cfg->www_root);
    }
}

/* ---------- HTTP/1.1 读处理 ---------- */

static int handle_http1_read(conn_t *conn, worker_t *w)
{
    char buf[4096];

    for (;;) {
        ssize_t nread = read(conn->fd, buf, sizeof(buf));
        if (nread > 0) {
            http_parse_result_t result = http_parser_feed(&conn->parser, buf, nread);

            if (result == HTTP_PARSE_DONE) {
                handle_http1_request(conn, w);

                /* WebSocket 升级后不再重置 parser */
                if (conn->proto == PROTO_WEBSOCKET) {
                    return 1;  /* 连接存活，等 WebSocket 帧 */
                }

                /* keep-alive：重置 parser */
                http_parser_reset(&conn->parser);

                /* 处理粘包 */
                if (conn->parser.buf_len > 0) {
                    http_parse_result_t r2 = http_parser_feed(&conn->parser, "", 0);
                    if (r2 == HTTP_PARSE_DONE) {
                        handle_http1_request(conn, w);
                        http_parser_reset(&conn->parser);
                    }
                }
                continue;
            } else if (result == HTTP_PARSE_ERROR) {
                send_http_error(conn->fd, 400, "Bad Request", "请求解析失败");
                return 0;
            }
            /* NEED_MORE：继续读 */
        } else if (nread == 0) {
            return 0;  /* 客户端关闭 */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            if (errno == EINTR) continue;
            return 0;
        }
    }
}

/* ---------- WebSocket 读处理 ---------- */

static int handle_websocket_read(conn_t *conn, worker_t *w)
{
    char buf[4096];

    for (;;) {
        ssize_t nread = read(conn->fd, buf, sizeof(buf));
        if (nread > 0) {
            int opcode;
            char *payload;
            int payload_len;

            int frame_len = ws_parse_frame(buf, nread, &opcode, &payload, &payload_len);
            if (frame_len > 0) {
                ws_handle_message(conn, opcode, payload, payload_len);
                /* 继续读下一帧 */
                continue;
            } else if (frame_len == 0) {
                /* 数据不够，等下次 */
                return 1;
            } else {
                /* 解析错误 */
                return 0;
            }
        } else if (nread == 0) {
            return 0;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            if (errno == EINTR) continue;
            return 0;
        }
    }
}

/* ---------- TLS 读处理 ---------- */

static int handle_tls_read(conn_t *conn, worker_t *w)
{
    /* TLS 握手 */
    if (!conn->tls_handshake_done) {
        int ret = tls_do_handshake(conn, w->ssl_ctx);
        if (ret == 0) {
            /* 握手未完成，需要更多数据 */
            return 1;
        } else if (ret < 0) {
            /* 握手失败 */
            return 0;
        }
        /* 握手成功 */
        conn->tls_handshake_done = 1;
    }

    /* SSL_read + HTTP 解析 */
    char buf[4096];

    for (;;) {
        int nread = SSL_read(conn->ssl, buf, sizeof(buf));
        if (nread > 0) {
            http_parse_result_t result = http_parser_feed(&conn->parser, buf, nread);

            if (result == HTTP_PARSE_DONE) {
                /* HTTPS 请求处理 */
                http_request_t *req = &conn->parser.request;
                w->request_count++;

                if (strcmp(req->uri, "/api/info") == 0) {
                    char body[512];
                    int blen = snprintf(body, sizeof(body),
                        "{\"status\":\"ok\",\"proto\":\"https\",\"worker\":%d}",
                        conn->worker_id);
                    /* 用 SSL_write 发送 */
                    char header[256];
                    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: %d\r\n\r\n", blen);
                    SSL_write(conn->ssl, header, hlen);
                    SSL_write(conn->ssl, body, blen);
                } else {
                    serve_static_file_ssl(conn->ssl, req->uri, w->cfg->www_root);
                }

                http_parser_reset(&conn->parser);
                continue;
            } else if (result == HTTP_PARSE_ERROR) {
                const char *err = "HTTP/1.1 400 Bad Request\r\n\r\n";
                SSL_write(conn->ssl, err, strlen(err));
                return 0;
            }
        } else {
            int err = SSL_get_error(conn->ssl, nread);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return 1;  /* 需要更多数据 */
            }
            return 0;  /* 连接关闭或错误 */
        }
    }
}

/* ---------- HTTP/2 读处理 ---------- */

static int handle_http2_read(conn_t *conn, worker_t *w)
{
    char buf[4096];

    for (;;) {
        ssize_t nread = read(conn->fd, buf, sizeof(buf));
        if (nread > 0) {
            int ret = h2_handle_data(conn, buf, nread);
            if (ret < 0) return 0;
            /* ret >= 0: 继续读 */
        } else if (nread == 0) {
            return 0;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            if (errno == EINTR) continue;
            return 0;
        }
    }
}

/* ---------- 连接读事件入口 ---------- */

int conn_handle_read(conn_t *conn, worker_t *w)
{
    /* 协议检测阶段 */
    if (conn->proto == PROTO_DETECTING) {
        /* 先 peek 前几字节（不消耗数据） */
        char peek[24];
        ssize_t n = recv(conn->fd, peek, sizeof(peek), MSG_PEEK);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return 0;
        }

        proto_type_t proto = detect_protocol(peek, n);
        conn->proto = proto;

        switch (proto) {
        case PROTO_TLS:
            if (w->cfg->log_file[0])
                alog_debug("fd=%d 检测到 TLS", conn->fd);
            return handle_tls_read(conn, w);

        case PROTO_HTTP2:
            if (w->cfg->log_file[0])
                alog_debug("fd=%d 检测到 HTTP/2", conn->fd);
            return handle_http2_read(conn, w);

        case PROTO_HTTP1:
        default:
            return handle_http1_read(conn, w);
        }
    }

    /* 已检测协议，按类型分发 */
    switch (conn->proto) {
    case PROTO_HTTP1:
        return handle_http1_read(conn, w);
    case PROTO_TLS:
        return handle_tls_read(conn, w);
    case PROTO_WEBSOCKET:
        return handle_websocket_read(conn, w);
    case PROTO_HTTP2:
        return handle_http2_read(conn, w);
    default:
        return 0;
    }
}

int conn_handle_write(conn_t *conn, worker_t *w)
{
    (void)conn; (void)w;
    return 1;
}