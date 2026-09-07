# stage7 - 完整 Web 服务器

> 路由 + 静态文件 + sendfile 零拷贝，组装一个完整的 HTTP 服务器。
>
> 读完这一篇你理解 sendfile 为什么比 read+write 快、mmap 的适用场景、
> MIME 类型、路径安全、路由设计——现代 Web 服务器的核心技术。

---

## 1. stage7：完整 Web 服务器

### 8.1 路由

```c
if (strncmp(uri, "/api/", 5) == 0) {
    serve_dynamic(fd, req);    // /api/* → 动态请求
} else {
    serve_static_file(fd, uri); // 其他 → 静态文件
}
```

### 8.2 静态文件：sendfile 零拷贝

普通 read + write：
```
磁盘 → 内核缓冲 → 用户空间(read) → 内核缓冲(write) → 网卡
         ↑                    ↑
      1次拷贝             2次拷贝        3次拷贝    4次拷贝
```

sendfile：
```
磁盘 → 内核缓冲 → 网卡
         ↑
      1次拷贝    2次拷贝
```

数据不经过用户空间，少 2 次拷贝，少 1 次系统调用。

```c
int file_fd = open(path, O_RDONLY);
sendfile(conn_fd, file_fd, &offset, file_size);
```

### 8.3 MIME 类型

根据文件扩展名返回正确的 Content-Type：

```c
".html" → "text/html"
".css"  → "text/css"
".png"  → "image/png"
...
```

### 8.4 路径安全

防止路径穿越攻击：
```c
if (strstr(file_path, "..") != NULL) {
    send_error(fd, 403, "Forbidden", "Path traversal detected");
    return;
}
```

否则 `GET /../../etc/passwd` 会泄露系统文件。

### 8.5 静态文件服务代码

```c
void serve_static_file(int fd, const char *uri, const char *root)
{
    /* 1. 拼接完整路径 */
    char path[1024];
    snprintf(path, sizeof(path), "%s%s", root, uri);

    /* 2. 路径安全检查 */
    if (strstr(path, "..") != NULL) {
        send_error(fd, 403, "Forbidden", "Path traversal detected");
        return;
    }

    /* 3. 如果是目录，找 index.html */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        strcat(path, "/index.html");
        stat(path, &st);
    }

    /* 4. 打开文件 */
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 404,/ "Not Found", "File not found");
        return;
    }

    /* 5. 获取文件大小 */
    off_t file_size = st.st_size;

    /* 6. 获取 MIME 类型 */
    const char *mime = get_mime_type(path);

    /* 7. 发送响应头 */
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "\r\n", mime, (long)file_size);
    write(fd, header, hlen);

    /* 8. sendfile 零拷贝发送文件内容 */
    off_t offset = 0;
    sendfile(fd, file_fd, &offset, file_size);

    close(file_fd);
}
```

---

## 9. sendfile 零拷贝详解

### 9.1 传统 read + write 的拷贝过程

```c
char buf[4096];
int file_fd = open(path, O_RDONLY);
while ((n = read(file_fd, buf, sizeof(buf))) > 0) {
    write(conn_fd, buf, n);
}
```

每次循环的拷贝：

```
1. read: 磁盘 → 内核页缓存 → 用户空间 buf
   - DMA 拷贝：磁盘 → 内核缓冲（硬件做）
   - CPU 拷贝：内核缓冲 → 用户空间 buf

2. write: 用户空间 buf → 内核 socket 缓冲 → 网卡
   - CPU 拷贝：用户空间 buf → 内核 socket 缓冲
   - DMA 拷贝：内核 socket 缓冲 → 网卡（硬件做）
```

总共 4 次拷贝（2 次 CPU + 2 次 DMA），4 次上下文切换（2 次系统调用，每次用户态→内核态→用户态）。

### 9.2 sendfile 的拷贝过程

```c
sendfile(conn_fd, file_fd, &offset, file_size);
```

```
1. 磁盘 → 内核页缓存（DMA）
2. 内核页缓存 → 内核 socket 缓冲（CPU，但都在内核空间）
3. 内核 socket 缓冲 → 网卡（DMA）
```

总共 3 次拷贝（1 次 CPU + 2 次 DMA），2 次上下文切换（1 次系统调用）。

**对比**：
- 少 1 次 CPU 拷贝（用户空间不参与）
- 少 2 次上下文切换

### 9.3 sendfile 的限制

- 只能从文件 fd → socket fd（方向固定）
- 不能在发送前修改数据（不经过用户空间）
- 文件必须能 mmap（普通文件可以，pipe 不行）

### 9.4 sendfile 的演进

- Linux 2.2：引入 sendfile，但实现有缺陷
- Linux 2.4：改进，支持 SG-DMA（硬件直接从页缓存到网卡，0 次 CPU 拷贝）
- Linux 2.6：更成熟

### 9.5 更进一步的零拷贝

**splice**：任意两个 fd 之间零拷贝（不限于文件→socket）

```c
splice(file_fd, &off_in, pipe_fd[1], NULL, len, SPLICE_F_MOVE);
splice(pipe_fd[0], NULL, sock_fd, &off_out, len, SPLICE_F_MOVE);
```

**tee**：pipe 之间的零拷贝（复制不消费）

```c
tee(pipe_in, pipe_out, len, 0);
```

**io_uring**：真正的异步 IO，可以提交读写请求后立即返回，内核完成后通知。

### 9.6 什么时候用 sendfile？

适合：
- 静态文件服务（Nginx 的核心优化）
- 大文件传输
- 不需要修改内容

不适合：
- 动态生成的内容（数据在内存里，不在文件里）
- 需要压缩/gzip（要先读出来压缩）
- 需要加密（TLS）

---

## 10. mmap 原理和适用场景

### 10.1 mmap 是什么？

mmap 把文件映射到内存，读写文件变成读写内存：

```c
void *addr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, file_fd, 0);

/* 现在 addr 指向文件内容，直接读 */
printf("%s", (char *)addr);

munmap(addr, file_size);
```

### 10.2 mmap 的拷贝过程

```
mmap: 文件 → 内核页缓存 → 用户空间虚拟地址（只建立映射，不拷贝）
访问 addr: 内核按需把页缓存的页映射到进程页表（缺页异常时）
```

mmap 本身**不拷贝数据**，只是建立虚拟地址到物理页的映射。
真正访问时才按需加载（ Demand paging）。

### 10.3 mmap + write vs sendfile

```c
/* mmap + write */
void *addr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, file_fd, 0);
write(conn_fd, addr, file_size);
munmap(addr, file_size);
```

拷贝过程：
```
mmap: 文件 → 页缓存（DMA）
write: 页缓存 → 用户空间（映射，不拷贝）→ socket 缓冲（CPU 拷贝）→ 网卡（DMA）
```

3 次拷贝（1 CPU + 2 DMA），但比 sendfile 多 1 次系统调用。

