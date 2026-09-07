# 03 - 阶段 5-7：HTTP 解析与 Web 服务器

> 从 echo 升级到 HTTP，理解状态机解析、Reactor 模式、sendfile 零拷贝。
>
> 这一篇会从 HTTP/1.1 协议讲起，把状态机解析器、主从 Reactor、零拷贝
> 这些现代 Web 服务器的核心技术讲透。读完之后你不仅理解 Nginx 的设计，
> 还能自己写一个高性能 HTTP server。

## 目录

1. [stage5：HTTP 状态机解析](#1-stage5http-状态机解析)
2. [HTTP/1.1 协议详解](#2-http11-协议详解)
3. [状态机解析器代码讲解](#3-状态机解析器代码讲解)
4. [分包和粘包处理](#4-分包和粘包处理)
5. [keep-alive 实现细节](#5-keep-alive-实现细节)
6. [stage6：主从 Reactor](#6-stage6主从-reactor)
7. [线程间通信](#7-线程间通信)
8. [stage7：完整 Web 服务器](#8-stage7完整-web-服务器)
9. [sendfile 零拷贝详解](#9-sendfile-零拷贝详解)
10. [mmap 原理和适用场景](#10-mmap-原理和适用场景)
11. [MIME 类型完整表](#11-mime-类型完整表)
12. [路由设计详解](#12-路由设计详解)
13. [压测结果分析](#13-压测结果分析)
14. [思考题和常见问题](#14-思考题和常见问题)

---

## 1. stage5：HTTP 状态机解析

### 1.1 为什么需要状态机？

TCP 是**字节流**，没有消息边界。一次 read 可能返回：

```
情况1（分包）：read 返回 "GET / HTTP/1.1\r\nHost: local"
情况2（完整）：read 返回 "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
情况3（粘包）：read 返回 "GET / HTTP/1.1\r\n\r\nGET /favicon HTTP/1.1\r\n"
```

如果用 `strstr("\r\n\r\n")` 找请求结束：
- 情况1：找不到，但数据已经收到一半，怎么办？
- 情况3：找到了，但后面还有半个请求，怎么处理？

### 1.2 状态机设计

```
HTTP_STATE_START  → 解析请求行 "GET /path HTTP/1.1"
      ↓
HTTP_STATE_HEADER → 解析头部 "Host: localhost"
      ↓
HTTP_STATE_BODY   → 解析 body（根据 Content-Length）
      ↓
HTTP_STATE_DONE   → 解析完成，可以处理请求
```

### 1.3 增量解析器接口

```c
// 每次收到数据就"喂"给解析器
http_parse_result_t http_parser_feed(parser, data, len);

// 返回值：
//   NEED_MORE：数据不够，继续 read
//   DONE：解析完成，可以处理请求了
//   ERROR：格式错误，关闭连接
```

解析器内部维护缓冲区和当前状态。数据不够时返回 NEED_MORE，
应用层继续 read 再喂。这样自然处理了分包。

### 1.4 粘包处理

```
一次 read 收到："请求1完整\r\n\r\n请求2前半"
                ↑               ↑
            parser 消费到这    剩余留在缓冲区

解析完请求1后：
  http_parser_reset(parser)  // 重置状态，保留剩余数据
  // 剩余的"请求2前半"在 parser->buf 里
  // 下次 read 的数据追加到后面，继续解析请求2
```

### 1.5 keep-alive

HTTP/1.1 默认 keep-alive：一个 TCP 连接可以发多个请求。

```
连接建立 → 请求1 → 响应1 → 请求2 → 响应2 → ... → FIN
```

不用每个请求都三次握手，性能更好。
但要求 server 解析完一个请求后不关连接，继续等下一个。

---

## 2. HTTP/1.1 协议详解

### 2.1 HTTP 请求格式

```
<method> <uri> <version>\r\n
<header1>: <value1>\r\n
<header2>: <value2>\r\n
...
\r\n
<body>
```

具体例子：

```
GET /api/users?id=123 HTTP/1.1\r\n
Host: localhost:8080\r\n
User-Agent: curl/7.68.0\r\n
Accept: */*\r\n
\r\n
```

### 2.2 HTTP 响应格式

```
<version> <status_code> <reason>\r\n
<header1>: <value1>\r\n
<header2>: <value2>\r\n
...
\r\n
<body>
```

具体例子：

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 13\r\n
Connection: keep-alive\r\n
\r\n
Hello, World!
```

### 2.3 请求方法

| 方法 | 说明 | 幂等？ | 安全？ |
|------|------|--------|--------|
| GET | 获取资源 | 是 | 是 |
| POST | 创建资源 | 否 | 否 |
| PUT | 更新资源 | 是 | 否 |
| DELETE | 删除资源 | 是 | 否 |
| HEAD | 只取头部 | 是 | 是 |
| OPTIONS | 查询支持的方法 | 是 | 是 |
| PATCH | 部分更新 | 否 | 否 |
| TRACE | 调试用 | 是 | 是 |
| CONNECT | 建立隧道（HTTPS） | 否 | 否 |

**幂等**：多次执行结果相同（GET 多次和一次效果一样）。
**安全**：不改变服务器状态（GET 不应该有副作用）。

### 2.4 状态码

| 范围 | 类别 | 说明 |
|------|------|------|
| 1xx | 信息 | 100 Continue, 101 Switching Protocols |
| 2xx | 成功 | 200 OK, 201 Created, 204 No Content |
| 3xx | 重定向 | 301 Moved, 302 Found, 304 Not Modified |
| 4xx | 客户端错误 | 400 Bad Request, 401 Unauthorized, 403 Forbidden, 404 Not Found, 429 Too Many |
| 5xx | 服务器错误 | 500 Internal Error, 502 Bad Gateway, 503 Unavailable, 504 Timeout |

常用状态码详解：

| 码 | 含义 | 何时用 |
|----|------|--------|
| 200 | OK | 请求成功 |
| 201 | Created | POST 创建成功 |
| 204 | No Content | 成功但无内容返回 |
| 301 | Moved Permanently | 永久重定向 |
| 302 | Found | 临时重定向 |
| 304 | Not Modified | 缓存有效，不用重新下载 |
| 400 | Bad Request | 请求格式错误 |
| 401 | Unauthorized | 未认证（要登录） |
| 403 | Forbidden | 已认证但无权限 |
| 404 | Not Found | 资源不存在 |
| 405 | Method Not Allowed | 方法不支持（如 POST 只读资源） |
| 408 | Request Timeout | 请求超时 |
| 413 | Payload Too Large | 请求体太大 |
| 429 | Too Many Requests | 限流 |
| 500 | Internal Server Error | 服务器内部错误 |
| 502 | Bad Gateway | 网关错误 |
| 503 | Service Unavailable | 服务不可用（过载） |
| 504 | Gateway Timeout | 网关超时 |

### 2.5 常用请求头

| 头部 | 说明 | 示例 |
|------|------|------|
| Host | 主机名（HTTP/1.1 必需） | `Host: localhost:8080` |
| User-Agent | 客户端标识 | `User-Agent: curl/7.68.0` |
| Accept | 可接受的内容类型 | `Accept: text/html,*/*` |
| Accept-Encoding | 可接受的编码 | `Accept-Encoding: gzip, deflate` |
| Accept-Language | 可接受的语言 | `Accept-Language: zh-CN,zh;q=0.9` |
| Content-Type | 请求体类型 | `Content-Type: application/json` |
| Content-Length | 请求体长度 | `Content-Length: 42` |
| Connection | 连接管理 | `Connection: keep-alive` |
| Cookie | Cookie | `Cookie: session=abc123` |
| Authorization | 认证信息 | `Authorization: Bearer token` |
| Referer | 来源页面 | `Referer: https://example.com/` |
| Origin | CORS 来源 | `Origin: https://example.com` |
| If-Modified-Since | 缓存检查 | `If-Modified-Since: Sat, 01 Jan 2024 00:00:00 GMT` |
| If-None-Match | ETag 检查 | `If-None-Match: "abc123"` |
| Range | 范围请求 | `Range: bytes=0-1023` |

### 2.6 常用响应头

| 头部 | 说明 | 示例 |
|------|------|------|
| Content-Type | 响应体类型 | `Content-Type: text/html; charset=utf-8` |
| Content-Length | 响应体长度 | `Content-Length: 1234` |
| Content-Encoding | 响应体编码 | `Content-Encoding: gzip` |
| Connection | 连接管理 | `Connection: keep-alive` |
| Set-Cookie | 设置 Cookie | `Set-Cookie: session=abc; HttpOnly` |
| Location | 重定向目标 | `Location: https://example.com/new` |
| Cache-Control | 缓存控制 | `Cache-Control: max-age=3600` |
| ETag | 资源版本标识 | `ETag: "abc123"` |
| Last-Modified | 最后修改时间 | `Last-Modified: Sat, 01 Jan 2024 00:00:00 GMT` |
| Server | 服务器标识 | `Server: nginx/1.18` |
| Date | 响应时间 | `Date: Sat, 01 Jan 2024 00:00:00 GMT` |
| Access-Control-Allow-Origin | CORS 允许 | `Access-Control-Allow-Origin: *` |
| X-Forwarded-For | 代理链 | `X-Forwarded-For: 1.2.3.4` |
| Transfer-Encoding | 传输编码 | `Transfer-Encoding: chunked` |

### 2.7 Content-Type 常见值

| Content-Type | 说明 |
|--------------|------|
| text/html | HTML |
| text/plain | 纯文本 |
| text/css | CSS |
| text/javascript | JavaScript |
| application/json | JSON |
| application/xml | XML |
| application/octet-stream | 二进制流 |
| application/x-www-form-urlencoded | 表单 |
| multipart/form-data | 文件上传 |
| image/jpeg | JPEG 图片 |
| image/png | PNG 图片 |
| image/gif | GIF 图片 |
| video/mp4 | MP4 视频 |
| audio/mpeg | MP3 音频 |

### 2.8 Connection 头部

```
Connection: keep-alive  ← 保持连接（HTTP/1.1 默认）
Connection: close       ← 响应后关闭
```

HTTP/1.1 默认 keep-alive，要关闭就显式写 `Connection: close`。
HTTP/1.0 默认 close，要保持就写 `Connection: keep-alive`。

### 2.9 chunked 传输编码

当不知道 Content-Length 时（如动态生成的内容），用 chunked：

```
HTTP/1.1 200 OK\r\n
Transfer-Encoding: chunked\r\n
\r\n
4\r\n
Wiki\r\n
6\r\n
pedia \r\n
E\r\n
in\r\n\r\nchunks.\r\n
0\r\n
\r\n
```

每个 chunk 前是十六进制长度，`0\r\n\r\n` 表示结束。

---

## 3. 状态机解析器代码讲解

### 3.1 解析器结构

```c
typedef enum {
    HTTP_STATE_START,    /* 解析请求行 */
    HTTP_STATE_HEADER,   /* 解析头部 */
    HTTP_STATE_BODY,     /* 解析 body */
    HTTP_STATE_DONE      /* 完成 */
} http_state_t;

typedef struct {
   , char     method[16];
    char     uri[1024];
    char     version[16];
    char     host[256];
    int      content_length;
    int      keep_alive;
} http_request_t;

typedef struct {
    http_state_t  state;
    char          buf[8192];      /* 内部缓冲区 */
    int           buf_len;
    int           body_read;      /* 已读 body 字节数 */
    http_request_t request;
} http_parser_t;
```

### 3.2 初始化

```c
void http_parser_init(http_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = HTTP_STATE_START;
    p->request.keep_alive = 1;  /* HTTP/1.1 默认 keep-alive */
}
```

### 3.3 feed 函数

```c
http_parse_result_t http_parser_feed(http_parser_t *p,
                                      const char *data, int len)
{
    /* 1. 把新数据追加到缓冲区 */
    if (len > 0) {
        if (p->buf_len + len > sizeof(p->buf)) {
            return HTTP_PARSE_ERROR;  /* 缓冲区溢出 */
        }
        memcpy(p->buf + p->buf_len, data, len);
        p->buf_len += len;
    }

    /* 2. 根据当前状态解析 */
    for (;;) {
        switch (p->state) {
        case HTTP_STATE_START:
            if (!parse_request_line(p)) return HTTP_PARSE_NEED_MORE;
            p->state = HTTP_STATE_HEADER;
            break;

        case HTTP_STATE_HEADER:
            if (!parse_headers(p)) return HTTP_PARSE_NEED_MORE;
            if (p->request.content_length > 0) {
                p->state = HTTP_STATE_BODY;
            } else {
                p->state = HTTP_STATE_DONE;
                return HTTP_PARSE_DONE;
            }
            break;

        case HTTP_STATE_BODY:
            if (!parse_body(p)) return HTTP_PARSE_NEED_MORE;
            p->state = HTTP_STATE_DONE;
            return HTTP_PARSE_DONE;

        default:
            return HTTP_PARSE_ERROR;
        }
    }
}
```

### 3.4 解析请求行

```c
static int parse_request_line(http_parser_t *p)
{
    /* 找 \r\n */
    char *eol = memchr(p->buf, '\n', p->buf_len);
    if (!eol) return 0;  /* 没找到，需要更多数据 */

    /* 解析 "GET /path HTTP/1.1\r\n" */
    char *sp1 = strchr(p->buf, ' ');
    if (!sp1) return -1;

    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return -1;

    /* 提取 method */
    int method_len = sp1 - p->buf;
    memcpy(p->request.method, p->buf, method_len);
    p->request.method[method_len] = '\0';

    /* 提取 uri */
    int uri_len = sp2 - sp1 - 1;
    memcpy(p->request.uri, sp1 + 1, uri_len);
    p->request.uri[uri_len] = '\0';

    /* 提取 version */
    char *eol_real = eol;
    if (*(eol - 1) == '\r') eol_real = eol - 1;
    int ver_len = eol_real - sp2 - 1;
    memcpy(p->request.version, sp2 + 1, ver_len);
    p->request.version[ver_len] = '\0';

    /* 消费已解析的数据 */
    int consumed = eol - p->buf + 1;
    memmove(p->buf, eol + 1, p->buf_len - consumed);
    p->buf_len -= consumed;

    return 1;
}
```

### 3.5 解析头部

```c
static int parse_headers(http_parser_t *p)
{
    for (;;) {
        char *eol = memchr(p->buf, '\n', p->buf_len);
        if (!eol) return 0;  /* 需要更多数据 */

        /* 空行 → 头部结束 */
        if (eol == p->buf || (eol == p->buf + 1 && p->buf[0] == '\r')) {
            int consumed = eol - p->buf + 1;
            memmove(p->buf, eol + 1, p->buf_len - consumed);
            p->buf_len -= consumed;
            return 1;
        }

        /* 解析 "Key: Value" */
        char *colon = strchr(p->buf, ':');
        if (!colon) return -1;

        char *key = p->buf;
        int key_len = colon - p->buf;

        char *val = colon + 1;
        while (*val == ' ') val++;  /* 跳过空格 */
        char *eol_real = eol;
        if (*(eol - 1) == '\r') eol_real = eol - 1;
        int val_len = eol_real - val;

        /* 处理已知头部 */
        if (strncasecmp(key, "Host", 4) == 0) {
            memcpy(p->request.host, val, val_len);
            p->request.host[val_len] = '\0';
        } else if (strncasecmp(key, "Content-Length", 14) == 0) {
            char tmp[32];
            memcpy(tmp, val, val_len);
            tmp[val_len] = '\0';
            p->request.content_length = atoi(tmp);
        } else if (strncasecmp(key, "Connection", 10) == 0) {
            if (strncasecmp(val, "close", 5) == 0) {
                p->request.keep_alive = 0;
            }
        }

        /* 消费这一行 */
        int consumed = eol - p->buf + 1;
        memmove(p->buf, eol + 1, p->buf_len - consumed);
        p->buf_len -= consumed;
    }
}
```

### 3.6 解析 body

```c
static int parse_body(http_parser_t *p)
{
    int need = p->request.content_length;
    if (p->buf_len >= need) {
        /* body 收齐了 */
        /* 这里可以拷贝 body 或只记录指针 */
        p->body_read = need;

        /* 消费 body 数据 */
        memmove(p->buf, p->buf + need, p->buf_len - need);
        p->buf_len -= need;

        return 1;
    }
    return 0;  /* 需要更多数据 */
}
```

### 3.7 重置解析器（keep-alive）

```c
void http_parser_reset(http_parser_t *p)
{
    http_state_t old_state = p->state;
    char *old_buf = p->buf;
    int old_buf_len = p->buf_len;

    /* 重置状态，但保留缓冲区里的剩余数据（粘包） */
    memset(p, 0, sizeof(*p));
    p->state = HTTP_STATE_START;
    p->request.keep_alive = 1;

    /* 恢复缓冲区 */
    p->buf_len = old_buf_len;
    /* buf 内容没动，因为 memset 把 buf 也清了，要恢复 */
    /* 实际实现要小心，这里简化了 */
}
```

---

## 4. 分包和粘包处理

### 4.1 什么是分包和粘包？

TCP 是字节流，没有消息边界。应用层发的"消息"可能被拆成多个 TCP 段，
也可能多个消息合并成一个段。

**分包**：一个 HTTP 请求被拆成多个 TCP 段到达

```
客户端发：  "GET / HTTP/1.1\r\nHost: a.com\r\n\r\n"
服务器收到：
  read 1: "GET / HTTP/1.1\r\nHost: a"
  read 2: ".com\r\n\r\n"
```

**粘包**：多个 HTTP 请求合并成一个 TCP 段到达

```
客户端发：  请求1 + 请求2
服务器收到：
  read: "请求1完整\r\n\r\n请求2完整\r\n\r\n"
```

### 4.2 分包的处理

状态机天然处理分包：数据不够时返回 NEED_MORE，应用层继续 read。

```
read 1: "GET / HTTP/1.1\r\nHost: a"
  → feed → 解析请求行 OK，解析头部需要更多 → NEED_MORE
 Bread 2: ".com\r\n\r\n"
  → feed → 追加到缓冲区，继续解析头部 → DONE
```

### 4.3 粘包的处理

解析完一个请求后，缓冲区可能还有剩余数据（下一个请求的开头）：

```
缓冲区：  "请求1完整\r\n\r\n请求2前半"
            ↑               ↑
        解析完请求1      剩余数据

处理：
  1. 解析请求1 → DONE
  2. 处理请求1（发响应）
  3. reset parser，保留剩余数据
  4. 继续解析剩余数据（请求2前半）
  5. 如果不够 → NEED_MORE → 继续 read
```

### 4.4 代码实现

```c
int conn_handle_read(conn_t *conn, ...)
{
    char buf[4096];

    for (;;) {
        ssize_t n = read(conn->fd, buf, sizeof(buf));

        if (n > 0) {
            http_parse_result_t result;
            result = http_parser_feed(&conn->parser, buf, n);

            if (result == HTTP_PARSE_DONE) {
                /* 处理请求 */
                dispatch_request(conn->fd, &conn->parser.request);

                /* keep-alive：重置解析器 */
                http_parser_reset(&conn->parser);

                /* 检查缓冲区是否有残留（粘包） */
                if (conn->parser.buf_len > 0) {
                    /* 尝试解析下一个请求 */
                    http_parse_result_t r2;
                    r2 = http_parser_feed(&conn->parser, "", 0);
                    if (r2 == HTTP_PARSE_DONE) {
                        dispatch_request(conn->fd, &conn->parser.request);
                        http_parser_reset(&conn->parser);
                    }
                    /* 不够就继续 read */
                }
                continue;

            } else if (result == HTTP_PARSE_ERROR) {
                send_400(conn->fd);
                return 0;  /* 关闭连接 */
            }
            /* NEED_MORE：继续 read */

        } else if (n == 0) {
            return 0;  /* 对端关闭 */
        } else {
            if (errno == EAGAIN) return 1;  /* 读完了 */
            if (errno == EINTR) continue;
            return 0;  /* 错误 */
        }
    }
}
```

### 4.5 分包粘包图解

```
时间轴：
  t1: 客户端发 "GET / HTTP/1.1\r\nHost: a.com\r\n\r\nGET /2 HTTP/1.1\r\n"
  t2: 网络传输，可能拆成多个段
  t3: 服务器 read 第一次：返回 "GET / HTTP/1.1\r\nHost: a.com\r\n\r\nGET /2"
  t4:   → 解析出请求1（GET /）
  t5:   → 处理请求1，发响应1
  t6:   → reset parser，缓冲区剩 "GET /2"
  t7:   → 尝试解析 "GET /2"，不够 → NEED_MORE
  t8: 服务器 read 第二次：返回 " HTTP/1.1\r\nHost: a.com\r\n\r\n"
  t9:   → 追加到缓冲区："GET /2 HTTP/1.1\r\nHost: a.com\r\n\r\n"
  t10:  → 解析出请求2（GET /2）
  t11:  → 处理请求2，发响应2
```

---

## 5. keep-alive 实现细节

### 5.1 为什么 keep-alive 重要？

没有 keep-alive：

```
请求1: 三次握手 → 请求 → 响应 → 四次挥手  (2 RTT + 数据)
请求2: 三次握手 → 请求 → 响应 → 四次挥手  (2 RTT + 数据)
请求3: 三次握手 → 请求 → 响应 → 四次挥手  (2 RTT + 数据)
```

有 keep-alive：

```
三次握手 → 请求1 → 响应1 → 请求2 → 响应2 → 请求3 → 响应3 → 四次挥手
         (1 RTT + 3 * 数据)
```

省了 2 次握手和 2 次挥手，延迟大幅降低。

### 5.2 keep-alive 的实现

```c
/* 解析完一个请求后 */
if (result == HTTP_PARSE_DONE) {
    /* 处理请求 */
    dispatch_request(conn->fd, &conn->parser.request);

    if (conn->parser.request.keep_alive) {
        /* keep-alive：重置解析器，继续等下一个请求 */
        http_parser_reset(&conn->parser);
        /* 不关连接，继续 epoll 监听 */
    } else {
        /* Connection: close，发完响应就关 */
        send_response(conn->fd);
        close(conn->fd);
        return 0;
    }
}
```

### 5.3 keep-alive 超时

不能让连接永远开着。设置超时：

```c
/* epoll_wait 用超时 */
int n = epoll_wait(epfd, events, MAX, 5000);  /* 5 秒 */

if (n == 0) {
    /* 超时，关闭空闲连接 */
    time_t now = time(NULL);
    for (int i = 0; i < num_conns; i++) {
        if (now - conns[i]->last_active > 30) {
            close(conns[i]->fd);
            /* ... */
        }
    }
}
```

### 5.4 keep-alive 相关头部

```
Keep-Alive: timeout=30, max=100
```

- `timeout=30`：30 秒后关闭空闲连接
- `max=100`：最多 100 个请求后关闭

### 5.5 HTTP/2 多路复用

HTTP/2 的多路复用比 keep-alive 更强：
- 一个 TCP 连接同时发多个请求（不用等响应）
- 请求 2 不用等请求 1 的响应
- 彻底解决 HTTP/1.1 的队头阻塞

---

## 6. stage6：主从 Reactor

### 6.1 为什么单线程不够？

stage5 的单线程 epoll 在多核 CPU 上只能用一个核。
高并发下，单线程的处理速度成为瓶颈。

### 6.2 主从 Reactor 架构

```
                    ┌─────────────┐
                    │  主 reactor  │  ← 只负责 accept
                    │  (主线程)    │
                    └──────┬──────┘
                           │ round-robin 分发
              ┌────────────┼────────────┐
              ↓            ↓            ↓
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │ worker 0 │ │ worker 1 │ │ worker 2 │
        │ epoll    │ │ epoll    │ │ epoll    │
        │ 处理 IO  │ │ 处理 IO  │ │ 处理 IO  │
        └──────────┘ └──────────┘ └──────────┘
```

- 主线程：epoll 只监听 listen_fd，accept 新连接
- 工作线程：各自有独立的 epoll，处理分配给自己的连接
- 主线程通过 pipe 通知工作线程有新连接

### 6.3 线程间通信

```c
// 主线程：accept 后通过 pipe 通知工作线程
write(worker_pipe[1], &conn_fd, sizeof(int));

// 工作线程：epoll 监听 pipe，收到通知后把 conn_fd 加入自己的 epoll
if (fd == notify_fd) {
    read(notify_fd, &conn_fd, sizeof(int));
    epoll_ctl(my_epfd, EPOLL_CTL_ADD, conn_fd, &ev);
}
```

为什么用 pipe？直观、可传任意数据。生产环境用 eventfd 更高效。

### 6.4 为什么不用共享 epoll + 锁？

- 多线程竞争同一个 epoll，锁开销大
- 主从 Reactor 每个线程独立 epoll，无锁，扩展性好

### 6.5 惊群问题

如果多个线程同时 epoll_wait 同一个 listen_fd，一个连接到来时所有线程被唤醒，
但只有一个 accept 成功，其他白醒。这就是"惊群"。

主从 Reactor 避免了这个问题：只有主线程监听 listen_fd。

### 6.6 主线程代码

```c
/* 主 reactor 循环 */
int epfd = epoll_create(1);
struct epoll_event ev;
ev.events  = EPOLLIN | EPOLLET;
ev.data.fd = listen_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

struct epoll_event events[64];

while (g_running) {
    int n = epoll_wait(epfd, events, 64, 1000);

    for (int i = 0; i < n; i++) {
        if (events[i].data.fd != listen_fd) continue;

        /* ET 模式：循环 accept */
        for (;;) {
            int conn_fd = accept(listen_fd, NULL, NULL);
            if (conn_fd < 0) {
                if (errno == EAGAIN) break;
                continue;
            }

            /* 分发给工作线程 */
            worker_dispatch(workers, num_workers, conn_fd);
        }
    }
}
```

### 6.7 工作线程代码

```c
/* 工作线程主循环 */
void *worker_loop(void *arg)
{
    worker_t *w = (worker_t *)arg;
    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == w->notify_fd) {
                /* 新连接通知 */
                int conn_fd;
                while (read(w->notify_fd, &conn_fd, sizeof(int)) > 0) {
                    set_nonblocking(conn_fd);
                    conn_t *conn = conn_create(conn_fd);

                    struct epoll_event ev;
                    ev.events   = EPOLLIN | EPOLLET;
                    ev.data.ptr = conn;
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);
                }
            } else {
                /* 连接有数据 */
                conn_t *conn = events[i].data.ptr;
                int alive = conn_handle_read(conn, ...);
                if (!alive) {
                    close(conn->fd);
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    conn_free(conn);
                }
            }
        }
    }
}
```

### 6.8 分发策略

```c
static int next_worker = 0;

void worker_dispatch(worker_t *workers, int num, int conn_fd)
{
    /* round-robin：轮流分发 */
    int target = next_worker;
    next_worker = (next_worker + 1) % num;

    /* 通过 pipe 通知 */
    write(workers[target].write_fd, &conn_fd, sizeof(int));
}
```

其他策略：
- **最少连接**：分发给当前连接数最少的工作线程
- **CPU 亲和**：分发给当前最空闲的 CPU 上的线程
- **hash**：按客户端 IP hash，同一客户端总是同一线程

---

## 7. 线程间通信

### 7.1 pipe

```c
int pipe_fd[2];
pipe(pipe_fd);  /* pipe_fd[0] 读，pipe_fd[1] 写 */

/* 线程 A 写 */
write(pipe_fd[1], &data, sizeof(data));

/* 线程 B 读 */
read(pipe_fd[0], &data, sizeof(data));
```

优点：简单，可传任意数据，可 epoll 监听。
缺点：内核缓冲区有限（默认 64KB），有拷贝开销。

### 7.2 eventfd

```c
int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

/* 线程 A 通知 */
uint64_t one = 1;
write(efd, &one, sizeof(one));

/* 线程 B 等待 */
uint64_t count;
read(efd, &count, sizeof(count));  /* count = 累积的通知次数 */
```

优点：内核只存一个 64 位计数器，开销极小。
缺点：只能传计数，不能传数据。

eventfd 是 Linux 2.6.22 引入的，专为线程间通知设计。
比 pipe 高效得多（无缓冲区，无拷贝）。

### 7.3 条件变量

```c
pthread_mutex_t mutex;
pthread_cond_t cond;

/* 线程 A 通知 */
pthread_mutex_lock(&mutex);
data_ready = 1;
pthread_cond_signal(&cond);
pthread_mutex_unlock(&mutex);

/* 线程 B 等待 */
pthread_mutex_lock(&mutex);
while (!data_ready) {
    pthread_cond_wait(&cond, &mutex);
}
/* 处理数据 */
pthread_mutex_unlock(&mutex);
```

优点：标准 POSIX，可移植。
缺点：不能和 epoll 一起用（不是 fd）。

### 7.4 三种方式的对比

| 方式 | 能传数据？ | 能 epoll？ | 开销 | 适用 |
|------|-----------|-----------|------|------|
| pipe | 是 | 是 | 中 | 通用 |
| eventfd | 否（只计数） | 是 | 小 | 通知 |
| 条件变量 | 是（共享内存） | 否 | 中 | 不用 epoll 的场景 |

Reactor 模式需要和 epoll 集成，所以用 pipe 或 eventfd。

### 7.5 用 eventfd 改进

```c
/* 创建 eventfd */
int efd = eventfd(0, EFD_NONBLOCK);

/* 加入 epoll */
struct epoll_event ev;
ev.events  = EPOLLIN;
ev.data.fd = efd;
epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev);

/* 主线程通知 */
uint64_t one = 1;
write(efd, &one, sizeof(one));

/* 工作线程处理 */
if (events[i].data.fd == efd) {
    uint64_t count;
    read(efd, &count, sizeof(count));
    /* 处理 count 个通知 */
}
```

---

## 8. stage7：完整 Web 服务器

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
