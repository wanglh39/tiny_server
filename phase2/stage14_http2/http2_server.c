/*
 * http2_server.c —— 阶段 14：HTTP/2 服务器
 *
 * ============================================================
 * HTTP/2 (RFC 7540) 核心特性：
 *
 * 1. 二进制帧：不再是文本协议，所有数据都是二进制帧
 * 2. 多路复用：一条 TCP 上并行多个请求/响应（Stream）
 * 3. 头部压缩：HPACK 算法压缩重复的 HTTP 头部
 * 4. 服务器推送：服务器主动推送资源
 * 5. 流优先级：客户端指定请求优先级
 *
 * HTTP/2 帧格式（9 字节头 + payload）：
 *   ┌─────────────────┬─────────┬─────────┬───────────────────┐
 *   │ Length (24 bit)  │ Type(8) │ Flags(8)│ Stream ID (31 bit)│
 *   │                  │         │         │ + Reserved (1 bit)│
 *   └─────────────────┴─────────┴─────────┴───────────────────┘
 *   │                    Payload (Length bytes)                │
 *   └──────────────────────────────────────────────────────────┘
 *
 * 帧类型：
 *   0x0: DATA      0x1: HEADERS    0x2: PRIORITY
 *   0x3: RST_STREAM 0x4: SETTINGS  0x5: PUSH_PROMISE
 *   0x6: PING       0x7: GOAWAY    0x8: WINDOW_UPDATE
 *   0x9: CONTINUATION
 *
 * 运行：
 *   build/bin/http2_server 8080
 *   curl --http2-prior-knowledge http://localhost:8080/
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define MAX_EVENTS  256
#define BACKLOG     512
#define BUF_SIZE    65536
#define MAX_STREAMS 128

/* ---------- HTTP/2 帧类型 ---------- */
enum {
    HTTP2_DATA          = 0x0,
    HTTP2_HEADERS       = 0x1,
    HTTP2_PRIORITY      = 0x2,
    HTTP2_RST_STREAM    = 0x3,
    HTTP2_SETTINGS      = 0x4,
    HTTP2_PUSH_PROMISE  = 0x5,
    HTTP2_PING          = 0x6,
    HTTP2_GOAWAY        = 0x7,
    HTTP2_WINDOW_UPDATE = 0x8,
    HTTP2_CONTINUATION  = 0x9,
};

/* ---------- 帧标志 ---------- */
enum {
    FLAG_END_STREAM  = 0x1,
    FLAG_ACK         = 0x1,
    FLAG_END_HEADERS = 0x4,
    FLAG_PADDED      = 0x8,
    FLAG_PRIORITY    = 0x20,
};

/* ---------- HTTP/2 帧头（9 字节） ---------- */
typedef struct {
    uint32_t length;    /* payload 长度（24 位） */
    uint8_t  type;      /* 帧类型 */
    uint8_t  flags;     /* 标志位 */
    uint32_t stream_id; /* 流 ID（31 位） */
} h2_frame_hdr_t;

/* ---------- 流状态 ---------- */
typedef enum {
    STREAM_IDLE,
    STREAM_OPEN,
    STREAM_HALF_CLOSED,
    STREAM_CLOSED,
} stream_state_t;

typedef struct {
    uint32_t       id;
    stream_state_t state;
    char           method[16];
    char           path[256];
    char           host[128];
    int            headers_done;
} h2_stream_t;

/* ---------- 连接 ---------- */
typedef struct {
    int         fd;
    int         preface_done;
    h2_stream_t streams[MAX_STREAMS];
    int         stream_count;
} h2_conn_t;

static h2_conn_t *g_conns[1024];

/* ---------- 工具函数 ---------- */
static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------- HTTP/2 连接前言 ---------- */
static const char H2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
#define H2_PREFACE_LEN 24

/* ---------- 帧解析/生成 ---------- */

static int parse_frame_hdr(const uint8_t *buf, h2_frame_hdr_t *hdr)
{
    hdr->length = ((uint32_t)buf[0] << 16) |
                  ((uint32_t)buf[1] << 8)  |
                  (uint32_t)buf[2];
    hdr->type      = buf[3];
    hdr->flags     = buf[4];
    hdr->stream_id = ((uint32_t)(buf[5] & 0x7F) << 24) |
                     ((uint32_t)buf[6] << 16) |
                     ((uint32_t)buf[7] << 8)  |
                     (uint32_t)buf[8];
    return 0;
}

