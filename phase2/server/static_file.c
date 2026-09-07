/*
 * static_file.c —— 静态文件服务
 *
 * ============================================================
 *  MIME 类型识别 + sendfile 零拷贝传输
 *
 *  sendfile vs read+write：
 *    普通 read+write: 磁盘→内核缓冲→用户空间→内核缓冲→网卡 (4次拷贝)
 *    sendfile:        磁盘→内核缓冲→网卡 (2次拷贝，不经过用户空间)
 * ============================================================
 */

#include "server.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>

#include <sys/stat.h>
#include <sys/sendfile.h>

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
    if (!dot) return "application/octet-stream";
    for (int i = 0; mime_table[i].ext; i++) {
        if (strcasecmp(dot, mime_table[i].ext) == 0) {
            return mime_table[i].mime;
        }
    }
    return "application/octet-stream";
}

/* ---------- 静态文件服务 (明文 socket) ---------- */

int serve_static_file(int fd, const char *uri, const char *www_root)
{
    char file_path[1024];
    if (strcmp(uri, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "%s/index.html", www_root);
    } else {
        snprintf(file_path, sizeof(file_path), "%s%s", www_root, uri);
    }

    /* 路径安全检查 */
    if (strstr(file_path, "..") != NULL) {
        const char *err = "HTTP/1.1 403 Forbidden\r\n"
                          "Content-Length: 0\r\n\r\n";
        write(fd, err, strlen(err));
        return -1;
    }

    struct stat st;
    if (stat(file_path, &st) < 0 || !S_ISREG(st.st_mode)) {
        const char *err = "HTTP/1.1 404 Not Found\r\n"
                          "Content-Length: 0\r\n\r\n";
        write(fd, err, strlen(err));
        return -1;
    }

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        const char *err = "HTTP/1.1 404 Not Found\r\n"
                          "Content-Length: 0\r\n\r\n";
        write(fd, err, strlen(err));
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
    write(fd, header, hlen);

    /* sendfile 零拷贝 */
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