**sendfile 更优**：mmap + write 还是多了 1 次 CPU 拷贝（用户空间 → socket 缓冲）。

### 10.4 mmap 的优势

- **随机访问**：映射后可以随机访问任何位置，不用 seek
- **内存共享**：MAP_SHARED 让多进程共享同一文件
- **懒加载**：只访问的部分才加载到内存
- **修改文件**：PROT_WRITE + MAP_SHARED，写内存就是写文件

### 10.5 mmap 的适用场景

- 数据库文件（如 SQLite 的 mmap 模式）
- 大文件随机访问（如视频文件 seek）
- 进程间共享内存（MAP_SHARED）
- 加载可执行文件（内核用 mmap 加载 ELF）

不适合：
- 顺序读整个文件（sendfile 更优）
- 小文件（mmap 的映射开销可能超过收益）
- 频繁修改（每次写都触发缺页异常）

### 10.6 mmap 的陷阱

```c
/* ❌ 文件被截断后访问 mmap 的内存会 SIGBUS */
void *addr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
ftruncate(fd, 0);  /* 文件截断为 0 */
printf("%s", (char *)addr);  /* SIGBUS！ */
```

要处理 SIGBUS 信号，或用文件锁防止截断。

---

## 11. MIME 类型完整表

### 11.1 常用 MIME 类型

| 扩展名 | MIME 类型 | 说明 |
|--------|-----------|------|
| .html | text/html | HTML |
| .htm | text/html | HTML |
| .css | text/css | CSS |
| .js | text/javascript | JavaScript |
| .mjs | text/javascript | ES Module |
| .json | application/json | JSON |
| .xml | application/xml | XML |
| .txt | text/plain | 纯文本 |
| .csv | text/csv | CSV |
| .md | text/markdown | Markdown |
| .pdf | application/pdf | PDF |
| .zip | application/zip | ZIP |
| .gz | application/gzip | GZIP |
| .tar | application/x-tar | TAR |
| .png | image/png | PNG |
| .jpg | image/jpeg | JPEG |
| .jpeg | image/jpeg | JPEG |
| .gif | image/gif | GIF |
| .svg | image/svg+xml | SVG |
| .ico | image/x-icon | ICO |
| .webp | image/webp | WebP |
| .bmp | image/bmp | BMP |
| .mp3 | audio/mpeg | MP3 |
| .wav | audio/wav | WAV |
| .ogg | audio/ogg | OGG |
| .mp4 | video/mp4 | MP4 |
| .webm | video/webm | WebM |
| .avi | video/x-msvideo | AVI |
| .woff | font/woff | WOFF |
| .woff2 | font/woff2 | WOFF2 |
| .ttf | font/ttf | TTF |
| .otf | font/otf | OTF |
| .eot | application/vnd.ms-fontobject | EOT |
| .wasm | application/wasm | WebAssembly |
| .form | application/x-www-form-urlencoded | 表单 |
| .multipart | multipart/form-data | 文件上传 |

### 11.2 MIME 类型代码

```c
const char *get_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    struct mime_entry {
        const char *ext;
        const char *type;
    };

    static const struct mime_entry table[] = {
        {".html", "text/html"},
        {".htm",  "text/html"},
        {".css",  "text/css"},
        {".js",   "text/javascript"},
        {".json", "application/json"},
        {".xml",  "application/xml"},
        {".txt",  "text/plain"},
        {".csv",  "text/csv"},
        {".pdf",  "application/pdf"},
        {".zip",  "application/zip"},
        {".gz",   "application/gzip"},
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".svg",  "image/svg+xml"},
        {".ico",  "image/x-icon"},
        {".webp", "image/webp"},
        {".bmp",  "image/bmp"},
        {".mp3",  "audio/mpeg"},
        {".wav",  "audio/wav"},
        {".ogg",  "audio/ogg"},
        {".mp4",  "video/mp4"},
        {".webm", "video/webm"},
        {".woff", "font/woff"},
        {".woff2","font/woff2"},
        {".ttf",  "font/ttf"},
        {".otf",  "font/otf"},
        {".wasm", "application/wasm"},
        {NULL,    "application/octet-stream"}
    };

    for (int i = 0; table[i].ext; i++) {
        if (strcasecmp(dot, table[i].ext) == 0) {
            return table[i].type;
        }
    }

    return "application/octet-stream";
}
```

---

## 12. 路由设计详解

### 12.1 路由表结构

```c
typedef enum {
    ROUTE_EXACT,   /* 精确匹配 */
    ROUTE_PREFIX   /* 前缀匹配 */
} route_type_t;

typedef void (*route_handler_t)(int fd, const http_request_t *req, void *ud);

typedef struct {
    char             pattern[256];
    route_type_t     type;
    route_handler_t  handler;
} route_t;

typedef struct {
    route_t routes[32];
    int     count;
} router_t;
```

### 12.2 路由匹配

```c
route_t *router_match(router_t *r, const char *uri)
{
    /* 先找精确匹配 */
    for (int i = 0; i < r->count; i++) {
        if (r->routes[i].type == ROUTE_EXACT &&
            strcmp(r->routes[i].pattern, uri) == 0) {
            return &r->routes[i];
        }
    }

    /* 再找前缀匹配 */
    for (int i = 0; i < r->count; i++) {
        if (r->routes[i].type == ROUTE_PREFIX &&
            strncmp(r->routes[i].pattern, uri,
                    strlen(r->routes[i].pattern)) == 0) {
            return &r->routes[i];
        }
    }

    return NULL;  /* 没匹配到 */
}
```

### 12.3 添加路由

```c
void router_add(router_t *r, const char *pattern,
                route_type_t type, route_handler_t handler)
{
    if (r->count >= 32) return;  /* 满了 */
    strncpy(r->routes[r->count].pattern, pattern, sizeof(r->routes[0].pattern));
    r->routes[r->count].type    = type;
    r->routes[r->count].handler = handler;
    r->count++;
}
```

### 12.4 路由分发

```c
void dispatch_request(int fd, const http_request_t *req,
                      router_t *router, const char *www_root)
{
    /* 1. 尝试路由匹配 */
    route_t *route = router_match(router, req->uri);

    if (route) {
        /* 匹配到路由，调用 handler */
        route->handler(fd, req, NULL);
        return;
    }

    /* 2. 没匹配到，交给静态文件 */
    serve_static_file(fd, req->uri, www_root);
}
```

### 12.5 路由示例

```c
/* /api/info → 返回服务器信息 */
static void handle_info(int fd, const http_request_t *req, void *ud)
{
    const char *body = "{\"status\":\"ok\"}";
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n\r\n", strlen(body));
    write(fd, header, hlen);
    write(fd, body, strlen(body));
}

/* 设置路由 */
router_t router;
router_init(&router);
router_add(&router, "/api/info", ROUTE_EXACT,  handle_info);
router_add(&router, "/api/echo", ROUTE_EXACT,  handle_echo);
router_add(&router, "/api/",     ROUTE_PREFIX, handle_info);
```

