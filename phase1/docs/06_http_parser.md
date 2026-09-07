# stage5 - HTTP 状态机解析

> 从 echo 升级到 HTTP，理解状态机解析、分包粘包、keep-alive。
>
> TCP 是字节流没有消息边界，一次 read 可能返回半个请求或两个请求。
> 状态机解析器能正确处理分包和粘包，是现代 Web 服务器的基础。

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

## 6. HTTP 协议历史和演进

理解 HTTP 协议的演进，能帮助我们看懂为什么现在的解析器要设计成这个样子。每一个版本的改动，都是在解决上一版的痛点。

### 6.1 HTTP/0.9：一切的开端（1991 年）

Tim Berners-Lee 在 CERN 设计了最早的 HTTP，极其简单：

```
请求：GET /hello.html
响应：<html>...页面内容...</html>
```

特点：
- 只有 `GET` 方法
- 没有请求头、没有响应头
- 没有状态码、没有版本号
- 响应完立刻断开连接，一个连接只能发一个请求
- 只能传 HTML，不能传图片、视频等其他类型

这一版几乎没有"协议"的样子，就是一问一答的纯文本。

### 6.2 HTTP/1.0：引入头部（1996 年，RFC 1945）

HTTP/1.0 做了重大改进，开始像现代 HTTP 了：

```
请求：
GET /hello.html HTTP/1.0
Host: www.example.com
User-Agent: MyBrowser/1.0
Accept: text/html

响应：
HTTP/1.0 200 OK
Content-Type: text/html
Content-Length: 1234

<html>...</html>
```

新增的东西：
- **请求行带版本号**：`HTTP/1.0`
- **请求头**：`Host`、`User-Agent`、`Accept` 等
- **响应状态行**：`HTTP/1.0 200 OK`
- **响应头**：`Content-Type`、`Content-Length` 等
- **可以传任意类型**：图片、视频、二进制都行（靠 `Content-Type` 区分）

但 HTTP/1.0 有个大问题：**默认每个请求都要新建 TCP 连接**。TCP 三次握手 + 慢启动，每次都要重来，性能很差。虽然可以手动加 `Connection: keep-alive` 复用，但不是默认行为。

### 6.3 HTTP/1.1：keep-alive 和 chunked（1999 年，RFC 2616）

HTTP/1.1 是用得最久的版本，主要解决连接复用问题：

- **默认 keep-alive**：连接默认不关闭，可以连续发多个请求
- **chunked 传输编码**：不知道总长度时可以分块发送
- **pipelining**：可以一次发多个请求，但响应必须按序返回（实际很少用，因为容易队头阻塞）
- **Host 头强制**：支持虚拟主机，一个 IP 可以 host 多个域名
- **新增方法**：`PUT`、`DELETE`、`OPTIONS`、`CONNECT`、`TRACE`

```
GET /a HTTP/1.1
Host: www.example.com

GET /b HTTP/1.1
Host: www.example.com
```

上面两个请求可以共用一个 TCP 连接，这就是 keep-alive。

### 6.4 HTTP/2：二进制帧和多路复用（2015 年，RFC 7540）

HTTP/1.1 的文本格式虽然好读，但解析慢、有歧义。HTTP/2 改成二进制帧：

- **二进制分帧**：数据切成带长度前缀的帧，不再用 `\r\n` 分隔
- **多路复用**：一个连接上多个请求/响应交错传输，互不阻塞
- **头部压缩**：HPACK 算法压缩重复的头部
- **服务器推送**：服务器可以主动把资源推给客户端

```
帧格式（二进制）：
| 长度(3字节) | 类型(1字节) | 标志(1字节) | 流ID(4字节) | 载荷(变长) |
```

HTTP/2 解决了 HTTP/1.1 的队头阻塞，但底层还是 TCP，TCP 层仍有队头阻塞（丢包会卡住整个连接）。

### 6.5 HTTP/3：QUIC 和 UDP（2022 年，RFC 9114）

HTTP/3 把传输层从 TCP 换成了 QUIC（基于 UDP）：

