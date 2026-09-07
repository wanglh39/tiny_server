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