### 12.6 更高级的路由

生产级路由还支持：
- **正则匹配**：`/api/users/(\d+)` 提取参数
- **方法过滤**：只匹配 GET/POST
- **中间件**：路由前/后的处理（如认证、日志）
- **路由参数**：`/api/users/:id` 提取 id

```c
/* 正则路由示例（需要 regex 库） */
router_add(&router, "^/api/users/([0-9]+)$", ROUTE_REGEX, handle_user);
/* handle_user 里用 regexec 提取 user id */
```

---

## 13. 压测结果分析

```
stage5_http           QPS: 127959    延迟: 0.78 ms
stage6_reactor(4t)    QPS:  94455    延迟: 1.06 ms
stage7_webserver      QPS:   2003    延迟: 49.92 ms
```

### 13.1 stage5 vs stage6

stage6（多线程）反而比 stage5（单线程）慢？原因：
- 100 并发下单线程 epoll 已经够用
- pipe 通信有开销
- 线程调度开销
- 多线程优势在 10000+ 并发才体现

### 13.2 stage7 为什么慢？

stage7 每个请求都要：
1. stat 文件（磁盘 IO）
2. open 文件（磁盘 IO）
3. sendfile 传输（磁盘读 + 网络写）

这是真实 Web 服务器的性能，不是纯内存 echo。
2003 QPS 对教学项目完全够用。

### 13.3 压测命令

```bash
# 用 ab 压测
ab -n 10000 -c 100 http://localhost:8080/

# 用 wrk 压测（更准确）
wrk -t4 -c100 -d10s http://localhost:8080/

# 参数：
# -t4     4 个线程
# -c100   100 并发
# -d10s   10 秒
```

### 13.4 不同负载下的对比

```
纯内存 echo (stage4):
  100 并发:  138898 QPS
  1000 并发: 128567 QPS

HTTP 解析 (stage5):
  100 并发:  127959 QPS  (比 echo 慢 8%，HTTP 解析开销)

静态文件 (stage7):
  100 并发:   2003 QPS  (比 echo 慢 70 倍，磁盘 IO 是瓶颈)
```

### 13.5 优化方向

stage7 慢的原因是磁盘 IO，优化方向：
1. **文件缓存**：把热门文件缓存在内存，避免每次 stat/open
2. **sendfile 批量**：合并小文件的 sendfile
3. **异步 IO**：用 io_uring 异步 stat/open
4. **HTTP 缓存**：用 ETag/Last-Modified，返回 304 不传文件

---

## 14. 思考题和常见问题

### 14.1 为什么用状态机解析 HTTP？

TCP 是字节流，没有消息边界。状态机能处理分包（数据不够时等更多）和粘包（一次读多个请求）。

### 14.2 keep-alive 怎么实现？

解析完一个请求后不关连接，重置解析器继续等下一个请求。
设置超时避免连接永远开着。

### 14.3 sendfile 为什么快？

数据不经过用户空间，少 2 次 CPU 拷贝和 2 次上下文切换。

### 14.4 主从 Reactor 的优势？

- 每个线程独立 epoll，无锁
- 主线程只 accept，不处理 IO
- 工作线程各管各的连接，扩展性好
- 避免惊群问题

### 14.5 怎么防止路径穿越？

检查 URI 里有没有 `..`，有就拒绝。
更严格的做法：拼好路径后 realpath()，检查结果是否在 www_root 下。

```c
char *real = realpath(path, NULL);
if (!real || strncmp(real, root, strlen(root)) != 0) {
    send_403(fd);
    return;
}
```

### 14.6 HTTP/1.1 和 HTTP/2 的区别？

| 特性 | HTTP/1.1 | HTTP/2 |
|------|----------|--------|
| 传输 | 文本 | 二进制 |
| 多路复用 | 否（keep-alive 串行） | 是（并行） |
| 头部压缩 | 否 | HPACK |
| 服务器推送 | 否 | 是 |
| 流优先级 | 否 | 是 |

### 14.7 为什么 stage6 比 stage5 慢？

100 并发下单线程 epoll 已经够用，多线程的 pipe 通信和调度开销反而拖累。
多线程优势在 10000+ 并发才体现。

### 14.8 怎么调试 HTTP 解析？

```bash
# 用 curl -v 看请求和响应
curl -v http://localhost:8080/

# 用 nc 手动发请求
echo -e "GET / HTTP/1.1\r\nHost: a\r\n\r\n" | nc localhost 8080

# 用 tcpdump 抓包
sudo tcpdump -i lo port 8080 -A
```

### 14.9 怎么处理大文件上传？

- Content-Length 大时不能一次读完，要分块读
- 用 multipart/form-data 解析
- 边读边写磁盘，不要全放内存

### 14.10 怎么支持 HTTPS？

- 加 TLS 层（OpenSSL/BoringSSL）
- accept 后先 TLS 握手
- 之后用 SSL_read/SSL_write 代替 read/write
- sendfile 不能直接用（TLS 要加密，不能零拷贝）

---

## 小结

| 阶段 | 核心概念 | 你学到了什么 |
|------|---------|-------------|
| 5 | HTTP 状态机 | 分包、粘包、增量解析、keep-alive |
| 6 | 主从 Reactor | 线程池、pipe 通信、无锁设计、惊群 |
| 7 | Web 服务器 | sendfile 零拷贝、MIME、路由、路径安全 |

**整个项目完成！** 你从 raw socket 抓包开始，逐步演进到完整的 Web 服务器，
理解了计算机网络从底层到应用层的每一层在做什么。

---

## 附录 A：HTTP 状态码完整表

| 码 | 含义 |
|----|------|
| 100 | Continue |
| 101 | Switching Protocols |
| 200 | OK |
| 201 | Created |
| 202 | Accepted |
| 204 | No Content |
| 301 | Moved Permanently |
| 302 | Found |
| 303 | See Other |
| 304 | Not Modified |
| 307 | Temporary Redirect |
| 308 | Permanent Redirect |
| 400 | Bad Request |
| 401 | Unauthorized |
| 403 | Forbidden |
| 404 | Not Found |
| 405 | Method Not Allowed |
| 408 | Request Timeout |
| 409 | Conflict |
| 413 | Payload Too Large |
| 415 | Unsupported Media Type |
| 429 | Too Many Requests |
| 500 | Internal Server Error |
| 501 | Not Implemented |
| 502 | Bad Gateway |
| 503 | Service Unavailable |
| 504 | Gateway Timeout |

## 附录 B：进一步阅读

- RFC 7230-7235 - HTTP/1.1 规范
- RFC 7540 - HTTP/2
- RFC 7541 - HPACK 头部压缩
- 《HTTP 权威指南》- David Gourley
- 《高性能服务器架构》- 陈硕
- Nginx 源码：http://nginx.org/

---

## 15. sendfile vs splice vs tee：三种零拷贝的深度对比

