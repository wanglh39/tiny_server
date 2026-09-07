/*
 * ws_server.c —— 阶段 12：WebSocket 服务器
 *
 * ============================================================
 * WebSocket 协议流程：
 *
 * 1. HTTP Upgrade 握手
 *    客户端发 HTTP 请求:
 *      GET /chat HTTP/1.1
 *      Upgrade: websocket
 *      Connection: Upgrade
 *      Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
 *      Sec-WebSocket-Version: 13
 *
 *    服务器返回:
 *      HTTP/1.1 101 Switching Protocols
 *      Upgrade: websocket
 *      Connection: Upgrade
 *      Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
 *
 * 2. 握手后，TCP 连接升级为 WebSocket，双方用帧通信
 *
 * WS 帧格式:
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-------+-+-------------+-------------------------------+
 *  |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 *  |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 *  |N|V|V|V|       |S|             |   (if payload len==126/127)   |
 *  | |1|2|3|       |K|             |                               |
 *  +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 *  |     Extended payload length continued, if payload len == 127  |
 *  + - - - - - - - - - - - - - - - +-------------------------------+
 *  |                               |Masking-key, if MASK set to 1  |
 *  +-------------------------------+-------------------------------+
 *  | Masking-key (continued)       |          Payload Data         |
 *  +-------------------------------- - - - - - - - - - - - - - - - +
 *  :                     Payload Data continued ...                :
 *  + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
 *  |                     Payload Data continued ...                |
 *  +---------------------------------------------------------------+
 *
 * 运行：
 *   build/bin/ws_server 8080
 *   浏览器打开 test.html 或用 wscat 测试
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

/* OpenSSL 的 SHA1 */
#include <openssl/sha.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define MAX_EVENTS 256
#define BACKLOG    512
#define BUF_SIZE   65536
#define MAX_CLIENTS 1024

/* ---------- WebSocket 帧操作码 ---------- */
enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT         = 0x1,
    WS_OPCODE_BINARY       = 0x2,
    WS_OPCODE_CLOSE        = 0x8,
    WS_OPCODE_PING         = 0x9,
    WS_OPCODE_PONG         = 0xA,
};

/* ---------- 连接状态 ---------- */
typedef enum {
    STATE_HTTP_HANDSHAKE,   /* 还在 HTTP 握手阶段 */
    STATE_WS_CONNECTED,     /* 已升级为 WebSocket */
} conn_state_t;

typedef struct {
    int          fd;
    conn_state_t state;
} conn_t;

static conn_t *clients[MAX_CLIENTS];
static volatile sig_atomic_t running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* 关闭连接：从 epoll 移除 → 关 fd → 释放 conn */
static void close_conn(int epfd, conn_t *conn)
{
    if (!conn) return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    if (conn->fd >= 0 && conn->fd < MAX_CLIENTS && clients[conn->fd]) {
        clients[conn->fd] = NULL;
    }
    free(conn);
}

/* ============================================================
 * Base64 编码（手动实现，避免 OpenSSL BIO 兼容问题）
 * ============================================================ */
static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t len)
{
    size_t out_len = 4 * ((len + 2) / 3);
    char *result = malloc(out_len + 1);
    if (!result) return NULL;

    int j = 0;
    for (size_t i = 0; i < len;) {
        unsigned int octet_a = i < len ? data[i++] : 0;
        unsigned int octet_b = i < len ? data[i++] : 0;
        unsigned int octet_c = i < len ? data[i++] : 0;
        unsigned int triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        result[j++] = base64_table[(triple >> 18) & 0x3F];
        result[j++] = base64_table[(triple >> 12) & 0x3F];
        result[j++] = base64_table[(triple >> 6) & 0x3F];
        result[j++] = base64_table[triple & 0x3F];
    }

    /* 填充 */
    for (size_t i = 0; i < (3 - len % 3) % 3; i++) {
        result[out_len - 1 - i] = '=';
    }
    result[out_len] = '\0';

    return result;
}

/* ============================================================
 * WebSocket 握手：计算 Sec-WebSocket-Accept
 * ============================================================
 *
 * 规则（RFC 6455）：
 *   1. 取客户端的 Sec-WebSocket-Key
 *   2. 拼接固定 GUID: "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
 *   3. 计算 SHA1
 *   4. Base64 编码
 */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static char *compute_accept_key(const char *client_key)
{
    /* 1. 拼接 Key + GUID */
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", client_key, WS_GUID);

    /* 2. SHA1 */
    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)combined, strlen(combined), sha1_hash);

    /* 3. Base64 */
    return base64_encode(sha1_hash, SHA_DIGEST_LENGTH);
}

