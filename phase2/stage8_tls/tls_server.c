/*
 * tls_server.c —— 阶段 8：TLS/HTTPS 服务器
 *
 * ============================================================
 * 在 phase1/stage7_webserver 基础上增加 TLS 加密：
 *   1. OpenSSL 初始化（SSL_CTX + 加载证书/私钥）
 *   2. accept 后做 TLS 握手（SSL_accept）
 *   3. SSL_read / SSL_write 替代 read / write
 *   4. sendfile 无法用于 SSL → 改为 read 文件 + SSL_write
 *
 * TLS 握手流程（TLS 1.3 简化版）：
 *   ClientHello → ServerHello + 证书 + Finished → Finished
 *   1-RTT 完成握手（TLS 1.2 需要 2-RTT）
 *
 * 运行：
 *   build/bin/tls_server 8443 4 ./phase2/www cert.pem key.pem
 *   浏览器访问 https://localhost:8443/
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

/* OpenSSL 头文件 */
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"
#include "http_parser.h"

#define MAX_EVENTS 256
#define BACKLOG    512
#define READ_BUF   4096
#define FILE_BUF   65536   /* 发送文件时的缓冲区（SSL 不支持 sendfile） */
#define MAX_WORKERS 32
#define MAX_PATH   1024

/* ---------- 全局配置 ---------- */
static char www_root[MAX_PATH] = "./phase2/www";
static SSL_CTX *g_ssl_ctx = NULL;   /* 全局 SSL 上下文，所有连接共享 */

/* ---------- MIME 类型表 ---------- */
typedef struct {
    const char *ext;
    const char *mime;
} mime_entry_t;

static const mime_entry_t mime_table[] = {
    {".html", "text/html"},
    {".htm",  "text/html"},
    {".css",  "text/css"},
    {".js",   "application/javascript"},
    {".json", "application/json"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".txt",  "text/plain"},
    {".pdf",  "application/pdf"},
    {NULL,    "application/octet-stream"},
};

static const char *get_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; mime_table[i].ext; i++) {
        if (strcasecmp(dot, mime_table[i].ext) == 0)
            return mime_table[i].mime;
    }
    return "application/octet-stream";
}

/* ---------- 连接结构体 ---------- */
/*
 * 和 phase1 的区别：多了 SSL *ssl 字段
 *   ssl != NULL → TLS 连接，用 SSL_read/SSL_write
 *   ssl == NULL → 普通 HTTP（本阶段总是 TLS，留 NULL 检测用）
 */
typedef struct {
    int            fd;
    SSL           *ssl;       /* OpenSSL 连接对象 */
    http_parser_t  parser;
} conn_t;

typedef struct {
    int       epoll_fd;
    int       notify_fd;
    int       thread_id;
    pthread_t thread;
} worker_t;

static worker_t workers[MAX_WORKERS];
static int      worker_write_fds[MAX_WORKERS];
static int      num_workers = 4;
static int      next_worker = 0;

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ============================================================
 * OpenSSL 初始化
 * ============================================================
 */

/*
 * tls_init —— 初始化 OpenSSL 库 + 创建 SSL_CTX
 *
 * SSL_CTX 是所有 TLS 连接的"工厂"：
 *   - 存放证书和私钥（所有连接共享同一份证书）
 *   - 存放 TLS 版本、密码套件等配置
 *   - 每个新连接从 CTX 创建一个 SSL 对象
 */
static SSL_CTX *tls_init(const char *cert_path, const char *key_path)
{
    /* OpenSSL 1.1.0+ 自动初始化，不需要 SSL_library_init() 等 */

    /* 创建 SSL_CTX：
     *   TLS_server_method() 表示用服务端模式，自动协商最高版本
     */
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        log_error("SSL_CTX_new 失败");
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    /* 要求 TLS 1.2 以上（TLS 1.3 优先） */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    /* 加载证书文件（公钥 + 证书链） */
    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        log_error("加载证书失败: %s", cert_path);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* 加载私钥文件 */
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        log_error("加载私钥失败: %s", key_path);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* 验证私钥和证书匹配 */
    if (SSL_CTX_check_private_key(ctx) <= 0) {
        log_error("私钥与证书不匹配");
        SSL_CTX_free(ctx);
        return NULL;
    }

    log_info("OpenSSL 初始化完成（证书: %s）", cert_path);
    return ctx;
}

