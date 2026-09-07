/*
 * webserver.c —— 阶段 7：完整 Web 服务器
 *
 * ============================================================
 * 在 stage6 Reactor 基础上增加：
 *   1. 路由表（精确匹配 + 静态文件）
 *   2. sendfile 零拷贝传输文件
 *   3. MIME 类型识别
 *   4. 404 / 405 错误处理
 * ============================================================
 *
 * 运行：
 *   build/bin/webserver 8080 4 ./www
 *   浏览器访问 http://localhost:8080/
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
#include <sys/sendfile.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"
#include "http_parser.h"

#define MAX_EVENTS 256
#define BACKLOG    512
#define READ_BUF   4096
#define MAX_WORKERS 32
#define MAX_PATH   1024

/* ---------- 全局配置 ---------- */
static char www_root[MAX_PATH] = "./www";  /* 静态文件根目录 */

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
    {NULL,    "application/octet-stream"},  /* 默认 */
};

static const char *get_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    for (int i = 0; mime_table[i].ext; i++) {
        if (strcasecmp(dot, mime_table[i].ext) == 0) {
            return mime_table[i].mime;
        }
    }
    return "application/octet-stream";
}

/* ---------- 连接结构体 ---------- */
typedef struct {
    int            fd;
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

/* ---------- 发送 HTTP 响应头 ---------- */
static void send_response_header(int fd, int status, const char *status_str,
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
    write(fd, header, len);
}

/* ---------- 发送错误响应 ---------- */
static void send_error(int fd, int status, const char *status_str,
                        const char *message)
{
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<html><body><h1>%d %s</h1><p>%s</p></body></html>",
        status, status_str, message);

    send_response_header(fd, status, status_str, "text/html", body_len);
    write(fd, body, body_len);
}

/* ---------- 处理静态文件请求 ---------- */
static void serve_static_file(int fd, const char *uri)
{
    /*
     * 把 URI 映射到文件系统路径
     *   URI: /index.html → www_root/index.html
     *   URI: /           → www_root/index.html（默认首页）
     */
    char file_path[MAX_PATH];
    if (strcmp(uri, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "%s/index.html", www_root);
    } else {
        snprintf(file_path, sizeof(file_path), "%s%s", www_root, uri);
    }

    /* 安全检查：防止路径穿越攻击（../../../etc/passwd） */
    if (strstr(file_path, "..") != NULL) {
        send_error(fd, 403, "Forbidden", "Path traversal detected");
        return;
    }

    /* stat 获取文件信息 */
    struct stat st;
    if (stat(file_path, &st) < 0) {
        send_error(fd, 404, "Not Found", "File not found");
        return;
    }

    if (!S_ISREG(st.st_mode)) {
        send_error(fd, 403, "Forbidden", "Not a regular file");
        return;
    }

    /* 打开文件 */
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 404, "Not Found", "Cannot open file");
        return;
    }

    /* 发送响应头 */
    const char *mime = get_mime_type(file_path);
    send_response_header(fd, 200, "OK", mime, st.st_size);

    /*
     * sendfile：零拷贝发送文件
     *
     * 普通 read + write：
     *   磁盘 → 内核缓冲 → 用户缓冲(read) → 内核缓冲(write) → 网卡
     *   4 次拷贝，2 次系统调用
     *
     * sendfile：
     *   磁盘 → 内核缓冲 → 网卡
     *   2 次拷贝，1 次系统调用
     *   数据不经过用户空间！
     */
    off_t offset = 0;
    sendfile(fd, file_fd, &offset, st.st_size);

    close(file_fd);
    log_debug("serve %s (%ld bytes, %s)", file_path, st.st_size, mime);
}

/* ---------- 处理动态请求 ---------- */
static void serve_dynamic(int fd, const http_request_t *req, int worker_id)
{
    char body[2048];
    int body_len = snprintf(body, sizeof(body),
        "<html><body>"
        "<h1>Hello from tiny_server!</h1>"
        "<p>Handled by worker thread #%d</p>"
        "<h2>Request Info</h2>"
        "<table>"
        "<tr><td>Method</td><td>%s</td></tr>"
        "<tr><td>URI</td><td>%s</td></tr>"
        "<tr><td>Host</td><td>%s</td></tr>"
        "</table>"
        "<p><a href=\"/\">Home</a></p>"
        "</body></html>",
        worker_id, http_method_str(req->method), req->uri, req->host);

    send_response_header(fd, 200, "OK", "text/html", body_len);
    write(fd, body, body_len);
}

/* ---------- 路由 + 请求处理 ---------- */
static void handle_request(int fd, const http_request_t *req, int worker_id)
{
    /*
     * 路由规则：
     *   /api/*  → 动态请求
     *   其他    → 静态文件
     */
    if (strncmp(req->uri, "/api/", 5) == 0) {
        serve_dynamic(fd, req, worker_id);
    } else {
        serve_static_file(fd, req->uri);
    }
}

/* ---------- 工作线程 ---------- */
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
                int conn_fd;
                while (read(w->notify_fd, &conn_fd, sizeof(int)) > 0) {
                    set_nonblocking(conn_fd);
                    conn_t *conn = calloc(1, sizeof(conn_t));
                    conn->fd = conn_fd;
                    http_parser_init(&conn->parser);

                    struct epoll_event ev;
                    ev.events   = EPOLLIN | EPOLLET;
                    ev.data.ptr = conn;
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);
                }
            } else {
                conn_t *conn = events[i].data.ptr;
                int fd = conn->fd;

                for (;;) {
                    ssize_t nread = read(fd, buf, sizeof(buf));
                    if (nread > 0) {
                        http_parse_result_t result;
                        result = http_parser_feed(&conn->parser, buf, nread);

                        if (result == HTTP_PARSE_DONE) {
                            handle_request(fd, &conn->parser.request,
                                          w->thread_id);
                            http_parser_reset(&conn->parser);
                        } else if (result == HTTP_PARSE_ERROR) {
                            send_error(fd, 400, "Bad Request",
                                      "Malformed HTTP request");
                            Close(fd);
                            epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                            free(conn);
                            break;
                        }

                    } else if (nread == 0) {
                        Close(fd);
                        epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        free(conn);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        Close(fd);
                        epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        free(conn);
                        break;
                    }
                }
            }
        }
    }
    return NULL;
}

/* ---------- main ---------- */
int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) port = atoi(argv[1]);
    if (argc >= 3) num_workers = atoi(argv[2]);
    if (argc >= 4) strncpy(www_root, argv[3], sizeof(www_root) - 1);
    if (num_workers > MAX_WORKERS) num_workers = MAX_WORKERS;

    signal(SIGPIPE, SIG_IGN);

    log_info("=== 阶段 7：完整 Web 服务器 ===");
    log_info("端口: %d, 线程: %d, 根目录: %s", port, num_workers, www_root);

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

    log_info("Web server 监听 0.0.0.0:%d", port);
    printf("\n>>> curl http://localhost:%d/ <<<\n\n", port);

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

                    /* round-robin 分发 */
                    int target = next_worker;
                    next_worker = (next_worker + 1) % num_workers;
                    write(worker_write_fds[target], &conn_fd, sizeof(int));
                }
            }
        }
    }

    Close(listen_fd);
    Close(epfd);
    return 0;
}