- **QUIC 协议**：在 UDP 上实现可靠传输 + 加密 + 多路复用
- **无 TCP 队头阻塞**：一个流丢包不影响其他流
- **0-RTT 握手**：复用连接时可以 0 个往返就发数据
- **连接迁移**：手机从 WiFi 切到 4G，连接不断

```
HTTP/3 协议栈：
应用层：HTTP/3
传输层：QUIC（UDP 之上）
网络层：IP
```

### 6.6 各版本对比

| 版本 | 年份 | 传输层 | 格式 | 连接复用 | 多路复用 |
|------|------|--------|------|----------|----------|
| 0.9  | 1991 | TCP    | 文本 | 无       | 无       |
| 1.0  | 1996 | TCP    | 文本 | 手动     | 无       |
| 1.1  | 1999 | TCP    | 文本 | 默认     | 无       |
| 2    | 2015 | TCP    | 二进制 | 默认   | 有       |
| 3    | 2022 | QUIC   | 二进制 | 默认   | 有       |

我们这个教学项目实现的是 HTTP/1.1 的解析，掌握了 1.1 的状态机思路，再去看 2 和 3 的帧解析会容易很多——本质上还是"按状态切分字节流"。

---

## 7. 状态机设计模式深入

HTTP 解析器为什么非要用状态机？这一节我们从数学定义讲到工程实践，把状态机这个设计模式彻底讲透。

### 7.1 状态机三要素：状态、事件、转换

一个状态机由三样东西定义：

1. **状态（State）**：系统当前处于什么情况。比如解析 HTTP 请求时有"正在读方法"、"正在读头部"、"正在读body"等状态。
2. **事件（Event）**：发生了什么。比如"收到一个字节"、"遇到 `\r`"、"遇到 `\n`"。
3. **转换（Transition）**：在某个状态下收到某个事件，应该转到哪个新状态，并执行什么动作。

举个例子，解析 `GET\r\n` 时：

```
状态=读方法, 事件=收到'G'  → 状态=读方法, 动作=缓存'G'
状态=读方法, 事件=收到'E'  → 状态=读方法, 动作=缓存'E'
状态=读方法, 事件=收到'T'  → 状态=读方法, 动作=缓存'T'
状态=读方法, 事件=收到'\r' → 状态=方法结束\r, 动作=无
状态=方法结束\r, 事件=收到'\n' → 状态=读URL, 动作=提交方法="GET"
```

### 7.2 有限状态机（FSM）的数学定义

数学上，一个确定性有限状态机（DFA）是一个五元组：

```
M = (Q, Σ, δ, q0, F)
```

- `Q`：有限的状态集合（如 `{读方法, 读URL, 读版本, 读头部, 读body, 完成, 错误}`）
- `Σ`：有限的输入字母表（如所有可能的字节值 0-255）
- `δ`：转换函数 `δ: Q × Σ → Q`（在状态 q 收到输入 a，转到 δ(q, a)）
- `q0`：初始状态（如 `读方法`）
- `F`：接受状态集合（如 `{完成}`）

HTTP 解析器就是一个巨大的 DFA，输入是字节流，接受状态是"完整解析出一个请求"。

### 7.3 两种实现方式：switch-case vs 状态转换表

**方式一：switch-case 嵌套**

```c
/* 用 switch-case 实现的状态机 */
enum state {
    S_METHOD,
    S_URL,
    S_VERSION,
    S_HEADER,
    S_BODY,
    S_DONE
};

int parse_byte(enum state *st, char c) {
    switch (*st) {
    case S_METHOD:
        if (c == ' ') {
            *st = S_URL;  /* 方法读完，转去读URL */
        } else {
            /* 缓存到方法缓冲区 */
        }
        break;
    case S_URL:
        if (c == ' ') {
            *st = S_VERSION;
        }
        break;
    /* ... 其他状态 ... */
    }
    return 0;
}
```

优点：直观、调试方便、编译器能优化。
缺点：状态多时代码很长，加状态要改多处。

**方式二：状态转换表（转移矩阵）**