/*
 * tls_accept —— 在 TCP accept 之后做 TLS 握手
 *
 * 流程：
 *   1. 从 SSL_CTX 创建一个 SSL 对象
 *   2. 把 SSL 对象绑定到 fd
 *   3. 调用 SSL_accept() 执行握手
 *
 * 握手期间会读写 fd，所以 fd 必须是阻塞模式（或用 BIO 管理非阻塞）
 * 教学简化：握手时临时设为阻塞，握手完成后设回非阻塞
 */
static SSL *tls_accept(SSL_CTX *ctx, int fd)
{
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        log_error("SSL_new 失败");
        return NULL;
    }

    /* 把 SSL 对象绑定到 socket fd */
    SSL_set_fd(ssl, fd);

    /* 临时设为阻塞，简化握手逻辑 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    /* TLS 握手 */
    int ret = SSL_accept(ssl);
    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);
        log_error("TLS 握手失败 (err=%d)", err);
        SSL_free(ssl);
        /* 恢复非阻塞 */
        fcntl(fd, F_SETFL, flags);
        return NULL;
    }

    /* 恢复非阻塞 */
    fcntl(fd, F_SETFL, flags);

    log_debug("TLS 握手成功 (协议=%s, 密码套件=%s)",
              SSL_get_version(ssl),
              SSL_get_cipher_name(ssl));
    return ssl;
}

/*
 * tls_close —— 关闭 TLS 连接
 *
 * 先 SSL_shutdown 发送 close_notify 警告（优雅关闭）
 * 再 SSL_free 释放 SSL 对象
 * 最后 close(fd) 关闭 TCP 连接
 */
static void tls_close(conn_t *conn)
{
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    close(conn->fd);
}

/* ============================================================
 * HTTP 响应发送（SSL 版）
 * ============================================================
 */

/* SSL_write 封装：处理 EAGAIN/部分写 */
static int ssl_write_all(SSL *ssl, const void *buf, size_t len)
{
    const char *p = buf;
    size_t written = 0;
    while (written < len) {
        int n = SSL_write(ssl, p + written, len - written);
        if (n > 0) {
            written += n;
        } else {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                /* 非阻塞，需要重试 */
                continue;
            }
            /* 真正的错误 */
            return -1;
        }
    }
    return written;
}

static void send_response_header(SSL *ssl, int status, const char *status_str,
                                  const char *content_type, long content_length)
{
    char header[512];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        status, status_str, content_type, content_length);
    ssl_write_all(ssl, header, len);
}

static void send_error(SSL *ssl, int status, const char *status_str,
                        const char *message)
{
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<html><body><h1>%d %s</h1><p>%s</p></body></html>",
        status, status_str, message);
    send_response_header(ssl, status, status_str, "text/html", body_len);
    ssl_write_all(ssl, body, body_len);
}

/* ---------- 处理静态文件请求 ---------- */
/*
 * SSL 不支持 sendfile 零拷贝！
 *
 * 原因：sendfile 直接在内核里把文件数据发到 socket
 *       但 TLS 加密必须在用户空间完成（OpenSSL 库）
 *       内核看不到加密后的数据
 *
 * 所以 SSL 发送文件只能：read 文件 → SSL_write 加密发送
 * 这也是 HTTPS 比 HTTP 慢的原因之一
 */