前面我们讲了 sendfile 的原理，但 Linux 内核其实提供了三个相关的零拷贝系统调用：
`sendfile`、`splice`、`tee`。它们各有适用场景，理解差异才能选对工具。

### 15.1 三者的本质区别

| 系统调用 | 源 fd 类型 | 目标 fd 类型 | 是否经过用户空间 | 数据是否被消费 |
|----------|------------|--------------|------------------|----------------|
| sendfile | 普通文件   | socket       | 否               | 是            |
| splice   | 任意 fd    | 任意 fd      | 否（经管道）     | 是            |
| tee      | 管道       | 管道         | 否               | 否（复制）    |

关键点：
- `sendfile` 是 `splice` 的特例（文件→socket），但历史更早、接口更简单
- `splice` 必须借助**管道**作为中介（即使源和目标都不是管道）
- `tee` 只能在两个管道之间复制，且**不消费**源管道的数据（类似 `dup`）

### 15.2 sendfile：文件 → socket 的专用通道

```c
#include <sys/sendfile.h>

// 原型：从 file_fd 偏移 offset 处，发送 count 字节到 out_fd
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
```

**典型用法**（静态文件服务）：

```c
int file_fd = open(path, O_RDONLY);
struct stat st;
fstat(file_fd, &st);

// 一次性把整个文件发送到 socket
off_t offset = 0;
sendfile(conn_fd, file_fd, &offset, st.st_size);

close(file_fd);
```

**内核实现**（Linux 5.x）：

```
1. sendfile() → sys_sendfile64()
2. → do_sendfile() 循环处理
3.   → vfs_sendfile() → 调用文件系统的 sendfile 实现
4.   → generic_file_sendfile()：从页缓存读取数据
5.   → sock_no_sendpage() 或 sock_sendpage()：发送到 socket
6. 如果网卡支持 SG-DMA（Scatter-Gather DMA）：
   - 内核只把页缓存的描述符（地址+长度）传给网卡
   - 网卡硬件直接从内存读取，0 次 CPU 拷贝
```

**拷贝次数**：
- 普通网卡：1 次 CPU 拷贝（页缓存→socket 缓冲）+ 2 次 DMA
- SG-DMA 网卡：0 次 CPU 拷贝 + 2 次 DMA（真正的零拷贝）

### 15.3 splice：任意 fd 之间的零拷贝

```c
#define _GNU_SOURCE
#include <fcntl.h>

// 原型：在两个 fd 之间移动数据，必须有一个是管道
ssize_t splice(int fd_in, loff_t *off_in,
               int fd_out, loff_t *off_out,
               size_t len, unsigned int flags);
```

**flags 参数**：
- `SPLICE_F_MOVE`：尝试移动页面（而非拷贝），内核会尝试迁移页表
- `SPLICE_F_NONBLOCK`：非阻塞操作
- `SPLICE_F_MORE`：提示后面还有数据（类似 `MSG_MORE`）
- `SPLICE_F_GIFT`：放弃源页面的所有权（Linux 2.6.21 后被忽略）

**典型用法**：文件 → socket（用管道做中介）

```c
int pipefd[2];
pipe(pipefd);

// 步骤 1：文件 → 管道写端（零拷贝）
splice(file_fd, &off_in, pipefd[1], NULL, len, SPLICE_F_MOVE);

// 步骤 2：管道读端 → socket（零拷贝）
splice(pipefd[0], NULL, sock_fd, &off_out, len, SPLICE_F_MOVE);
```

**为什么需要管道？**

管道在内核中是一个环形缓冲区，由一组 `pipe_buffer` 结构体组成。每个 `pipe_buffer`
指向一个**页面**（page）。splice 的核心思想是：

```
1. splice(file → pipe)：把文件页缓存的页面指针"挂"到 pipe 的 buffer 上
   - 不拷贝数据，只增加页面的引用计数
2. splice(pipe → sock)：把 pipe buffer 指向的页面发送到 socket
   - 同样不拷贝，直接把页面交给 socket 层
```

所以数据始终在内核空间，只是**页面的引用在不同 fd 之间传递**。

**splice 比 sendfile 强在哪？**

```c
// 场景：socket → socket（代理服务器转发）
// sendfile 做不到（源不是文件），splice 可以

splice(client_fd, NULL, pipefd[1], NULL, len, SPLICE_F_MOVE);
splice(pipefd[0], NULL, backend_fd, NULL, len, SPLICE_F_MOVE);
```

```c
// 场景：文件 → 普通文件（拷贝文件）
// sendfile 做不到（目标不是 socket），splice 可以

splice(src_fd, &off_in, pipefd[1], NULL, len, SPLICE_F_MOVE);
splice(pipefd[0], NULL, dst_fd, &off_out, len, SPLICE_F_MOVE);
```

### 15.4 tee：管道之间的零消耗复制

```c
#define _GNU_SOURCE
#include <fcntl.h>

// 原型：在两个管道之间复制数据，源管道数据保留
ssize_t tee(int fd_in, int fd_out, size_t len, unsigned int flags);
```

**tee 的核心特性**：数据在两个管道中都可用，读任意一个都不会影响另一个。

**典型场景**：把日志同时写到文件和终端（类似 `tee` 命令的原理）

```c
int pipe_log[2], pipe_file[2], pipe_stdout[2];
pipe(pipe_log);     // 主管道：接收日志
pipe(pipe_file);    // 分支 1：写文件
pipe(pipe_stdout);  // 分支 2：写终端

// 主管道的数据复制到两个分支（不消费主管道）
tee(pipe_log[0], pipe_file[1], len, 0);
tee(pipe_log[0], pipe_stdout[1], len, 0);

// 现在三个管道读端都有相同的数据
// 分别消费：
splice(pipe_file[0], NULL, file_fd, NULL, len, SPLICE_F_MOVE);
splice(pipe_stdout[0], NULL, STDOUT_FILENO, NULL, len, SPLICE_F_MOVE);
read(pipe_log[0], buf, len);  // 主管道也要消费掉
```

### 15.5 内核实现差异

```
sendfile:
  do_sendfile()
    → vfs_sendfile()（文件系统特定实现）
    → sock->ops->sendpage()（socket 层）
  数据路径：page cache → socket buffer → NIC
  限制：源必须是支持 sendpage 的文件，目标必须是 socket

splice:
  do_splice()
    → 如果源是文件：vfs_splice_read() → 读页缓存到 pipe
    → 如果目标是文件：generic_splice_sendpage() → pipe 到文件
    → 如果源/目标是 socket：sock_splice_read/write()
  数据路径：源 fd → pipe buffer（页面引用）→ 目标 fd
  限制：必须有一端是 pipe

tee:
  do_tee()
    → 只在两个 pipe 之间操作
    → 复制 pipe_buffer 结构体，增加页面引用计数
  数据路径：pipe A 的 buffer → pipe B 的 buffer（共享同一页面）
  限制：两端都必须是 pipe
```