static int build_frame_hdr(uint8_t *buf, uint32_t length,
                           uint8_t type, uint8_t flags,
                           uint32_t stream_id)
{
    buf[0] = (length >> 16) & 0xFF;
    buf[1] = (length >> 8)  & 0xFF;
    buf[2] = length & 0xFF;
    buf[3] = type;
    buf[4] = flags;
    buf[5] = (stream_id >> 24) & 0x7F;
    buf[6] = (stream_id >> 16) & 0xFF;
    buf[7] = (stream_id >> 8)  & 0xFF;
    buf[8] = stream_id & 0xFF;
    return 9;
}

static int h2_send_frame(int fd, uint8_t type, uint8_t flags,
                         uint32_t stream_id,
                         const uint8_t *payload, uint32_t payload_len)
{
    uint8_t hdr[9];
    build_frame_hdr(hdr, payload_len, type, flags, stream_id);

    uint8_t frame[BUF_SIZE];
    memcpy(frame, hdr, 9);
    if (payload_len > 0 && payload) {
        memcpy(frame + 9, payload, payload_len);
    }

    int total = 9 + payload_len;
    int written = 0;
    while (written < total) {
        int n = write(fd, frame + written, total - written);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        written += n;
    }
    return 0;
}

/* ---------- SETTINGS 帧 ---------- */

static int send_settings(int fd)
{
    return h2_send_frame(fd, HTTP2_SETTINGS, 0, 0, NULL, 0);
}

static int send_settings_ack(int fd)
{
    return h2_send_frame(fd, HTTP2_SETTINGS, FLAG_ACK, 0, NULL, 0);
}

/* ---------- HPACK 简化实现 ---------- */

/* HPACK 静态表（RFC 7541 Appendix A，简化版） */
typedef struct { const char *name; const char *value; } hpack_entry_t;

static const hpack_entry_t hpack_static_table[] = {
    {NULL, NULL},              /* 0: 保留 */
    {":authority", ""},        /* 1 */
    {":method", "GET"},        /* 2 */
    {":method", "POST"},       /* 3 */
    {":path", "/"},            /* 4 */
    {":path", "/index.html"},  /* 5 */
    {":scheme", "http"},       /* 6 */
    {":scheme", "https"},      /* 7 */
    {":status", "200"},        /* 8 */
    {":status", "204"},        /* 9 */
    {":status", "206"},        /* 10 */
    {":status", "304"},        /* 11 */
    {":status", "400"},        /* 12 */
    {":status", "404"},        /* 13 */
    {"accept-charset", ""},    /* 14 */
    {"accept-encoding", "gzip, deflate"}, /* 15 */
    {"accept-language", ""},   /* 16 */
    {"accept-ranges", ""},     /* 17 */
    {"accept", ""},            /* 18 */
    {"access-control-allow-origin", ""}, /* 19 */
    {"age", ""},               /* 20 */
    {"allow", ""},             /* 21 */
    {"authorization", ""},     /* 22 */
    {"cache-control", ""},     /* 23 */
    {"content-disposition", ""}, /* 24 */
    {"content-encoding", ""},  /* 25 */
    {"content-language", ""},  /* 26 */
    {"content-length", ""},    /* 27 */
    {"content-location", ""},  /* 28 */
    {"content-range", ""},     /* 29 */
    {"content-type", ""},      /* 30 */
    {"cookie", ""},            /* 31 */
    {"date", ""},              /* 32 */
    {"etag", ""},              /* 33 */
    {"expect", ""},            /* 34 */
    {"expires", ""},           /* 35 */
    {"from", ""},              /* 36 */
    {"host", ""},              /* 37 */
    {"if-match", ""},          /* 38 */
    {"if-modified-since", ""}, /* 39 */
    {"if-none-match", ""},     /* 40 */
    {"if-range", ""},          /* 41 */
    {"if-unmodified-since", ""}, /* 42 */
    {"last-modified", ""},     /* 43 */
    {"link", ""},              /* 44 */
    {"location", ""},          /* 45 */
    {"max-forwards", ""},      /* 46 */
    {"proxy-authenticate", ""}, /* 47 */
    {"proxy-authorization", ""}, /* 48 */
    {"range", ""},             /* 49 */
    {"referer", ""},           /* 50 */
    {"refresh", ""},           /* 51 */
    {"retry-after", ""},       /* 52 */
    {"server", ""},            /* 53 */
    {"set-cookie", ""},        /* 54 */
    {"strict-transport-security", ""}, /* 55 */
    {"transfer-encoding", ""}, /* 56 */
    {"user-agent", ""},        /* 57 */
    {"vary", ""},              /* 58 */
    {"via", ""},               /* 59 */
    {"www-authenticate", ""},  /* 60 */
};
#define HPACK_STATIC_TABLE_SIZE (sizeof(hpack_static_table) / sizeof(hpack_static_table[0]))

