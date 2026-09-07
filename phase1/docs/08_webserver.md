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