```c
/* 用二维表实现的状态机：table[当前状态][输入字符] = 下一个状态 */
enum state table[NUM_STATES][256];

/* 初始化转换表 */
void init_table(void) {
    /* 默认：在 S_METHOD 状态收到任何字符都留在 S_METHOD */
    for (int c = 0; c < 256; c++) {
        table[S_METHOD][c] = S_METHOD;
    }
    /* 遇到空格，转到 S_URL */
    table[S_METHOD][' '] = S_URL;
    /* ... 其他转换 ... */
}

int parse_byte(enum state *st, char c) {
    *st = table[*st][(unsigned char)c];
    /* 还要处理动作，通常用单独的动作表 */
    return 0;
}
```

优点：数据驱动，加状态只改表不改代码，适合自动生成。
缺点：不直观，调试难，动作处理需要额外的表。

我们的 HTTP 解析器用 switch-case，因为状态不算太多，可读性更重要。如果是正则引擎、词法分析器，状态成百上千，就该用表驱动了。

### 7.4 状态机在工程中的经典应用

状态机不是 HTTP 解析的专利，很多地方都在用：

**TCP 状态机**：TCP 连接有 11 个状态（LISTEN、SYN_SENT、ESTABLISHED、FIN_WAIT_1...），RFC 793 的状态转换图就是经典 FSM。

**词法分析器**：把源代码切成 token，每个 token 的识别都是一个小状态机。比如识别数字：先读整数部分，遇到 `.` 转去读小数部分，遇到 `e` 转去读指数部分。

**正则表达式引擎**：正则编译后就是一个 NFA/DFA，匹配过程就是在这个状态机上跑输入串。

**游戏 AI**：怪物的行为用状态机描述（巡逻→发现敌人→追击→攻击→死亡）。

**协议解析**：HTTP、TLS、WebSocket、JSON 解析器，全都是状态机。

### 7.5 为什么 HTTP 解析必须用状态机

关键原因：**TCP 是字节流，不是消息流**。

一次 `recv()` 收到的数据可能是：
- 半个请求头（粘包的另一半还没到）
- 一个完整请求
- 一个半请求（下一个请求的开头也来了）
- 三个请求拼在一起

如果用"一次性读完再解析"的方式，遇到数据不完整就只能干等。而状态机可以**记住当前解析到哪了**，下次新数据来了接着解析。这就是增量解析，下一节详讲。

---

## 8. 增量解析器的设计要点

### 8.1 为什么需要增量解析

TCP 是字节流协议，它不保证一次 `recv` 恰好收到一个完整的 HTTP 请求。实际场景：

```c
/* 第一次 recv：只收到半个头部 */
recv(fd, buf, 1024, 0);
/* buf = "GET / HTTP/1.1\r\nHost: exam" */

/* 第二次 recv：补齐了剩下的 */
recv(fd, buf, 1024, 0);
/* buf = "ple.com\r\n\r\n" */
```

如果解析器只能接受完整请求，第一次调用就得失败。但我们不想丢数据，所以需要**增量解析**：每次喂一部分数据，解析器记住进度，数据齐了再返回成功。

### 8.2 feed 接口设计

增量解析器的核心是一个 `feed` 函数，每次喂一部分数据：

```c
/* 解析器结构体：保存所有中间状态 */
typedef struct {
    /* 当前状态 */
    int state;
    /* 已解析并消费的偏移量 */
    size_t parsed_offset;
    /* 累积缓冲区：存还没解析完的数据 */
    char buf[8192];
    size_t buf_len;
    /* 解析结果 */
    char method[16];
    char url[1024];
    /* ... 其他字段 ... */
} http_parser_t;

/* 喂数据给解析器，返回解析状态 */
typedef enum {
    PARSE_NEED_MORE,  /* 数据不够，继续喂 */
    PARSE_OK,         /* 解析出一个完整请求 */
    PARSE_ERROR       /* 语法错误 */
} parse_result_t;

parse_result_t http_parser_feed(http_parser_t *p,
                                 const char *data,
                                 size_t len);
```

使用方式：

```c
http_parser_t parser;
http_parser_init(&parser);

while (1) {
    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;

    parse_result_t r = http_parser_feed(&parser, buf, n);
    if (r == PARSE_OK) {
        /* 一个完整请求解析好了，处理它 */
        handle_request(&parser);
        /* 处理完后重置，准备解析下一个请求 */
        http_parser_reset(&parser);
    } else if (r == PARSE_ERROR) {
        /* 出错，关闭连接 */
        break;
    }
    /* PARSE_NEED_MORE：继续 recv */
}
```