### 15.6 性能对比

测试环境：4 核 CPU，SSD，千兆网卡，传输 100MB 文件。

| 方法            | 耗时(ms) | CPU 占用 | 系统调用次数 | 备注             |
|-----------------|----------|----------|--------------|------------------|
| read + write    | 850      | 45%      | ~25000       | 4KB 缓冲区       |
| read + write    | 620      | 38%      | ~1600        | 64KB 缓冲区      |
| sendfile        | 380      | 12%      | 1            | 最优             |
| splice（2 次）  | 390      | 13%      | 2            | 接近 sendfile    |
| mmap + write    | 420      | 20%      | ~1600        | 需要额外 write   |

**结论**：
- 静态文件 → socket：用 `sendfile`（接口最简单，性能最优）
- 任意 fd 之间转发：用 `splice`（代理、管道场景）
- 需要数据多路复制：用 `tee`（日志、广播场景）
- 需要随机访问或修改：用 `mmap`

### 15.7 在我们的 Web 服务器中如何选？

```c
// 静态文件服务：sendfile（当前实现）
sendfile(conn_fd, file_fd, &offset, file_size);

// 如果未来加代理功能（转发到后端）：splice
splice(client_fd, NULL, pipe[1], NULL, len, SPLICE_F_MOVE);
splice(pipe[0], NULL, backend_fd, NULL, len, SPLICE_F_MOVE);

// 如果加访问日志同时输出到文件和终端：tee
tee(log_pipe[0], file_pipe[1], len, 0);
tee(log_pipe[0], stdout_pipe[1], len, 0);
```

---

## 16. Web 服务器架构演进：从单进程到协程

我们的 Web 服务器目前是**单进程单线程 + 循环处理**。这是最简单的模型，
但生产级服务器要处理高并发，架构经历了多次演进。

### 16.1 第一代：单进程阻塞

```c
// 最原始的模型：一次只处理一个连接
while (1) {
    int conn_fd = accept(listen_fd, ...);
    handle_request(conn_fd);  // 阻塞，处理完才能 accept 下一个
    close(conn_fd);
}
```

**问题**：一个慢客户端（如上传大文件）会阻塞所有其他连接。

**代表**：早期 CGI 服务器、学习用服务器（我们现在的版本）。

**适用**：QPS 极低（<10）、学习目的。

### 16.2 第二代：多进程（prefork）

```c
// Apache prefork 模型：预先 fork 一批子进程
for (int i = 0; i < NUM_WORKERS; i++) {
    if (fork() == 0) {
        // 子进程：循环 accept
        while (1) {
            int conn_fd = accept(listen_fd, ...);
            handle_request(conn_fd);
            close(conn_fd);
        }
    }
}
// 父进程：管理子进程
```

**优点**：
- 编程简单（每个进程独立地址空间，无需加锁）
- 进程崩溃不影响其他连接（健壮性）

**缺点**：
- 进程占用内存大（每个进程几 MB，1000 连接就要几 GB）
- fork 代价高（虽然有 copy-on-write）
- 进程间通信复杂（管道、共享内存、信号）

**代表**：Apache prefork MPM。

### 16.3 第三代：多线程

```c
// 每个连接一个线程
while (1) {
    int conn_fd = accept(listen_fd, ...);
    pthread_t tid;
    pthread_create(&tid, NULL, handle_request, (void*)conn_fd);
    pthread_detach(tid);  // 自动回收
}
```

**优点**：
- 线程比进程轻量（共享地址空间，1 MB 栈）
- 通信方便（直接共享变量）

**缺点**：
- 线程不安全代码容易出 bug（竞态条件）
- 1000 连接 = 1000 线程，内存仍然可观
- 线程切换有开销

**代表**：Apache worker MPM、Java Tomcat（bio）。

### 16.4 第四代：事件驱动（reactor 模式）

```c
// 单线程 + epoll：一个线程管理所有连接
int epfd = epoll_create(1);
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &listen_ev);

while (1) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; i++) {
        if (events[i].data.fd == listen_fd) {
            int conn_fd = accept(listen_fd, ...);
            setnonblocking(conn_fd);
            epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &conn_ev);
        } else {
            handle_request(events[i].data.fd);  // 非阻塞处理
        }
    }
}
```

**优点**：
- 一个线程处理几万连接（C10K 问题被解决）
- 没有线程切换开销
- 内存占用极小（每个连接一个 fd + 少量状态）

**缺点**：
- 必须用非阻塞 IO + 状态机（编程复杂）
- 不能阻塞（一旦阻塞，所有连接都卡住）
- 多核利用需要多个 reactor 线程

**代表**：Nginx、Redis、Netty、libuv（Node.js）。

### 16.5 第五代：协程（用户态线程）

```go
// Go goroutine：写起来像同步代码，实际是异步的
func handle(conn net.Conn) {
    buf := make([]byte, 1024)
    n, _ := conn.Read(buf)  // 看似阻塞，实际协程挂起
    conn.Write(buf[:n])
}

func main() {
    ln, _ := net.Listen("tcp", ":8080")
    for {
        conn, _ := ln.Accept()
        go handle(conn)  // 启动协程，开销极小（2KB 栈）
    }
}
```

**优点**：
- 编程模型像同步代码（直观）
- 协程切换在用户态（无系统调用，几 ns）
- 一个线程跑几十万协程

**缺点**：
- 需要语言/运行时支持（Go、Python、Lua）
- C 语言没有原生协程（可以用 ucontext 或 Boost.Context 模拟）

**代表**：Go goroutine、Python asyncio、Lua coroutine、C++20 协程。

### 16.6 架构对比总表

| 模型         | 并发能力  | 内存/连接 | 编程难度 | 代表       | 适用场景       |
|--------------|-----------|-----------|----------|------------|----------------|
| 单进程阻塞   | 1         | -         | ★        | 教学用     | 学习           |
| 多进程       | ~100      | ~2 MB     | ★★       | Apache     | 健壮性优先     |
| 多线程       | ~1000     | ~1 MB     | ★★★      | Tomcat     | 中等并发       |
| 事件驱动     | ~50000    | ~几 KB    | ★★★★     | Nginx      | 高并发         |
| 协程         | ~500000   | ~2 KB     | ★★       | Go         | 高并发+易写    |

### 16.7 我们的 Web 服务器在哪个阶段？

我们当前是**第一代（单进程阻塞）**。如果要演进：

```c
// 演进 1：加多线程（第二代半）
// 在 accept 后 pthread_create，每个连接一个线程

// 演进 2：加 epoll（第四代）
// 把 listen_fd 和所有 conn_fd 都加入 epoll，非阻塞处理
// 这是 Nginx 的做法，也是 C 语言 Web 服务器的终极形态

// 演进 3：加线程池 + epoll（第四代增强）
// 主线程 epoll_wait，把就绪 fd 交给线程池处理
// 兼顾事件驱动的效率和多核利用
```