/* ============================================================
 * HTTP Upgrade 握手
 * ============================================================ */
static int do_handshake(int fd, char *request, int req_len)
{
    /* 提取 Sec-WebSocket-Key */
    char *key_start = strstr(request, "Sec-WebSocket-Key: ");
    if (!key_start) {
        log_error("缺少 Sec-WebSocket-Key");
        return -1;
    }
    key_start += strlen("Sec-WebSocket-Key: ");
    char *key_end = strstr(key_start, "\r\n");
    if (!key_end) return -1;

    char client_key[128];
    int key_len = key_end - key_start;
    if (key_len >= (int)sizeof(client_key)) return -1;
    memcpy(client_key, key_start, key_len);
    client_key[key_len] = '\0';

    /* 计算 Accept Key */
    char *accept_key = compute_accept_key(client_key);

    /* 构造 101 响应 */
    char response[512];
    int resp_len = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept_key);

    free(accept_key);

    /* 发送握手响应 */
    write(fd, response, resp_len);
    log_info("WebSocket 握手完成 fd=%d", fd);
    return 0;
}

/* ============================================================
 * WebSocket 帧解析
 * ============================================================ */

/*
 * ws_parse_frame —— 解析一个 WebSocket 帧
 *
 * 返回值：
 *   > 0: 帧的完整长度（已解析成功）
 *   0:  数据不够，需要更多数据
 *   -1: 协议错误
 *
 * 输出参数：
 *   opcode:  帧类型
 *   payload: 指向 payload 数据的指针（仍在 buf 内，不拷贝）
 *   payload_len: payload 长度
 */
static int ws_parse_frame(const char *buf, int buf_len,
                          int *opcode, char **payload, int *payload_len)
{
    if (buf_len < 2) return 0;  /* 至少需要 2 字节头部 */

    /* 第 1 字节 */
    int fin    = (buf[0] >> 7) & 0x1;
    int op     = buf[0] & 0x0F;

    /* 第 2 字节 */
    int masked = (buf[1] >> 7) & 0x1;
    int len    = buf[1] & 0x7F;

    /* 计算 payload 长度和头部长度 */
    int header_len = 2;
    int payload_length;

    if (len < 126) {
        payload_length = len;
    } else if (len == 126) {
        /* 扩展长度：2 字节 */
        if (buf_len < 4) return 0;
        payload_length = ((unsigned char)buf[2] << 8) |
                         (unsigned char)buf[3];
        header_len = 4;
    } else {
        /* len == 127：8 字节扩展长度 */
        if (buf_len < 10) return 0;
        payload_length = 0;
        for (int i = 0; i < 8; i++) {
            payload_length = (payload_length << 8) |
                             (unsigned char)buf[2 + i];
        }
        header_len = 10;
    }

    /* 掩码键（4 字节） */
    if (masked) {
        header_len += 4;
    }

    /* 检查数据是否完整 */
    if (buf_len < header_len + payload_length) return 0;

    /* 提取 payload */
    char *p = (char *)buf + header_len - (masked ? 4 : 0);

    /* 解掩码 */
    if (masked) {
        unsigned char mask[4];
        memcpy(mask, buf + header_len - 4, 4);
        p = (char *)buf + header_len;
        for (int i = 0; i < payload_length; i++) {
            p[i] ^= mask[i % 4];
        }
    }

    *opcode      = op;
    *payload     = p;
    *payload_len = payload_length;

    (void)fin;  /* 教学简化：不处理分片 */
    return header_len + payload_length;
}

/* ============================================================
 * WebSocket 帧生成
 * ============================================================
 *
 * 服务器→客户端的帧不掩码（RFC 6455 规定）
 */