### 8.3 解析状态的保存

增量解析器必须在 `feed` 调用之间保存进度，关键状态有：

1. **当前处于哪个解析阶段**（`state` 字段）
2. **已累积但未解析完的原始数据**（`buf` 和 `buf_len`）
3. **正在构建的字段**（如方法字符串读到一半）
4. **已读的 body 字节数**（用来判断 body 是否读完）

```c
/* feed 的内部实现示意 */
parse_result_t http_parser_feed(http_parser_t *p,
                                 const char *data,
                                 size_t len) {
    /* 1. 把新数据追加到缓冲区 */
    if (p->buf_len + len > sizeof(p->buf)) {
        return PARSE_ERROR;  /* 缓冲区溢出 */
    }
    memcpy(p->buf + p->buf_len, data, len);
    p->buf_len += len;

    /* 2. 尽量解析，直到数据不够或解析完成 */
    while (p->parsed_offset < p->buf_len) {
        char c = p->buf[p->parsed_offset];
        switch (p->state) {
        case S_METHOD:
            if (c == ' ') {
                p->method[p->method_len] = '\0';
                p->state = S_URL;
            } else {
                p->method[p->method_len++] = c;
            }
            p->parsed_offset++;
            break;
        /* ... 其他状态 ... */
        case S_DONE:
            return PARSE_OK;
        }
    }

    /* 3. 数据用完了还没解析完，要更多数据 */
    return PARSE_NEED_MORE;
}
```

### 8.4 错误恢复

解析失败后怎么办？三种策略：

1. **直接关闭连接**（最简单）：HTTP/1.1 允许这样做，因为出错后后续数据的边界已经不可靠。
2. **重置解析器，跳过到下一个请求**： risky，因为不知道错误数据有多长。
3. **找 `\r\n\r\n` 边界，丢弃当前请求**：比较实用。

教学项目里用最简单的策略——出错就关连接：

```c
/* 解析出错，直接关闭 */
if (r == PARSE_ERROR) {
    close(fd);
    return;
}

/* 解析成功且是 keep-alive，重置解析器继续 */
http_parser_reset(&parser);
/* reset 只清状态，不清缓冲区里属于下一个请求的数据 */
```

`reset` 的关键是：**只重置状态，不清空缓冲区**。因为缓冲区里可能还有下一个请求的数据（粘包），那些数据要留给下一轮解析。

---

## 9. HTTP 请求方法详解

### 9.1 常见方法一览

| 方法    | 用途           | 有 body | 幂等 | 安全 |
|---------|----------------|---------|------|------|
| GET     | 获取资源       | 否      | 是   | 是   |
| POST    | 提交数据/创建  | 是      | 否   | 否   |
| PUT     | 替换资源       | 是      | 是   | 否   |
| DELETE  | 删除资源       | 否      | 是   | 否   |
| PATCH   | 部分修改       | 是      | 否   | 否   |
| HEAD    | 只取头部       | 否      | 是   | 是   |
| OPTIONS | 查询支持的方法 | 否      | 是   | 是   |

### 9.2 幂等性和安全性

- **安全（safe）**：不改变服务器状态。GET、HEAD、OPTIONS 是安全的——只读不改。
- **幂等（idempotent）**：调用一次和调用 N 次效果一样。GET、PUT、DELETE 是幂等的——删一次和删十次结果都是"没了"。

POST 不是幂等的：提交两次订单会创建两个订单。PATCH 一般也不幂等（虽然可以设计成幂等的）。

### 9.3 常见误区

1. **"GET 不能有 body"**：RFC 没禁止，但很多服务器/代理会忽略或拒绝。最好别这么做。
2. **"POST 一定有 body"**：不一定，POST 可以没有 body。
3. **"DELETE 是删数据"**：语义上是，但具体行为由服务器决定，也可以是"软删除"。
4. **"PUT 和 POST 的区别是 URL 不同"**：不完全。PUT 是"用这个内容替换那个资源"，POST 是"提交这些数据，你看着办"。