static void serve_static_file(SSL *ssl, const char *uri)
{
    char file_path[MAX_PATH];
    if (strcmp(uri, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "%s/index.html", www_root);
    } else {
        snprintf(file_path, sizeof(file_path), "%s%s", www_root, uri);
    }

    if (strstr(file_path, "..") != NULL) {
        send_error(ssl, 403, "Forbidden", "Path traversal detected");
        return;
    }

    struct stat st;
    if (stat(file_path, &st) < 0) {
        send_error(ssl, 404, "Not Found", "File not found");
        return;
    }

    if (!S_ISREG(st.st_mode)) {
        send_error(ssl, 403, "Forbidden", "Not a regular file");
        return;
    }

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        send_error(ssl, 404, "Not Found", "Cannot open file");
        return;
    }

    const char *mime = get_mime_type(file_path);
    send_response_header(ssl, 200, "OK", mime, st.st_size);

    /* read + SSL_write 发送文件内容 */
    char buf[FILE_BUF];
    ssize_t n;
    while ((n = read(file_fd, buf, sizeof(buf))) > 0) {
        if (ssl_write_all(ssl, buf, n) < 0) break;
    }

    close(file_fd);
    log_debug("serve %s (%ld bytes, %s)", file_path, st.st_size, mime);
}

/* ---------- 处理动态请求 ---------- */
static void serve_dynamic(SSL *ssl, const http_request_t *req, int worker_id)
{
    char body[2048];
    int body_len = snprintf(body, sizeof(body),
        "<html><body>"
        "<h1>Hello from tiny_server (HTTPS)!</h1>"
        "<p>Handled by worker thread #%d</p>"
        "<h2>Request Info</h2>"
        "<table>"
        "<tr><td>Method</td><td>%s</td></tr>"
        "<tr><td>URI</td><td>%s</td></tr>"
        "<tr><td>Host</td><td>%s</td></tr>"
        "</table>"
        "<p>🔒 This connection is encrypted with %s</p>"
        "<p><a href=\"/\">Home</a></p>"
        "</body></html>",
        worker_id, http_method_str(req->method), req->uri, req->host,
        SSL_get_cipher_name(ssl));

    send_response_header(ssl, 200, "OK", "text/html", body_len);
    ssl_write_all(ssl, body, body_len);
}

/* ---------- 路由 + 请求处理 ---------- */
static void handle_request(SSL *ssl, const http_request_t *req, int worker_id)
{
    if (strncmp(req->uri, "/api/", 5) == 0) {
        serve_dynamic(ssl, req, worker_id);
    } else {
        serve_static_file(ssl, req->uri);
    }
}

/* ============================================================
 * 工作线程
 * ============================================================
 */
static void *worker_loop(void *arg)
{
    worker_t *w = (worker_t *)arg;
    log_info("工作线程 #%d 启动", w->thread_id);

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
                /* 新连接通知 */
                int conn_fd;
                while (read(w->notify_fd, &conn_fd, sizeof(int)) > 0) {
                    /*
                     * TLS 握手：在 accept 之后、加入 epoll 之前
                     * 握手是阻塞的（教学简化）
                     */
                    SSL *ssl = tls_accept(g_ssl_ctx, conn_fd);
                    if (!ssl) {
                        close(conn_fd);
                        continue;
                    }

                    set_nonblocking(conn_fd);
                    conn_t *conn = calloc(1, sizeof(conn_t));
                    conn->fd  = conn_fd;
                    conn->ssl = ssl;
                    http_parser_init(&conn->parser);

                    struct epoll_event ev;
                    ev.events   = EPOLLIN | EPOLLET;
                    ev.data.ptr = conn;
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);
                }
            } else {
                /* 连接有数据 */
                conn_t *conn = events[i].data.ptr;

                for (;;) {
                    /*
                     * 关键区别：SSL_read 替代 read
                     * SSL 内部会解密，返回明文数据
                     */
                    int nread = SSL_read(conn->ssl, buf, sizeof(buf));
                    if (nread > 0) {
                        http_parse_result_t result;
                        result = http_parser_feed(&conn->parser, buf, nread);

                        if (result == HTTP_PARSE_DONE) {
                            handle_request(conn->ssl,
                                          &conn->parser.request,
                                          w->thread_id);
                            http_parser_reset(&conn->parser);
                        } else if (result == HTTP_PARSE_ERROR) {
                            send_error(conn->ssl, 400, "Bad Request",
                                      "Malformed HTTP request");
                            tls_close(conn);
                            epoll_ctl(w->epoll_fd,
                                     EPOLL_CTL_DEL, conn->fd, NULL);
                            free(conn);
                            break;
                        }

                    } else if (nread == 0) {
                        /* SSL_read 返回 0 = 对端关闭 */
                        tls_close(conn);
                        epoll_ctl(w->epoll_fd,
                                 EPOLL_CTL_DEL, conn->fd, NULL);
                        free(conn);
                        break;
                    } else {
                        /* SSL_read 返回 < 0：检查 SSL 错误 */
                        int err = SSL_get_error(conn->ssl, nread);
                        if (err == SSL_ERROR_WANT_READ ||
                            err == SSL_ERROR_WANT_WRITE) {
                            /* 非阻塞，数据还没到 */
                            break;
                        }
                        /* 真正的错误或连接断开 */
                        tls_close(conn);
                        epoll_ctl(w->epoll_fd,
                                 EPOLL_CTL_DEL, conn->fd, NULL);
                        free(conn);
                        break;
                    }
                }
            }
        }
    }
    return NULL;
}