static int ws_send_frame(int fd, int opcode, const char *data, int len)
{
    /*
     * 把头部和 payload 拼到一个缓冲区，一次 write 发出。
     * 如果分两次 write，TCP 可能分成两个段发送，
     * 客户端第一次 recv 只拿到头部，解析出 length=N
     * 但 payload 为空，导致后续帧全部错位。
     */
    char frame[BUF_SIZE];
    int header_len = 0;

    /* 第 1 字节：FIN=1, opcode */
    frame[0] = 0x80 | (opcode & 0x0F);
    header_len = 1;

    /* 第 2 字节：MASK=0, payload length */
    if (len < 126) {
        frame[1] = len;
        header_len = 2;
    } else if (len < 65536) {
        frame[1] = 126;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        header_len = 4;
    } else {
        frame[1] = 127;
        for (int i = 0; i < 8; i++) {
            frame[2 + i] = (len >> (56 - 8 * i)) & 0xFF;
        }
        header_len = 10;
    }

    /* 拷贝 payload 到帧缓冲区 */
    if (len > 0 && data) {
        memcpy(frame + header_len, data, len);
    }

    /* 一次性发送整个帧 */
    int total = header_len + len;
    int written = 0;
    while (written < total) {
        int n = write(fd, frame + written, total - written);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        written += n;
    }

    return written;
}

/* ============================================================
 * 处理 WebSocket 消息
 * ============================================================ */
static void handle_ws_message(int fd, int opcode, char *payload, int payload_len)
{
    switch (opcode) {

    case WS_OPCODE_TEXT:
    case WS_OPCODE_BINARY:
        /* Echo：原样发回 */
        log_debug("收到消息 fd=%d: %.*s", fd, payload_len, payload);
        ws_send_frame(fd, opcode, payload, payload_len);
        break;

    case WS_OPCODE_PING:
        /* Ping → 回 Pong */
        log_debug("收到 Ping fd=%d", fd);
        ws_send_frame(fd, WS_OPCODE_PONG, payload, payload_len);
        break;

    case WS_OPCODE_PONG:
        /* Pong：心跳响应，忽略 */
        log_debug("收到 Pong fd=%d", fd);
        break;

    case WS_OPCODE_CLOSE:
        /* 客户端关闭：回 Close 帧后由 epoll 正常清理 */
        log_debug("收到 Close fd=%d", fd);
        ws_send_frame(fd, WS_OPCODE_CLOSE, NULL, 0);
        shutdown(fd, SHUT_WR);
        break;

    default:
        log_error("未知 opcode: %d", opcode);
    }
}

/* ============================================================
 * 连接处理
 * ============================================================ */
static void handle_read(int epfd, conn_t *conn)
{
    char buf[BUF_SIZE];
    int n = read(conn->fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close_conn(epfd, conn);
        }
        return;
    }
    buf[n] = '\0';

    if (conn->state == STATE_HTTP_HANDSHAKE) {
        /* HTTP Upgrade 握手 */
        if (do_handshake(conn->fd, buf, n) == 0) {
            conn->state = STATE_WS_CONNECTED;
        } else {
            close_conn(epfd, conn);
        }
    } else {
        /* WebSocket 帧解析 */
        int opcode, payload_len;
        char *payload;
        int frame_len = ws_parse_frame(buf, n, &opcode, &payload, &payload_len);

        if (frame_len > 0) {
            handle_ws_message(conn->fd, opcode, payload, payload_len);
        } else if (frame_len < 0) {
            log_error("帧解析错误 fd=%d", conn->fd);
            close_conn(epfd, conn);
        }
        /* frame_len == 0: 数据不够，等下次 */
    }
}

/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    log_info("=== 阶段 12：WebSocket 服务器 ===");
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

    int epfd = epoll_create(1);
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    log_info("WebSocket server 监听 0.0.0.0:%d", port);
    printf("\n>>> 用浏览器打开 ws://localhost:%d/ 测试 <<<\n", port);
    printf(">>> 或用 wscat: wscat -c ws://localhost:%d/ <<<\n\n", port);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                if (!running) break;
                continue;
            }
            err_sys("epoll_wait error");
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                /* 新连接 */
                for (;;) {
                    int conn_fd = accept(listen_fd, NULL, NULL);
                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }
                    set_nonblocking(conn_fd);

                    conn_t *conn = calloc(1, sizeof(conn_t));
                    conn->fd    = conn_fd;
                    conn->state = STATE_HTTP_HANDSHAKE;
                    clients[conn_fd] = conn;

                    struct epoll_event cev;
                    cev.events   = EPOLLIN | EPOLLET;
                    cev.data.fd  = conn_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &cev);

                    log_debug("新连接 fd=%d", conn_fd);
                }
            } else {
                /* 数据到达 */
                int fd = events[i].data.fd;
                if (clients[fd]) {
                    handle_read(epfd, clients[fd]);
                }
            }
        }
    }

    Close(listen_fd);
    Close(epfd);
    return 0;
}