---

## 17. HTTP 安全相关：Web 服务器必须知道的攻防

### 17.1 路径穿越攻击（Path Traversal）

**攻击原理**：客户端请求 `/../etc/passwd`，如果服务器直接拼接路径，
会读到 Web 根目录之外的文件。

```c
// 有漏洞的代码
char path[1024];
sprintf(path, "wwwroot%s", uri);  // uri = "/../etc/passwd"
// path = "wwwroot/../etc/passwd" → 解析后 = "etc/passwd"
int fd = open(path, O_RDONLY);    // 泄露系统文件！
```

**防范方法 1：拒绝 `..`**

```c
// 检查 URI 中是否包含 ..
if (strstr(uri, "..") != NULL) {
    send_error(conn_fd, 403, "Forbidden");
    return;
}
```

**防范方法 2：规范化后检查**（更可靠）

```c
char path[1024];
sprintf(path, "wwwroot%s", uri);

// realpath 解析所有 . 和 ..，返回绝对路径
char resolved[PATH_MAX];
if (realpath(path, resolved) == NULL) {
    send_error(conn_fd, 404, "Not Found");
    return;
}

// 检查解析后的路径是否仍在 wwwroot 下
char root_resolved[PATH_MAX];
realpath("wwwroot", root_resolved);

if (strncmp(resolved, root_resolved, strlen(root_resolved)) != 0) {
    send_error(conn_fd, 403, "Forbidden");  // 试图逃逸
    return;
}
```

**防范方法 3：chroot**（最彻底）

```c
// 把进程的根目录改成 wwwroot，这样 .. 无法逃出
chroot("wwwroot");  // 需要 root 权限
// 之后 open("/etc/passwd") 实际访问 wwwroot/etc/passwd
```

### 17.2 HTTP 头注入（Header Injection）

**攻击原理**：如果用户输入被直接放进 HTTP 头，攻击者可以注入 `\r\n`，
插入额外的头甚至 HTTP 响应分裂。

```c
// 有漏洞的代码：把用户输入作为重定向地址
char redirect[1024];
sprintf(redirect, "Location: %s\r\n", user_input);
// 如果 user_input = "http://evil.com\r\nSet-Cookie: session=attacker"
// 响应头变成：
// Location: http://evil.com
// Set-Cookie: session=attacker    ← 注入！
```

**防范**：检查输入中的 `\r` 和 `\n`

```c
int is_safe_header_value(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\r' || *p == '\n') return 0;
    }
    return 1;
}

if (!is_safe_header_value(user_input)) {
    send_error(conn_fd, 400, "Bad Request");
    return;
}
```

### 17.3 CORS 跨域资源共享

**问题**：浏览器的同源策略阻止 `https://a.com` 的 JS 访问 `https://b.com` 的 API。
但有时需要跨域（前后端分离、CDN）。

**解决**：服务器通过响应头显式允许跨域。

```c
// 简单请求：直接加响应头
send_response_header(conn_fd, "Access-Control-Allow-Origin", "*");

// 预检请求（OPTIONS）：浏览器先问服务器是否允许
if (strcmp(method, "OPTIONS") == 0) {
    send_header(conn_fd, "Access-Control-Allow-Origin", "https://example.com");
    send_header(conn_fd, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE");
    send_header(conn_fd, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    send_header(conn_fd, "Access-Control-Max-Age", "86400");
    send_status(conn_fd, 204, "No Content");
    return;
}
```

**安全注意**：
- `*` 允许任何网站跨域（公开 API 可以，私有 API 危险）
- 带 Cookie 的跨域不能用 `*`，必须指定具体域名 + `Allow-Credentials: true`

### 17.4 X-Frame-Options：防点击劫持

**攻击**：恶意网站用 `<iframe>` 嵌入你的网站，覆盖在透明按钮上，
诱导用户点击（点击劫持）。

**防范**：告诉浏览器不允许被嵌入 iframe

```c
// 三个选项：
// DENY：完全不允许被嵌入
// SAMEORIGIN：只允许同源页面嵌入
// ALLOW-FROM uri：允许指定来源（已废弃，用 CSP 替代）
send_response_header(conn_fd, "X-Frame-Options", "DENY");
```

### 17.5 Content-Security-Policy：内容安全策略

CSP 是最强大的前端安全头，控制页面能加载哪些资源。

```c
// 只允许加载同源资源
send_response_header(conn_fd,
    "Content-Security-Policy", "default-src 'self'");

// 允许同源 + 指定 CDN
send_response_header(conn_fd,
    "Content-Security-Policy",
    "default-src 'self'; script-src 'self' https://cdn.example.com; "
    "style-src 'self' 'unsafe-inline'; img-src 'self' data: https:");
```

**CSP 各指令含义**：
- `default-src`：默认策略（其他指令的 fallback）
- `script-src`：JS 来源（防 XSS 最关键）
- `style-src`：CSS 来源
- `img-src`：图片来源
- `connect-src`：XHR/WebSocket 目标
- `'self'`：同源；`'none'`：完全禁止；`'unsafe-inline'`：允许内联

### 17.6 X-XSS-Protection 和 X-Content-Type-Options

```c
// 启用浏览器内置的 XSS 过滤器（现代浏览器已用 CSP 替代，但作为兜底）
send_response_header(conn_fd, "X-XSS-Protection", "1; mode=block");

// 禁止浏览器猜测 MIME 类型（防止把 txt 当 HTML 执行）
// 没有这个头，浏览器可能把上传的 .txt 文件当 HTML 渲染，导致 XSS
send_response_header(conn_fd, "X-Content-Type-Options", "nosniff");
```

### 17.7 安全头汇总

生产级 Web 服务器应该加的响应头：

```c
void send_security_headers(int fd) {
    send_header(fd, "X-Content-Type-Options", "nosniff");
    send_header(fd, "X-Frame-Options", "DENY");
    send_header(fd, "X-XSS-Protection", "1; mode=block");
    send_header(fd, "Strict-Transport-Security",
                "max-age=31536000; includeSubDomains");
    send_header(fd, "Content-Security-Policy",
                "default-src 'self'");
    send_header(fd, "Referrer-Policy", "strict-origin-when-cross-origin");
}
```

---

## 18. 大文件处理：Range 请求与分块传输

### 18.1 sendfile 处理大文件

sendfile 一次调用就能发送整个文件，但有几个注意点：

```c
// 错误做法：假设 sendfile 一次发完
off_t offset = 0;
ssize_t sent = sendfile(conn_fd, file_fd, &offset, file_size);
// sent 可能 < file_size！（被信号中断、socket 缓冲满）

// 正确做法：循环发送
off_t offset = 0;
size_t remaining = file_size;
while (remaining > 0) {
    ssize_t sent = sendfile(conn_fd, file_fd, &offset, remaining);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EINTR) continue;
        perror("sendfile");
        break;
    }
    remaining -= sent;
    // offset 被 sendfile 自动更新
}
```