static int hpack_apply_header(h2_stream_t *stream, const char *name, const char *value)
{
    log_debug("  头部: %s: %s", name, value);
    if (strcasecmp(name, ":method") == 0)
        strncpy(stream->method, value, sizeof(stream->method) - 1);
    else if (strcasecmp(name, ":path") == 0)
        strncpy(stream->path, value, sizeof(stream->path) - 1);
    else if (strcasecmp(name, ":authority") == 0 || strcasecmp(name, "host") == 0)
        strncpy(stream->host, value, sizeof(stream->host) - 1);
    return 0;
}

static uint8_t *hpack_encode_int(uint8_t *buf, uint32_t value,
                                 uint8_t prefix_bits, uint8_t prefix)
{
    uint32_t max = (1 << prefix_bits) - 1;

    if (value < max) {
        *buf++ = prefix | value;
    } else {
        *buf++ = prefix | max;
        value -= max;
        while (value >= 128) {
            *buf++ = (value & 0x7F) | 0x80;
            value >>= 7;
        }
        *buf++ = value;
    }
    return buf;
}

static uint8_t *hpack_encode_str(uint8_t *buf, const char *str)
{
    size_t len = strlen(str);
    buf = hpack_encode_int(buf, len, 7, 0x00);
    memcpy(buf, str, len);
    return buf + len;
}

static uint8_t *hpack_encode_header(uint8_t *buf, const char *name,
                                    const char *value)
{
    *buf++ = 0x40;  /* 字面头部，不索引 */
    buf = hpack_encode_str(buf, name);
    buf = hpack_encode_str(buf, value);
    return buf;
}

