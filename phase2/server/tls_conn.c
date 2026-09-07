/*
 * tls_conn.c —— TLS/HTTPS 连接处理
 *
 * ============================================================
 *  整合 stage8_tls 的核心逻辑：
 *    1. tls_ctx_create: 创建 SSL_CTX，加载证书/私钥
 *    2. tls_do_handshake: 非阻塞 TLS 握手
 *    3. SSL_read/SSL_write: 加密读写
 *    4. serve_static_file_ssl: TLS 下的静态文件（不能用 sendfile）
 *
 *  TLS 握手在非阻塞 socket 上的处理：
 *    SSL_accept 可能返回 SSL_ERROR_WANT_READ/WANT_WRITE
 *    需要等 epoll 通知后再重试
 * ============================================================
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/stat.h>

/* ---------- SSL_CTX 创建/释放 ---------- */

SSL_CTX *tls_ctx_create(const char *cert_path, const char *key_path)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "SSL_CTX_new 失败\n");
        return NULL;
    }

    /* 要求 TLS 1.2+ */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    /* 加载证书和私钥 */
    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "加载证书失败: %s\n", cert_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "加载私钥失败: %s\n", key_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* 验证私钥与证书匹配 */
    if (SSL_CTX_check_private_key(ctx) <= 0) {
        fprintf(stderr, "私钥与证书不匹配\n");
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* ALPN 协商 HTTP/2 */
    static const unsigned char alpn_protos[] = {
        2, 'h', '2',           /* "h2" (HTTP/2) */
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'  /* "http/1.1" */
    };
    SSL_CTX_set_alpn_select_cb(ctx, NULL, NULL);  /* 简化：不实际做 ALPN */

    return ctx;
}

void tls_ctx_free(SSL_CTX *ctx)
{
    if (ctx) {
        SSL_CTX_free(ctx);
    }
}

/* ---------- TLS 握手 ---------- */

int tls_do_handshake(conn_t *conn, SSL_CTX *ctx)
{
    if (!conn->ssl) {
        conn->ssl = SSL_new(ctx);
        if (!conn->ssl) return -1;
        SSL_set_fd(conn->ssl, conn->fd);
        SSL_set_accept_state(conn->ssl);
    }

    int ret = SSL_accept(conn->ssl);
    if (ret == 1) {
        /* 握手成功 */
        return 1;
    }

    int err = SSL_get_error(conn->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        /* 需要更多数据，等下次 epoll 通知 */
        return 0;
    }

    /* 握手失败 */
    return -1;
}

/* ---------- TLS 读写 ---------- */

int tls_read(conn_t *conn, char *buf, int size)
{
    return SSL_read(conn->ssl, buf, size);
}

int tls_write(conn_t *conn, const char *buf, int size)
{
    int written = 0;
    while (written < size) {
        int n = SSL_write(conn->ssl, buf + written, size - written);
        if (n > 0) {
            written += n;
        } else {
            int err = SSL_get_error(conn->ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            return -1;
        }
    }
    return written;
}

void tls_conn_free(conn_t *conn)
{
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
}

/* ---------- TLS 静态文件服务 ---------- */

int serve_static_file_ssl(SSL *ssl, const char *uri, const char *www_root)
{
    char file_path[1024];
    if (strcmp(uri, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "%s/index.html", www_root);
    } else {
        snprintf(file_path, sizeof(file_path), "%s%s", www_root, uri);
    }

    if (strstr(file_path, "..") != NULL) {
        const char *err = "HTTP/1.1 403 Forbidden\r\n\r\n";
        SSL_write(ssl, err, strlen(err));
        return -1;
    }

    struct stat st;
    if (stat(file_path, &st) < 0 || !S_ISREG(st.st_mode)) {
        const char *err = "HTTP/1.1 404 Not Found\r\n\r\n";
        SSL_write(ssl, err, strlen(err));
        return -1;
    }

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        const char *err = "HTTP/1.1 404 Not Found\r\n\r\n";
        SSL_write(ssl, err, strlen(err));
        return -1;
    }

    const char *mime = get_mime_type(file_path);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "Server: advanced_server\r\n"
        "\r\n", mime, (long)st.st_size);
    SSL_write(ssl, header, hlen);

    /*
     * TLS 不能用 sendfile（数据需要加密）
     * 用 read + SSL_write 替代
     */
    char buf[16384];
    ssize_t remaining = st.st_size;
    while (remaining > 0) {
        ssize_t n = read(file_fd, buf, sizeof(buf) < remaining ? sizeof(buf) : remaining);
        if (n <= 0) break;
        SSL_write(ssl, buf, n);
        remaining -= n;
    }

    close(file_fd);
    return 0;
}