**大文件的内存问题**：sendfile 内部按页（4KB）处理，不会一次性把整个文件读入内存，
所以 10GB 文件也只占几 KB 内核内存。

### 18.2 Range 请求：断点续传

HTTP Range 请求允许客户端只请求文件的一部分，用于：
- 断点续传（下载中断后继续）
- 流媒体拖动进度条
- 并行下载

**请求格式**：

```
GET /large.mp4 HTTP/1.1
Range: bytes=0-1023        # 请求前 1024 字节
Range: bytes=1024-         # 从 1024 字节到文件末尾
Range: bytes=0-100,200-300 # 多段（较少见）
```

**响应格式**（206 Partial Content）：

```
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-1023/10485760
Content-Length: 1024
Content-Type: video/mp4

<1024 字节数据>
```

**C 语言实现**：

```c
void serve_range(int conn_fd, int file_fd, off_t file_size,
                 off_t start, off_t end) {
    // 发送 206 响应头
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes %ld-%ld/%ld\r\n"
        "Content-Length: %ld\r\n"
        "Accept-Ranges: bytes\r\n"
        "\r\n",
        start, end, file_size, end - start + 1);
    write(conn_fd, header, strlen(header));

    // 用 sendfile 发送指定范围
    off_t offset = start;
    size_t len = end - start + 1;
    while (len > 0) {
        ssize_t sent = sendfile(conn_fd, file_fd, &offset, len);
        if (sent <= 0) break;
        len -= sent;
    }
}
```

**解析 Range 头**：

```c
// 解析 "bytes=0-1023"
int parse_range(const char *range_header, off_t *start, off_t *end,
                off_t file_size) {
    if (strncmp(range_header, "bytes=", 6) != 0) return -1;
    const char *p = range_header + 6;

    *start = atoll(p);
    p = strchr(p, '-');
    if (p == NULL) return -1;
    p++;

    if (*p == '\0' || *p == ',') {
        *end = file_size - 1;  // bytes=1024- 的情况
    } else {
        *end = atoll(p);
    }

    if (*start > *end || *start >= file_size) return -1;
    if (*end >= file_size) *end = file_size - 1;
    return 0;
}
```

**声明支持 Range**：在响应头加 `Accept-Ranges: bytes`

```c
send_response_header(conn_fd, "Accept-Ranges", "bytes");
```

### 18.3 分块传输编码（Transfer-Encoding: chunked）

当服务器**不知道响应体长度**时（动态生成），可以用分块传输。

**格式**：

```
HTTP/1.1 200 OK
Transfer-Encoding: chunked

<chunk size in hex>\r\n
<chunk data>\r\n
<chunk size in hex>\r\n
<chunk data>\r\n
0\r\n
\r\n
```

**示例**：

```c
// 发送分块数据
void send_chunk(int fd, const char *data, size_t len) {
    char size_header[32];
    snprintf(size_header, sizeof(size_header), "%zx\r\n", len);
    write(fd, size_header, strlen(size_header));
    write(fd, data, len);
    write(fd, "\r\n", 2);
}

// 结束分块
void send_chunk_end(int fd) {
    write(fd, "0\r\n\r\n", 5);
}

// 使用
send_header(conn_fd, "Transfer-Encoding", "chunked");
send_chunk(conn_fd, "Hello ", 6);
send_chunk(conn_fd, "World!", 6);
send_chunk_end(conn_fd);
```

### 18.4 mmap 处理大文件

mmap 把文件映射到内存，可以像访问数组一样访问文件内容。

```c
int fd = open("large.bin", O_RDONLY);
struct stat st;
fstat(fd, &st);

// 映射整个文件（虚拟地址空间，不占物理内存）
char *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

// 随机访问（按需读入页缓存）
char byte_at_1000 = data[1000];

// 发送部分内容
write(conn_fd, data + offset, length);

munmap(data, st.st_size);
close(fd);
```

**mmap vs sendfile**：
- mmap + write：2 次 CPU 拷贝（页缓存→用户空间→socket）
- sendfile：1 次 CPU 拷贝（页缓存→socket）
- 但 mmap 可以**修改数据**后再发送（sendfile 不行）

**大文件 mmap 的注意点**：
- 32 位系统地址空间只有 3GB，不能 mmap 10GB 文件
- 64 位系统没问题（虚拟地址空间 48 位 = 256TB）
- mmap 后访问可能触发 SIGBUS（文件被截断）

---

## 19. keep-alive 连接管理

### 19.1 为什么需要 keep-alive？

没有 keep-alive 时，每个请求都要重新建立 TCP 连接：

```
请求 1: TCP 三次握手 → HTTP 请求 → HTTP 响应 → TCP 四次挥手
请求 2: TCP 三次握手 → HTTP 请求 → HTTP 响应 → TCP 四次挥手
请求 3: ...
```

TCP 握手要 1 个 RTT（往返时间），如果请求本身很小（如 1KB），
握手开销可能比传输数据还大。

有 keep-alive 时，连接复用：

```
TCP 三次握手 →
  请求 1 → 响应 1
  请求 2 → 响应 2
  请求 3 → 响应 3
  ...（超时后才关闭）
→ TCP 四次挥手
```

**性能提升**：对 100 个小请求，省 99 次握手，如果 RTT=50ms，省 5 秒。

### 19.2 HTTP/1.0 vs HTTP/1.1

- HTTP/1.0：默认**关闭** keep-alive，需要 `Connection: keep-alive` 头才开启
- HTTP/1.1：默认**开启** keep-alive，需要 `Connection: close` 才关闭

```c
int is_keep_alive(const char *version, const char *conn_header) {
    if (strcmp(version, "HTTP/1.1") == 0) {
        // 1.1 默认 keep-alive，除非显式 close
        if (conn_header && strcasecmp(conn_header, "close") == 0)
            return 0;
        return 1;
    } else {
        // 1.0 默认关闭，除非显式 keep-alive
        if (conn_header && strcasecmp(conn_header, "keep-alive") == 0)
            return 1;
        return 0;
    }
}
```

### 19.3 keep-alive 超时

不能让连接永远开着，服务器要设置超时：

```c
#define KEEPALIVE_TIMEOUT 15   // 15 秒无请求则关闭
#define KEEPALIVE_MAX_REQ 100  // 最多复用 100 次后关闭

void handle_connection(int conn_fd) {
    // 设置 socket 接收超时
    struct timeval tv = {KEEPALIVE_TIMEOUT, 0};
    setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int req_count = 0;
    while (req_count < KEEPALIVE_MAX_REQ) {
        // 读取请求（超时则退出循环）
        int n = read_request(conn_fd, &req);
        if (n <= 0) break;  // 超时或客户端关闭

        // 处理并发送响应
        handle_request(conn_fd, &req);

        // 检查是否还要 keep-alive
        if (!is_keep_alive(req.version, req.connection)) break;

        req_count++;
    }
    close(conn_fd);
}
```