static const uint8_t *hpack_decode_int(const uint8_t *buf, uint32_t *value,
                                       uint8_t prefix_bits, uint8_t prefix_val)
{
    uint32_t max = (1 << prefix_bits) - 1;
    *value = prefix_val & max;

    if (*value < max) {
        return buf;
    }

    uint32_t shift = 0;
    uint8_t byte;
    do {
        byte = *buf++;
        *value += (uint32_t)(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);

    return buf;
}

static const uint8_t *hpack_decode_str(const uint8_t *buf, char *str, int max_len)
{
    uint32_t len;
    uint8_t huffman = (*buf >> 7) & 0x1;
    uint8_t prefix_val = *buf;
    buf = hpack_decode_int(buf + 1, &len, 7, prefix_val & 0x7F);

    if (huffman) {
        /*
         * Huffman 编码：教学简化，不解码。
         * 直接标记为未知，不阻断后续头部解析。
         */
        snprintf(str, max_len, "[huffman:%u]", len);
        return buf + len;
    }

    if (len >= (uint32_t)max_len) len = max_len - 1;
    memcpy(str, buf, len);
    str[len] = '\0';
    return buf + len;
}

static int parse_headers(const uint8_t *buf, int len, h2_stream_t *stream)
{
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;

    while (p < end) {
        uint8_t first = *p;

        if ((first & 0x80) == 0x80) {
            /* 1xxxxxxx: 完全索引头部 */
            uint32_t idx = first & 0x7F;
            p++;

            if (idx == 0) {
                log_error("HPACK 索引 0 非法");
                return -1;
            }
            if (idx < HPACK_STATIC_TABLE_SIZE) {
                hpack_apply_header(stream,
                    hpack_static_table[idx].name,
                    hpack_static_table[idx].value);
            }

        } else if ((first & 0xC0) == 0x40) {
            /* 01xxxxxx: 字面头部，indexed name */
            uint32_t name_idx = first & 0x3F;
            p++;

            char name[128] = {0};
            char value[256] = {0};

            if (name_idx == 0) {
                /* name 是字面量 */
                p = hpack_decode_str(p, name, sizeof(name));
                if (!p || p >= end) return -1;
            } else if (name_idx < HPACK_STATIC_TABLE_SIZE) {
                strncpy(name, hpack_static_table[name_idx].name, sizeof(name) - 1);
            }

            if (p >= end) return -1;
            p = hpack_decode_str(p, value, sizeof(value));
            if (!p || p > end) return -1;

            hpack_apply_header(stream, name, value);

        } else if ((first & 0xE0) == 0x20) {
            /* 001xxxxx: 动态表大小更新 */
            p++;
        } else if (first == 0x00 || first == 0x10) {
            /* 0000xxxx: 字面头部，不索引 */
            p++;
            char name[128] = {0};
            char value[256] = {0};
            p = hpack_decode_str(p, name, sizeof(name));
            if (!p || p >= end) return -1;
            p = hpack_decode_str(p, value, sizeof(value));
            if (!p || p > end) return -1;
            hpack_apply_header(stream, name, value);
        } else {
            p++;
        }
    }

    stream->headers_done = 1;
    return 0;
}

/* ---------- 流管理 ---------- */

static h2_stream_t *find_or_create_stream(h2_conn_t *conn, uint32_t stream_id)
{
    for (int i = 0; i < conn->stream_count; i++) {
        if (conn->streams[i].id == stream_id)
            return &conn->streams[i];
    }

    if (conn->stream_count >= MAX_STREAMS) {
        log_error("流数量超限");
        return NULL;
    }

    h2_stream_t *s = &conn->streams[conn->stream_count++];
    memset(s, 0, sizeof(h2_stream_t));
    s->id = stream_id;
    s->state = STREAM_OPEN;
    return s;
}

/* ---------- 发送响应 ---------- */

static int send_response_headers(int fd, uint32_t stream_id,
                                 int status, const char *content_type)
{
    uint8_t buf[BUF_SIZE];
    uint8_t *p = buf;

    if (status == 200) {
        *p++ = 0x88;  /* HPACK 索引 8 = :status: 200 */
    } else {
        char status_str[8];
        snprintf(status_str, sizeof(status_str), "%d", status);
        p = hpack_encode_header(p, ":status", status_str);
    }

    p = hpack_encode_header(p, "content-type", content_type);
    p = hpack_encode_header(p, "server", "tiny_http2/0.1");

    uint32_t payload_len = p - buf;
    return h2_send_frame(fd, HTTP2_HEADERS, FLAG_END_HEADERS,
                         stream_id, buf, payload_len);
}

static int send_response_data(int fd, uint32_t stream_id,
                              const char *data, int len)
{
    return h2_send_frame(fd, HTTP2_DATA, FLAG_END_STREAM,
                         stream_id, (const uint8_t *)data, len);
}

static int send_response(int fd, uint32_t stream_id,
                         int status, const char *content_type,
                         const char *body)
{
    send_response_headers(fd, stream_id, status, content_type);
    send_response_data(fd, stream_id, body, strlen(body));
    return 0;
}

/* ---------- 帧处理 ---------- */

static void handle_settings(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                            const uint8_t *payload)
{
    (void)payload;
    if (hdr->flags & FLAG_ACK) {
        log_debug("收到 SETTINGS ACK fd=%d", conn->fd);
        return;
    }
    log_debug("收到 SETTINGS fd=%d length=%d", conn->fd, hdr->length);
    send_settings_ack(conn->fd);
}

static void handle_headers(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                           const uint8_t *payload)
{
    log_debug("收到 HEADERS fd=%d stream=%d", conn->fd, hdr->stream_id);

    h2_stream_t *stream = find_or_create_stream(conn, hdr->stream_id);
    if (!stream) return;

    const uint8_t *p = payload;
    int len = hdr->length;

    if (hdr->flags & FLAG_PADDED) {
        uint8_t pad_len = *p++;
        len -= 1 + pad_len;
    }

    if (hdr->flags & FLAG_PRIORITY) {
        p += 5;
        len -= 5;
    }

    if (parse_headers(p, len, stream) < 0) {
        log_warn("HPACK 部分解码失败，使用已解码头部");
    }

    log_info("请求: %s %s (stream=%d)",
             stream->method, stream->path, hdr->stream_id);

    if (hdr->flags & FLAG_END_STREAM) {
        char body[1024];
        snprintf(body, sizeof(body),
            "<html><body>"
            "<h1>HTTP/2 Server</h1>"
            "<p>Method: %s</p>"
            "<p>Path: %s</p>"
            "<p>Stream ID: %d</p>"
            "<p>Host: %s</p>"
            "</body></html>",
            stream->method, stream->path, hdr->stream_id,
            stream->host[0] ? stream->host : "unknown");

        send_response(conn->fd, hdr->stream_id, 200,
                      "text/html; charset=utf-8", body);
    }
}

static void handle_data(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                        const uint8_t *payload)
{
    (void)payload;
    log_debug("收到 DATA fd=%d stream=%d length=%d",
              conn->fd, hdr->stream_id, hdr->length);
}

static void handle_ping(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                        const uint8_t *payload)
{
    log_debug("收到 PING fd=%d flags=%d", conn->fd, hdr->flags);
    if (!(hdr->flags & FLAG_ACK)) {
        h2_send_frame(conn->fd, HTTP2_PING, FLAG_ACK, 0, payload, hdr->length);
    }
}

static void handle_goaway(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                          const uint8_t *payload)
{
    (void)hdr; (void)payload;
    log_info("收到 GOAWAY fd=%d", conn->fd);
}

static void handle_window_update(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                                 const uint8_t *payload)
{
    (void)payload;
    log_debug("收到 WINDOW_UPDATE fd=%d stream=%d", conn->fd, hdr->stream_id);
}

static void handle_frame(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                         const uint8_t *payload)
{
    switch (hdr->type) {
    case HTTP2_SETTINGS:      handle_settings(conn, hdr, payload); break;
    case HTTP2_HEADERS:       handle_headers(conn, hdr, payload); break;
    case HTTP2_DATA:          handle_data(conn, hdr, payload); break;
    case HTTP2_PING:          handle_ping(conn, hdr, payload); break;
    case HTTP2_GOAWAY:        handle_goaway(conn, hdr, payload); break;
    case HTTP2_WINDOW_UPDATE: handle_window_update(conn, hdr, payload); break;
    case HTTP2_RST_STREAM:    log_debug("RST_STREAM stream=%d", hdr->stream_id); break;
    case HTTP2_PRIORITY:      log_debug("PRIORITY stream=%d", hdr->stream_id); break;
    default:                  log_debug("未知帧类型: %d", hdr->type);
    }
}

static void handle_read(int epfd, h2_conn_t *conn)
{
    uint8_t buf[BUF_SIZE];
    int n = read(conn->fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
            close(conn->fd);
            if (conn->fd >= 0 && conn->fd < 1024)
                g_conns[conn->fd] = NULL;
            free(conn);
        }
        return;
    }

    int offset = 0;

    if (!conn->preface_done) {
        if (n < H2_PREFACE_LEN) {
            log_error("连接前言不完整");
            return;
        }
        if (memcmp(buf, H2_PREFACE, H2_PREFACE_LEN) != 0) {
            log_error("连接前言错误");
            return;
        }
        conn->preface_done = 1;
        offset = H2_PREFACE_LEN;
        log_debug("连接前言验证通过 fd=%d", conn->fd);
        send_settings(conn->fd);
    }

    while (offset + 9 <= n) {
        h2_frame_hdr_t hdr;
        parse_frame_hdr(buf + offset, &hdr);

        if (offset + 9 + (int)hdr.length > n) break;

        const uint8_t *payload = buf + offset + 9;
        handle_frame(conn, &hdr, payload);
        offset += 9 + hdr.length;
    }
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 14：HTTP/2 服务器 ===");
    log_info("端口: %d", port);

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

    int epfd = epoll_create(1);
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    log_info("HTTP/2 server 监听 0.0.0.0:%d", port);
    printf("\n>>> curl --http2-prior-knowledge http://localhost:%d/ <<<\n\n", port);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            err_sys("epoll_wait error");
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                for (;;) {
                    int conn_fd = accept(listen_fd, NULL, NULL);
                    if (conn_fd < 0) break;
                    set_nonblocking(conn_fd);

                    h2_conn_t *conn = calloc(1, sizeof(h2_conn_t));
                    conn->fd = conn_fd;
                    g_conns[conn_fd] = conn;

                    struct epoll_event cev;
                    cev.events  = EPOLLIN | EPOLLET;
                    cev.data.fd = conn_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &cev);

                    log_debug("新连接 fd=%d", conn_fd);
                }
            } else {
                int fd = events[i].data.fd;
                if (fd >= 0 && fd < 1024 && g_conns[fd]) {
                    handle_read(epfd, g_conns[fd]);
                }
            }
        }
    }

    Close(listen_fd);
    Close(epfd);
    return 0;
}