---

## 10. HTTP 状态码详解

### 10.1 五大类

| 范围  | 类别       | 含义                 |
|-------|------------|----------------------|
| 1xx   | 信息       | 请求已收到，继续处理 |
| 2xx   | 成功       | 请求成功             |
| 3xx   | 重定向     | 需要进一步动作       |
| 4xx   | 客户端错误 | 请求有问题           |
| 5xx   | 服务器错误 | 服务器出问题         |

### 10.2 常见状态码

- **200 OK**：一切正常
- **201 Created**：资源创建成功（POST/PUT 后返回）
- **204 No Content**：成功但没 body（DELETE 后常用）
- **301 Moved Permanently**：永久重定向，浏览器会缓存
- **302 Found**：临时重定向，不缓存
- **304 Not Modified**：资源没变，用缓存（配合 `If-Modified-Since`）
- **400 Bad Request**：请求语法错误
- **401 Unauthorized**：没认证（要登录）
- **403 Forbidden**：认证了但没权限
- **404 Not Found**：资源不存在
- **405 Method Not Allowed**：方法不支持（如对只读资源 PUT）
- **500 Internal Server Error**：服务器内部出错
- **502 Bad Gateway**：网关后面的服务器挂了
- **503 Service Unavailable**：服务不可用（过载、维护中）

### 10.3 301 vs 302 vs 307 vs 308

这几个重定向容易混：

- **301**：永久重定向，**允许把 POST 改成 GET**（历史遗留问题）
- **302**：临时重定向，**也允许把 POST 改成 GET**（同样历史问题）
- **307**：临时重定向，**保持原方法不变**（POST 还是 POST）
- **308**：永久重定向，**保持原方法不变**

简单记：307/308 是 302/301 的"严格版"，不允许偷偷改方法。新代码尽量用 307/308。

---

## 11. chunked 传输编码详解

### 11.1 为什么需要 chunked

正常响应靠 `Content-Length` 告诉客户端 body 多长。但有时候**发响应时还不知道总长度**：

- 动态生成的内容（如数据库查询结果边查边输出）
- 流式数据（如实时日志、视频转码）
- 内容太大，不想全部缓存到内存再发

这时就用 chunked 编码：把 body 分成一块一块发，每块带自己的长度，最后用 `0` 长度块结束。

### 11.2 chunked 格式

```
HTTP/1.1 200 OK
Transfer-Encoding: chunked

4\r\n        ← 第一块长度（十六进制，4 字节）
Wiki\r\n     ← 第一块数据
5\r\n        ← 第二块长度（5 字节）
pedia\r\n    ← 第二块数据
0\r\n        ← 结束块（长度为 0）
\r\n         ← 结束块的数据（空）
```

注意：
- 长度是**十六进制**的
- 每块格式：`长度\r\n` + `数据\r\n`
- 最后一块长度是 `0`，表示结束
- `Transfer-Encoding: chunked` 时**不能**有 `Content-Length`

### 11.3 解析 chunked 的状态机

解析 chunked body 也是状态机，状态比普通 body 多几层：