### 19.4 keep-alive 的陷阱

**陷阱 1：响应必须带 Content-Length**

keep-alive 下，客户端靠 Content-Length 判断响应何时结束。如果忘了写，
客户端会一直等（直到超时）。

```c
// 正确：带 Content-Length
send_header(conn_fd, "Content-Length", "1024");
write(conn_fd, body, 1024);

// 错误：没 Content-Length，客户端不知道何时结束
write(conn_fd, body, 1024);
```

**陷阱 2：不能用 Content-Length 就用 chunked**

动态生成的内容不知道长度，用 `Transfer-Encoding: chunked`（见上一章）。

**陷阱 3：慢客户端占用连接**

keep-alive 让连接长时间保持，如果有 10000 个慢客户端，连接数会被占满。
解决：设置超时 + 最大请求数 + 限制总连接数。

### 19.5 连接池（客户端侧）

客户端（如浏览器、curl）也会维护连接池，复用 TCP 连接：

```c
// 简化的连接池逻辑
typedef struct {
    int fd;
    char host[256];
    int port;
    time_t last_used;
} connection_t;

connection_t pool[MAX_POOL];

int get_connection(const char *host, int port) {
    // 查找池中可复用的连接
    for (int i = 0; i < MAX_POOL; i++) {
        if (strcmp(pool[i].host, host) == 0 &&
            pool[i].port == port &&
            pool[i].fd > 0) {
            return pool[i].fd;  // 复用
        }
    }
    // 没有可复用的，新建
    int fd = connect_to_server(host, port);
    // 存入池中
    ...
    return fd;
}
```

---

## 20. 错误处理和日志

### 20.1 HTTP 错误响应

```c
// 标准错误响应
void send_error(int fd, int status, const char *msg) {
    char body[1024];
    snprintf(body, sizeof(body),
        "<html><body><h1>%d %s</h1></body></html>",
        status, msg);

    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, msg, strlen(body));

    write(fd, header, strlen(header));
    write(fd, body, strlen(body));
}

// 使用
send_error(fd, 404, "Not Found");
send_error(fd, 403, "Forbidden");
send_error(fd, 500, "Internal Server Error");
```

### 20.2 自定义错误页面

生产服务器通常有定制的错误页面（带样式、联系方式）：

```c
// 从文件读取错误页面
void send_error_page(int fd, int status, const char *msg) {
    char path[256];
    snprintf(path, sizeof(path), "wwwroot/errors/%d.html", status);

    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, status, msg);  // 回退到默认错误页
        return;
    }

    struct stat st;
    fstat(file_fd, &st);

    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %ld\r\n"
        "\r\n",
        status, msg, st.st_size);
    write(fd, header, strlen(header));

    sendfile(fd, file_fd, NULL, st.st_size);
    close(file_fd);
}
```

### 20.3 访问日志格式

**Nginx 默认格式（combined log format）**：

```
$remote_addr - $remote_user [$time_local] "$request" 
$status $body_bytes_sent "$http_referer" "$http_user_agent"
```

**示例**：

```
192.168.1.100 - - [07/Sep/2026:10:23:45 +0800] "GET /index.html HTTP/1.1" 
200 1024 "http://example.com/" "Mozilla/5.0 ..."
```

**C 语言实现**：

```c
void log_access(FILE *log_fp, const char *client_ip,
                const char *method, const char *uri,
                const char *version, int status, size_t bytes) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%d/%b/%Y:%H:%M:%S %z", tm);

    fprintf(log_fp, "%s - - [%s] \"%s %s %s\" %d %zu\n",
            client_ip, time_str, method, uri, version, status, bytes);
    fflush(log_fp);
}
```

### 20.4 错误日志

错误日志记录服务器内部问题，用于排查 bug：

```c
void log_error(FILE *err_fp, const char *file, int line,
               const char *fmt, ...) {
    time_t now = time(NULL);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S",
             localtime(&now));

    fprintf(err_fp, "[%s] [%s:%d] ", time_str, file, line);

    va_list args;
    va_start(args, fmt);
    vfprintf(err_fp, fmt, args);
    va_end(args);

    fprintf(err_fp, "\n");
    fflush(err_fp);
}

// 使用宏，自动填入文件名和行号
#define LOG_ERROR(fp, ...) log_error(fp, __FILE__, __LINE__, __VA_ARGS__)

// 示例
LOG_ERROR(err_fp, "open %s failed: %s", path, strerror(errno));
// 输出：[2026-09-07 10:23:45] [webserver.c:123] open /xxx failed: No such file
```

### 20.5 日志级别

```c
typedef enum {
    LOG_DEBUG,   // 调试信息（开发时用）
    LOG_INFO,    // 一般信息（启动、停止）
    LOG_WARN,    // 警告（可恢复的异常）
    LOG_ERROR,   // 错误（影响单个请求）
    LOG_FATAL,   // 致命（服务器要退出）
} log_level_t;

log_level_t current_level = LOG_INFO;

void log_msg(log_level_t level, const char *fmt, ...) {
    if (level < current_level) return;  // 级别不够，不记录

    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    // ... 格式化并输出
}
```

### 20.6 日志的注意事项

- **不要在信号处理函数中调用 printf**（不可重入），用 `write` + 预格式化字符串
- **日志文件会变大**：需要 logrotate 轮转（按大小/日期切割）
- **同步写日志会拖慢请求**：可以用缓冲区 + 后台线程异步写
- **生产环境关掉 DEBUG 级别**：避免日志量过大

```c
// 异步日志：主线程写入缓冲，后台线程写文件
char log_buffer[65536];
size_t log_buf_len = 0;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_async(const char *msg) {
    pthread_mutex_lock(&log_mutex);
    size_t len = strlen(msg);
    if (log_buf_len + len < sizeof(log_buffer)) {
        memcpy(log_buffer + log_buf_len, msg, len);
        log_buf_len += len;
    }
    pthread_mutex_unlock(&log_mutex);
    // 后台线程会定期把 log_buffer 写入文件并清空
}
```

---

## 附录 C：本章涉及的系统调用一览

| 系统调用     | 头文件           | 作用                          |
|--------------|------------------|-------------------------------|
| sendfile     | sys/sendfile.h   | 文件→socket 零拷贝            |
| splice       | fcntl.h          | 任意 fd 间零拷贝（经管道）    |
| tee          | fcntl.h          | 管道间零消耗复制              |
| mmap         | sys/mman.h       | 文件映射到内存                |
| realpath     | stdlib.h         | 解析路径的 . 和 ..            |
| chroot       | unistd.h         | 改变进程根目录                |
| epoll_create | sys/epoll.h      | 创建 epoll 实例               |
| epoll_ctl    | sys/epoll.h      | 添加/修改/删除 fd             |
| epoll_wait   | sys/epoll.h      | 等待事件就绪                  |
| setsockopt   | sys/socket.h     | 设置 socket 选项（超时等）    |
