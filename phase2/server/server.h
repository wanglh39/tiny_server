#ifndef ADV_SERVER_H
#define ADV_SERVER_H

/*
 * server.h —— 聚合版高级服务器公共头文件
 *
 * ============================================================
 *  整合 phase2 全部 7 个进阶特性：
 *    1. TLS/HTTPS (OpenSSL)
 *    2. io_uring 异步文件 IO
 *    3. 内存池 (Nginx 风格)
 *    4. 异步日志 (muduo 风格双缓冲)
 *    5. SO_REUSEPORT 多线程
 *    6. WebSocket 协议
 *    7. HTTP/2 协议 (HPACK + 多路复用)
 *
 *  架构：
 *    SO_REUSEPORT 多线程，每线程独立 listen_fd + epoll
 *    → accept 后读前几字节检测协议
 *    → 分发到 TLS / WebSocket / HTTP/2 / HTTP/1.1 handler
 *    → 内存池管理连接，异步日志记录
 * ============================================================
 */

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>

/* OpenSSL */
#include <openssl/ssl.h>
#include <openssl/err.h>

/* phase1 HTTP 解析器 */
#include "../../phase1/stage5_http_parser/http_parser.h"

/* phase2 内存池 + 异步日志 */
#include "../stage10_mempool_log/mempool.h"
#include "../stage10_mempool_log/async_log.h"

/* ---------- 配置 ---------- */

typedef struct {
    int  port;            /* 监听端口 */
    int  num_workers;     /* 工作线程数 */
    char www_root[512];   /* 静态文件根目录 */
    int  backlog;         /* listen backlog */

    /* TLS 配置 */
    int  tls_enabled;         /* 是否启用 TLS */
    char cert_path[512];     /* 证书路径 */
    char key_path[512];      /* 私钥路径 */

    /* io_uring 配置 */
    int  uring_enabled;       /* 是否用 io_uring 做异步文件读 */

    /* 日志配置 */
    char log_file[512];      /* 日志文件路径 */
} server_config_t;

void config_init(server_config_t *cfg);
int  config_parse_args(server_config_t *cfg, int argc, char *argv[]);
void config_print(const server_config_t *cfg);

/* ---------- 协议类型 ---------- */

typedef enum {
    PROTO_DETECTING = 0,   /* 尚未检测 */
    PROTO_HTTP1,           /* HTTP/1.1 明文 */
    PROTO_TLS,             /* TLS/HTTPS */
    PROTO_WEBSOCKET,       /* WebSocket（握手完成后） */
    PROTO_HTTP2,           /* HTTP/2 明文 (h2c) */
} proto_type_t;

/* ---------- WebSocket 状态 ---------- */

typedef enum {
    WS_STATE_HANDSHAKE = 0,   /* HTTP 握手阶段 */
    WS_STATE_CONNECTED,       /* WebSocket 帧通信阶段 */
} ws_state_t;

/* ---------- HTTP/2 流状态 ---------- */

typedef enum {
    H2_STREAM_IDLE = 0,
    H2_STREAM_OPEN,
    H2_STREAM_HALF_CLOSED,
    H2_STREAM_CLOSED,
} h2_stream_state_t;

typedef struct {
    uint32_t          id;
    h2_stream_state_t state;
    char              method[16];
    char              path[256];
    char              host[128];
    int               headers_done;
} h2_stream_t;

#define H2_MAX_STREAMS 128
#define H2_PREFACE_LEN 24

/* ---------- 连接 ---------- */

typedef struct conn conn_t;

struct conn {
    int            fd;
    proto_type_t   proto;          /* 协议类型 */

    /* HTTP/1.1 解析器（也用于 WebSocket 握手） */
    http_parser_t  parser;

    /* TLS */
    SSL           *ssl;
    int            tls_handshake_done;

    /* WebSocket */
    ws_state_t     ws_state;

    /* HTTP/2 */
    int            h2_preface_read;
    int            h2_preface_pos;
    h2_stream_t    h2_streams[H2_MAX_STREAMS];
    int            h2_stream_count;
    int            h2_settings_done;

    /* 内存池（每连接一个池，管理临时分配） */
    pool_t        *pool;

    /* 所属 worker */
    int            worker_id;
};

/* ---------- 工作线程 ---------- */

#define MAX_EVENTS 256

typedef struct {
    int       listen_fd;    /* 自己的监听 socket (SO_REUSEPORT) */
    int       epoll_fd;     /* 自己的 epoll */
    int       thread_id;    /* 线程编号 */
    int       num_threads;  /* 总线程数 */
    pthread_t thread;       /* 线程句柄 */

    /* 共享配置指针 */
    const server_config_t *cfg;

    /* TLS 上下文（所有线程共享） */
    SSL_CTX  *ssl_ctx;

    /* 统计 */
    long      accept_count;
    long      request_count;
} worker_t;

/* ---------- 全局状态 ---------- */

typedef struct {
    server_config_t  cfg;
    SSL_CTX         *ssl_ctx;       /* TLS 工厂，NULL 表示未启用 */
    worker_t        *workers;       /* 工作线程数组 */
    int              num_workers;
    volatile int     running;       /* 运行标志 */
} server_t;

extern server_t g_server;          /* 全局服务器实例 */

/* ---------- 工具函数 ---------- */

static inline void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline void set_reuseaddr(int fd)
{
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

static inline void set_reuseport(int fd)
{
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
}

/* ---------- MIME 类型 ---------- */

const char *get_mime_type(const char *path);

/* ---------- 静态文件 ---------- */

int serve_static_file(int fd, const char *uri, const char *www_root);
int serve_static_file_ssl(SSL *ssl, const char *uri, const char *www_root);

/* ---------- TLS 模块 ---------- */

SSL_CTX *tls_ctx_create(const char *cert_path, const char *key_path);
void     tls_ctx_free(SSL_CTX *ctx);
int      tls_do_handshake(conn_t *conn, SSL_CTX *ctx);
int      tls_read(conn_t *conn, char *buf, int size);
int      tls_write(conn_t *conn, const char *buf, int size);
void     tls_conn_free(conn_t *conn);

/* ---------- WebSocket 模块 ---------- */

int ws_do_handshake(conn_t *conn, const char *buf, int len);
int ws_parse_frame(const char *buf, int buf_len,
                   int *opcode, char **payload, int *payload_len);
int ws_send_frame(int fd, int opcode, const char *data, int len);
void ws_handle_message(conn_t *conn, int opcode, char *payload, int payload_len);

/* ---------- HTTP/2 模块 ---------- */

int h2_check_preface(const char *buf, int len);
int h2_send_settings(int fd);
int h2_send_settings_ack(int fd);
int h2_handle_data(conn_t *conn, const char *buf, int len);
int h2_send_response(int fd, uint32_t stream_id, int status,
                     const char *content_type, const char *body, int body_len);

/* ---------- 连接管理 ---------- */

conn_t *conn_create(int fd, int worker_id);
void    conn_free(conn_t *conn);
int     conn_handle_read(conn_t *conn, worker_t *w);
int     conn_handle_write(conn_t *conn, worker_t *w);

/* ---------- 协议检测 ---------- */

proto_type_t detect_protocol(const char *buf, int len);

#endif /* ADV_SERVER_H */