```c
/* chunked 解析状态 */
enum chunk_state {
    CHUNK_SIZE,       /* 正在读块大小行 */
    CHUNK_SIZE_LF,    /* 读完了块大小的 \r，等 \n */
    CHUNK_DATA,       /* 正在读块数据 */
    CHUNK_DATA_CR,    /* 读完了块数据，等 \r */
    CHUNK_DATA_LF,    /* 读完了 \r，等 \n */
    CHUNK_END_CR,     /* 读到 0 块后，等最后的 \r */
    CHUNK_END_LF,     /* 等最后的 \n */
    CHUNK_DONE        /* 全部结束 */
};

/* 解析 chunked body 的核心循环 */
parse_result_t parse_chunked(http_parser_t *p, char c) {
    switch (p->chunk_state) {
    case CHUNK_SIZE:
        if (c == '\r') {
            p->chunk_size[p->chunk_size_len] = '\0';
            p->current_chunk_remaining =
                strtol(p->chunk_size, NULL, 16);  /* 十六进制转数字 */
            p->chunk_state = CHUNK_SIZE_LF;
        } else {
            p->chunk_size[p->chunk_size_len++] = c;
        }
        break;
    case CHUNK_SIZE_LF:
        if (c != '\n') return PARSE_ERROR;
        if (p->current_chunk_remaining == 0) {
            p->chunk_state = CHUNK_END_CR;  /* 最后一块 */
        } else {
            p->chunk_state = CHUNK_DATA;
        }
        break;
    case CHUNK_DATA:
        /* 把字节交给上层，并递减剩余计数 */
        p->current_chunk_remaining--;
        if (p->current_chunk_remaining == 0) {
            p->chunk_state = CHUNK_DATA_CR;
        }
        break;
    case CHUNK_DATA_CR:
        if (c != '\r') return PARSE_ERROR;
        p->chunk_state = CHUNK_DATA_LF;
        break;
    case CHUNK_DATA_LF:
        if (c != '\n') return PARSE_ERROR;
        p->chunk_state = CHUNK_SIZE;  /* 回去读下一块 */
        p->chunk_size_len = 0;
        break;
    case CHUNK_END_CR:
        if (c != '\r') return PARSE_ERROR;
        p->chunk_state = CHUNK_END_LF;
        break;
    case CHUNK_END_LF:
        if (c != '\n') return PARSE_ERROR;
        p->chunk_state = CHUNK_DONE;
        return PARSE_OK;
    }
    return PARSE_NEED_MORE;
}
```

### 11.4 生成 chunked 响应

服务器端生成 chunked 响应也很简单：

```c
/* 发送一个 chunk */
void send_chunk(int fd, const char *data, size_t len) {
    char header[32];
    /* 长度行：十六进制长度 + \r\n */
    int hlen = snprintf(header, sizeof(header),
                        "%zx\r\n", len);  /* %zx：十六进制 size_t */
    send(fd, header, hlen, 0);
    /* 数据 */
    send(fd, data, len, 0);
    /* 结尾 \r\n */
    send(fd, "\r\n", 2, 0);
}

/* 发送 chunked 响应示例 */
void serve_streaming(int fd) {
    /* 响应头：声明 chunked */
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    send(fd, hdr, strlen(hdr), 0);

    /* 边生成边发 */
    char buf[1024];
    while (1) {
        size_t n = generate_next_chunk(buf, sizeof(buf));
        if (n == 0) break;  /* 没数据了 */
        send_chunk(fd, buf, n);
    }

    /* 结束块：0\r\n\r\n */
    send(fd, "0\r\n\r\n", 5, 0);
}
```

### 11.5 chunked 的实际应用

- **动态页面**：PHP/Python 脚本边执行边输出，不用先算总长度
- **服务器推送**：Server-Sent Events 用 chunked 保持连接不断
- **大文件转码**：视频转码时边转边发，客户端可以边播边下
- **流式 JSON**：`{"stream": [1,2,3,...]}` 数组元素一个个发

chunked 是 HTTP/1.1 的重要特性，理解了它就能做很多 Content-Length 做不了的事。HTTP/2 里虽然换成了 DATA 帧，但"分块发送"的思想完全一样。

---

## 12. 小结

这一章我们从 HTTP/1.1 的状态机解析，一路讲到了协议演进、状态机原理、增量解析、方法和状态码、chunked 编码。核心要点：

1. **HTTP 解析必须用状态机**，因为 TCP 是字节流，数据可能分多次到达、可能粘包。
2. **状态机 = 状态 + 事件 + 转换**，可以用 switch-case 或转换表实现。
3. **增量解析器**靠 `feed` 接口每次喂数据，在调用间保存进度。
4. **chunked 编码**解决"不知道总长度"的问题，解析它需要额外的状态层。
5. **HTTP 协议演进**的每一步都在解决上一版的痛点，理解 1.1 是理解 2/3 的基础。

把这些搞懂，你写的 HTTP 解析器就不只是"能跑"，而是"知道为什么这么写"。下一章我们会进入事件驱动 I/O，看看怎么同时处理多个连接。

---