/* ============================================================
 * main
 * ============================================================
 */
int main(int argc, char *argv[])
{
    char cert_path[MAX_PATH] = "";
    char key_path[MAX_PATH]  = "";
    int  port = 8443;

    if (argc >= 2) port = atoi(argv[1]);
    if (argc >= 3) num_workers = atoi(argv[2]);
    if (argc >= 4) strncpy(www_root, argv[3], sizeof(www_root) - 1);
    if (argc >= 5) strncpy(cert_path, argv[4], sizeof(cert_path) - 1);
    if (argc >= 6) strncpy(key_path, argv[5], sizeof(key_path) - 1);
    if (num_workers > MAX_WORKERS) num_workers = MAX_WORKERS;

    if (cert_path[0] == '\0' || key_path[0] == '\0') {
        fprintf(stderr, "用法: %s <port> <workers> <www_root> <cert.pem> <key.pem>\n", argv[0]);
        fprintf(stderr, "示例: %s 8443 4 ./phase2/www cert.pem key.pem\n", argv[0]);
        fprintf(stderr, "生成证书: bash phase2/stage8_tls/gen_cert.sh\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 8：TLS/HTTPS 服务器 ===");
    log_info("端口: %d, 线程: %d, 根目录: %s", port, num_workers, www_root);

    /* 初始化 OpenSSL */
    g_ssl_ctx = tls_init(cert_path, key_path);
    if (!g_ssl_ctx) {
        fprintf(stderr, "OpenSSL 初始化失败\n");
        return 1;
    }

    /* 初始化工作线程 */
    for (int i = 0; i < num_workers; i++) {
        workers[i].thread_id = i;
        workers[i].epoll_fd  = epoll_create(1);

        int pipe_fd[2];
        if (pipe(pipe_fd) < 0) err_sys("pipe error");
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

    int epfd = epoll_create(1);
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    log_info("HTTPS server 监听 0.0.0.0:%d", port);
    printf("\n>>> curl -k https://localhost:%d/ <<<\n\n", port);

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
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(listen_fd,
                                         (SA *)&client_addr, &client_len);
                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }

                    /*
                     * TCP accept 完成 → 分发给工作线程
                     * TLS 握手在工作线程里做（tls_accept）
                     * 这样主线程不会因握手阻塞
                     */
                    int target = next_worker;
                    next_worker = (next_worker + 1) % num_workers;
                    write(worker_write_fds[target], &conn_fd, sizeof(int));
                }
            }
        }
    }

    /* 清理 */
    SSL_CTX_free(g_ssl_ctx);
    Close(listen_fd);
    Close(epfd);
    return 0;
}