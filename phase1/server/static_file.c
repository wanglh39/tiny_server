/*
 * static_file.c —— 静态文件服务实现
 *
 * 核心知识点：sendfile 零拷贝
 *
 * 普通 read + write（4 次拷贝）：
 *   磁盘 → 内核缓冲 → 用户空间 → 内核缓冲 → 网卡
 *
 * sendfile（2 次拷贝）：
 *   磁盘 → 内核缓冲 → 网卡
 *   数据不经过用户空间
 */

#include "static_file.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <strings.h>

/* MIME 类型表 */
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
    {".xml",  "application/xml"},
    {".zip",  "application/zip"},
    {".mp4",  "video/mp4"},
    {".woff", "font/woff"},
    {".woff2","font/woff2"},
    {NULL,    "application/octet-stream"},
};

const char *get_mime_type(const char *path)
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

/* 发送 HTTP 响应头 */
static void send_header(int fd, int status, const char *status_str,
                         const char *content_type, long content_length)
{
    char header[512];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "Server: tiny_server\r\n"
        "\r\n",
        status, status_str, content_type, content_length);
    write(fd, header, len);
}

/* 发送错误响应 */
static void send_error(int fd, int status, const char *status_str,
                        const char *message)
{
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><title>%d %s</title></head>"
        "<body><h1>%d %s</h1><p>%s</p>"
        "<p><a href=\"/\">返回首页</a></p>"
        "</body></html>",
        status, status_str, status, status_str, message);

    send_header(fd, status, status_str, "text/html", body_len);
    write(fd, body, body_len);
}

int serve_static_file(int fd, const char *uri, const char *www_root)
{
    /* URI → 文件路径 */
    char file_path[1024];
    if (strcmp(uri, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "%s/index.html", www_root);
    } else {
        snprintf(file_path, sizeof(file_path), "%s%s", www_root, uri);
    }

    /* 路径安全检查：防止 ../../../etc/passwd */
    if (strstr(file_path, "..") != NULL) {
        send_error(fd, 403, "Forbidden", "路径穿越被拦截");
        return -1;
    }

    /* stat 获取文件信息 */
    struct stat st;
    if (stat(file_path, &st) < 0) {
        send_error(fd, 404, "Not Found", "文件不存在");
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        send_error(fd, 403, "Forbidden", "不是普通文件");
        return -1;
    }

    /* 打开文件 */
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 404, "Not Found", "无法打开文件");
        return -1;
    }

    /* 发送响应头 */
    const char *mime = get_mime_type(file_path);
    send_header(fd, 200, "OK", mime, st.st_size);

    /*
     * sendfile：零拷贝
     *   sendfile(目标fd, 源fd, 偏移量指针, 传输字节数)
     *   数据直接在内核空间从 file_fd 拷贝到 socket fd
     *   不经过用户空间，减少 2 次拷贝
     */
    off_t offset = 0;
    ssize_t sent = 0;
    while (sent < st.st_size) {
        ssize_t n = sendfile(fd, file_fd, &offset, st.st_size - sent);
        if (n <= 0) break;
        sent += n;
    }

    close(file_fd);
    return 0;
}