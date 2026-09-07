# stage14 - HTTP/2 协议与实现

## 本章导读

HTTP/1.1 有三个核心问题：
1. **文本协议**：解析慢，格式冗余
2. **串行请求**：一个请求等一个响应，队头阻塞（Head-of-Line Blocking）
3. **头部冗余**：每次请求都带完整头部，大量重复

HTTP/2（RFC 7540，2015 年）解决这三个问题：
1. **二进制帧**：解析快，格式紧凑
2. **多路复用**：一条 TCP 上并行多个请求/响应（Stream）
3. **头部压缩**：HPACK 算法压缩重复头部

```bash
cmake --build build --target http2_server
build/bin/http2_server 8080
curl --http2-prior-knowledge http://localhost:8080/
```

---

## 一、HTTP/1.1 的痛点

### 1.1 队头阻塞（Head-of-Line Blocking）

HTTP/1.1 在一条 TCP 连接上只能串行处理请求：

```
客户端 ──请求1──→ 服务器
客户端 ←──响应1── 服务器  (2秒)
客户端 ──请求2──→ 服务器
客户端 ←──响应2── 服务器  (0.1秒)
```

请求2 很快就能处理完，但必须等请求1 的响应。
这就是**队头阻塞**：前面的慢请求阻塞后面的快请求。

**HTTP/1.1 的缓解方案**：
- **管道化（Pipelining）**：客户端连续发多个请求，但服务器必须按序响应
  → 实际很少用，因为还是有队头阻塞
- **多连接**：浏览器开 6 个 TCP 连接并行请求
  → 浪费资源，每个连接要 TCP 握手 + TLS 握手

### 1.2 头部冗余

HTTP/1.1 每次请求都带完整头部：

```http
GET /page1 HTTP/1.1
Host: example.com
User-Agent: Mozilla/5.0 ...
Accept: text/html,...
Accept-Language: en-US,en;q=0.9
Cookie: session=abc123; user=john; ...
```

同一个连接上的多个请求，大部分头部是**重复的**（User-Agent、Accept、Cookie 等）。
这些重复头部浪费带宽，尤其对移动端。

### 1.3 文本协议

HTTP/1.1 是文本协议，解析需要：
- 逐字符扫描 `\r\n` 分隔行
- 逐行匹配头部名
- 处理大小写不敏感
- 处理空格、冒号等分隔符

```http
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 1234\r\n
\r\n
```

文本协议容易出错，解析效率低。

---

## 二、HTTP/2 核心特性

### 2.1 二进制帧

HTTP/2 把所有数据分成**帧（Frame）**，帧是二进制格式：

```
┌─────────────────┬─────────┬─────────┬───────────────────┐
│ Length (24 bit)  │ Type(8) │ Flags(8)│ Stream ID (31 bit)│
│                  │         │         │ + Reserved (1 bit)│
└─────────────────┴─────────┴─────────┴───────────────────┘
│                    Payload (Length bytes)                │
└──────────────────────────────────────────────────────────┘
```

- **Length**：payload 长度（24 位，最大 16MB）
- **Type**：帧类型（8 位，10 种类型）
- **Flags**：标志位（8 位，不同帧类型有不同含义）
- **Stream ID**：流 ID（31 位，标识属于哪个请求/响应）

二进制解析比文本解析快得多：直接读固定长度的字段，不需要扫描分隔符。

### 2.2 多路复用

HTTP/2 在一条 TCP 连接上**并行**多个流（Stream）：

```
客户端 ──请求1(stream=1)──→
客户端 ──请求2(stream=3)──→    服务器同时处理
客户端 ──请求3(stream=5)──→

客户端 ←──响应2(stream=3)──    响应2 先完成，先返回
客户端 ←──响应1(stream=1)──    响应1 后完成，后返回
客户端 ←──响应3(stream=5)──
```

- 每个流有唯一 ID（奇数=客户端发起，偶数=服务器推送）
- 流之间互不阻塞，响应可以乱序
- 一条 TCP 连接处理所有流，不需要多连接

### 2.3 头部压缩（HPACK）

HPACK（RFC 7541）用**索引表**压缩重复头部：

```
第一次请求:
  :method: GET     → 索引 2（静态表）
  :path: /index    → 字面量（加入动态表）
  user-agent: ...  → 字面量（加入动态表）

第二次请求:
  :method: GET     → 索引 2（静态表）
  :path: /index    → 索引 62（动态表，第一次加的）
  user-agent: ...  → 索引 63（动态表，第一次加的）
```

第二次请求的头部从几百字节压缩到几个字节。

### 2.4 服务器推送

服务器可以主动推送资源：

```
客户端 ──请求 /index.html──→ 服务器
客户端 ←──响应 /index.html── 服务器
客户端 ←──推送 /style.css──── 服务器  (服务器主动推)
客户端 ←──推送 /script.js──── 服务器  (服务器主动推)
```

减少客户端的往返请求。

---

## 三、HTTP/2 帧格式

### 3.1 帧头（9 字节）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 Length (24)                     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Type (8)   |   Flags (8)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|R|                 Stream ID (31)              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

代码实现：

```c
typedef struct {
    uint32_t length;    /* payload 长度（24 位） */
    uint8_t  type;      /* 帧类型 */
    uint8_t  flags;     /* 标志位 */
    uint32_t stream_id; /* 流 ID（31 位） */
} h2_frame_hdr_t;

static int parse_frame_hdr(const uint8_t *buf, h2_frame_hdr_t *hdr)
{
    hdr->length = ((uint32_t)buf[0] << 16) |
                  ((uint32_t)buf[1] << 8)  |
                  (uint32_t)buf[2];
    hdr->type      = buf[3];
    hdr->flags     = buf[4];
    hdr->stream_id = ((uint32_t)(buf[5] & 0x7F) << 24) |
                     ((uint32_t)buf[6] << 16) |
                     ((uint32_t)buf[7] << 8)  |
                     (uint32_t)buf[8];
    return 0;
}
```

### 3.2 帧类型

| 类型 | 值 | 说明 |
|------|----|------|
| DATA | 0x0 | 请求/响应体数据 |
| HEADERS | 0x1 | 请求/响应头部 |
| PRIORITY | 0x2 | 流优先级 |
| RST_STREAM | 0x3 | 终止流 |
| SETTINGS | 0x4 | 连接参数 |
| PUSH_PROMISE | 0x5 | 服务器推送承诺 |
| PING | 0x6 | 心跳检测 |
| GOAWAY | 0x7 | 关闭连接 |
| WINDOW_UPDATE | 0x8 | 流控窗口更新 |
| CONTINUATION | 0x9 | HEADERS 帧续接 |

### 3.3 帧标志

| 标志 | 值 | 适用帧 | 含义 |
|------|----|--------|------|
| END_STREAM | 0x1 | DATA, HEADERS | 流的最后一帧 |
| ACK | 0x1 | SETTINGS, PING | 确认帧 |
| END_HEADERS | 0x4 | HEADERS, PUSH_PROMISE | 头部完整 |
| PADDED | 0x8 | DATA, HEADERS | 有填充 |
| PRIORITY | 0x20 | HEADERS | 含优先级信息 |

### 3.4 Stream ID

- **0**：连接级帧（SETTINGS、PING、GOAWAY、WINDOW_UPDATE）
- **奇数**：客户端发起的流
- **偶数**：服务器推送的流
- **递增**：同一端发起的流 ID 严格递增

---

## 四、连接前言（Connection Preface）

HTTP/2 连接以一个固定的"魔法字符串"开始：

```
PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n
```

这个 24 字节的字符串是 HTTP/2 的"暗号"：
- 客户端发这个前缀告诉服务器"我要用 HTTP/2"
- 服务器看到这个前缀就知道后续是二进制帧

```c
static const char H2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/* 验证连接前言 */
if (memcmp(buf, H2_PREFACE, H2_PREFACE_LEN) != 0) {
    log_error("连接前言错误");
    return;
}
```

前言之后，客户端发送 SETTINGS 帧，服务器也发送 SETTINGS 帧，
双方协商连接参数。

---

## 五、SETTINGS 帧

SETTINGS 帧用于协商连接参数：

| 参数 | ID | 默认值 | 说明 |
|------|----|--------|------|
| HEADER_TABLE_SIZE | 1 | 4096 | HPACK 动态表大小 |
| ENABLE_PUSH | 2 | 1 | 允许服务器推送 |
| MAX_CONCURRENT_STREAMS | 3 | ∞ | 最大并发流数 |
| INITIAL_WINDOW_SIZE | 4 | 65535 | 初始流控窗口 |
| MAX_FRAME_SIZE | 5 | 16384 | 最大帧大小 |
| MAX_HEADER_LIST_SIZE | 6 | ∞ | 最大头部列表大小 |

握手流程：

```
客户端 ──前言 + SETTINGS──→ 服务器
客户端 ←──SETTINGS────────── 服务器
客户端 ──SETTINGS ACK──→     服务器  (确认服务器的 SETTINGS)
客户端 ←──SETTINGS ACK────── 服务器  (确认客户端的 SETTINGS)
```

```c
/* 服务器收到 SETTINGS 后回复 ACK */
static void handle_settings(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                            const uint8_t *payload)
{
    if (hdr->flags & FLAG_ACK) {
        /* 这是 ACK，不需要回复 */
        return;
    }
    /* 回复 SETTINGS ACK */
    send_settings_ack(conn->fd);
}
```

---

## 六、HEADERS 帧与 HPACK

### 6.1 HEADERS 帧

HEADERS 帧携带 HTTP 请求/响应的头部，payload 是 HPACK 编码的头部列表。

```
HEADERS 帧:
  ┌─────────┬─────────┬──────────┬───────────┐
  │ 帧头(9)  │ Pad Len?│ Priority?│ HPACK 数据 │
  └─────────┴─────────┴──────────┴───────────┘
```

### 6.2 HPACK 编码

HPACK 有三种编码方式：

**1. 完全索引（Indexed Header Field）**

```
  1   1xxxxxxx
  ├───┼───────┤
  │ 1 │ Index  │
  └───┴───────┘
```

直接用索引表中的条目。索引 2 = `:method: GET`。

```c
/* 客户端发送 */
*p++ = 0x82;  /* 10000010 = 索引 2 = :method: GET */

/* 服务器解析 */
if ((first & 0x80) == 0x80) {
    uint32_t idx = first & 0x7F;
    /* 查静态表 */
    name  = hpack_static_table[idx].name;
    value = hpack_static_table[idx].value;
}
```

**2. 字面头部，不索引（Literal Header Field without Indexing）**

```
  0   1   0   x x x x x x
  ├───┼───┼───────────────┤
  │ 0 │ 1 │  Name Index  │     (索引 0 = 字面 name)
  └───┴───┴───────────────┘
  │ H │     Name Length     │   (如果 Name Index = 0)
  │     Name String         │
  │ H │    Value Length     │
  │     Value String        │
```

```c
/* 编码 */
*p++ = 0x40;  /* 01000000: 字面 name + 字面 value, 不索引 */
p = hpack_encode_str(p, "content-type");
p = hpack_encode_str(p, "text/html");
```

**3. 字面头部，加入索引（Literal Header Field with Incremental Indexing）**

```
  0   1   x x x x x x x
  ├───┼───────────────┤
  │ 0 │  Name Index  │
  └───┴───────────────┘
```

和"不索引"类似，但会把头部加入动态表，后续可以引用。

### 6.3 HPACK 静态表

RFC 7541 定义的 61 个静态表条目：

| 索引 | 名称 | 值 |
|------|------|-----|
| 1 | :authority | (空) |
| 2 | :method | GET |
| 3 | :method | POST |
| 4 | :path | / |
| 5 | :path | /index.html |
| 6 | :scheme | http |
| 7 | :scheme | https |
| 8 | :status | 200 |
| 9 | :status | 204 |
| ... | ... | ... |
| 37 | host | (空) |
| 57 | user-agent | (空) |

curl 发送 `GET /` 请求时，头部编码为：
```
0x82  → 索引 2 = :method: GET
0x86  → 索引 6 = :scheme: http
0x84  → 索引 4 = :path: /
```

只需 3 字节就编码了 3 个头部！

### 6.4 HPACK 整数编码

HPACK 用变长整数编码：

```
值 < 2^N - 1:  直接放在 N 位前缀
值 >= 2^N - 1: 前缀全 1，后续用 7 位变长编码
```

```c
static uint8_t *hpack_encode_int(uint8_t *buf, uint32_t value,
                                 uint8_t prefix_bits, uint8_t prefix)
{
    uint32_t max = (1 << prefix_bits) - 1;

    if (value < max) {
        *buf++ = prefix | value;
    } else {
        *buf++ = prefix | max;
        value -= max;
        while (value >= 128) {
            *buf++ = (value & 0x7F) | 0x80;
            value >>= 7;
        }
        *buf++ = value;
    }
    return buf;
}
```

### 6.5 Huffman 编码

HPACK 可以用 Huffman 编码压缩字符串值：
- 常见字符用短编码（如 'e' = 3 位）
- 罕见字符用长编码（如 '~' = 28 位）

```c
/* 解码时检查 Huffman 标志 */
uint8_t huffman = (*buf >> 7) & 0x1;
if (huffman) {
    /* 教学简化：不解码 Huffman */
    snprintf(str, max_len, "[huffman:%u]", len);
    return buf + len;
}
```

本实现不解码 Huffman（教学简化），但能正确跳过 Huffman 编码的数据。

---

## 七、DATA 帧与流

### 7.1 DATA 帧

DATA 帧携带请求/响应体：

```
DATA 帧:
  ┌─────────┬─────────┬───────────┐
  │ 帧头(9)  │ Pad Len?│  数据      │
  └─────────┴─────────┴───────────┘
```

标志 END_STREAM 表示这是流的最后一帧。

### 7.2 流的生命周期

```
IDLE ──收到 HEADERS──→ OPEN
                          │
              收到 END_STREAM │
                          ▼
                    HALF_CLOSED
                          │
              收到 END_STREAM │
                          ▼
                       CLOSED
```

### 7.3 多路复用示例

```
客户端发送:
  HEADERS (stream=1, END_STREAM)  → GET /page1
  HEADERS (stream=3, END_STREAM)  → GET /page2
  HEADERS (stream=5, END_STREAM)  → GET /page3

服务器响应（可以乱序）:
  HEADERS (stream=3, END_HEADERS) → 200 OK
  DATA    (stream=3, END_STREAM)  → page2 内容
  HEADERS (stream=1, END_HEADERS) → 200 OK
  DATA    (stream=1, END_STREAM)  → page1 内容
  HEADERS (stream=5, END_HEADERS) → 200 OK
  DATA    (stream=5, END_STREAM)  → page3 内容
```

---

## 八、流控制（Flow Control）

HTTP/2 有两层流控：

### 8.1 连接级流控

限制整个连接上所有流的数据量。

### 8.2 流级流控

限制单个流的数据量。

```c
static void handle_window_update(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                                 const uint8_t *payload)
{
    log_debug("收到 WINDOW_UPDATE fd=%d stream=%d",
              conn->fd, hdr->stream_id);
}
```

WINDOW_UPDATE 帧增加流控窗口，允许对端发送更多数据。

---

## 九、PING 与 GOAWAY

### 9.1 PING（心跳）

```
客户端 ──PING(payload)──→ 服务器
客户端 ←──PING(ACK)────── 服务器
```

```c
static void handle_ping(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                        const uint8_t *payload)
{
    if (!(hdr->flags & FLAG_ACK)) {
        /* 回复 PING ACK，payload 原样返回 */
        h2_send_frame(conn->fd, HTTP2_PING, FLAG_ACK, 0,
                      payload, hdr->length);
    }
}
```

### 9.2 GOAWAY（优雅关闭）

```
客户端 ──GOAWAY(last_stream_id)──→ 服务器
```

告诉服务器：不要再发起 ID > last_stream_id 的流。

---

## 十、服务器实现

### 10.1 主循环

```c
for (;;) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; i++) {
        if (events[i].data.fd == listen_fd) {
            /* 新连接 */
            int fd = accept(listen_fd, NULL, NULL);
            h2_conn_t *conn = calloc(1, sizeof(h2_conn_t));
            conn->fd = fd;
        } else {
            /* 数据到达 */
            handle_read(epfd, g_conns[fd]);
        }
    }
}
```

### 10.2 帧处理流程

```c
static void handle_read(int epfd, h2_conn_t *conn)
{
    int n = read(conn->fd, buf, sizeof(buf));

    /* 1. 检查连接前言 */
    if (!conn->preface_done) {
        if (memcmp(buf, H2_PREFACE, 24) != 0) return;
        conn->preface_done = 1;
        send_settings(conn->fd);  /* 发送服务器 SETTINGS */
    }

    /* 2. 逐帧解析 */
    while (offset + 9 <= n) {
        parse_frame_hdr(buf + offset, &hdr);
        handle_frame(conn, &hdr, buf + offset + 9);
        offset += 9 + hdr.length;
    }
}
```

### 10.3 帧分发

```c
static void handle_frame(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                         const uint8_t *payload)
{
    switch (hdr->type) {
    case HTTP2_SETTINGS:      handle_settings(conn, hdr, payload); break;
    case HTTP2_HEADERS:       handle_headers(conn, hdr, payload); break;
    case HTTP2_DATA:          handle_data(conn, hdr, payload); break;
    case HTTP2_PING:          handle_ping(conn, hdr, payload); break;
    case HTTP2_GOAWAY:        handle_goaway(conn, hdr, payload); break;
    case HTTP2_WINDOW_UPDATE: handle_window_update(conn, hdr, payload); break;
    }
}
```

### 10.4 请求处理

```c
static void handle_headers(h2_conn_t *conn, h2_frame_hdr_t *hdr,
                           const uint8_t *payload)
{
    h2_stream_t *stream = find_or_create_stream(conn, hdr->stream_id);

    /* HPACK 解码头部 */
    parse_headers(payload, hdr->length, stream);

    log_info("请求: %s %s (stream=%d)",
             stream->method, stream->path, hdr->stream_id);

    /* 如果 END_STREAM，发送响应 */
    if (hdr->flags & FLAG_END_STREAM) {
        send_response(conn->fd, hdr->stream_id, 200,
                      "text/html", body);
    }
}
```

### 10.5 响应生成

```c
static int send_response(int fd, uint32_t stream_id,
                         int status, const char *content_type,
                         const char *body)
{
    /* 1. 发送 HEADERS 帧 */
    send_response_headers(fd, stream_id, status, content_type);

    /* 2. 发送 DATA 帧（END_STREAM） */
    send_response_data(fd, stream_id, body, strlen(body));

    return 0;
}
```

---

## 十一、HTTP/2 vs HTTP/1.1 对比

| 特性         | HTTP/1.1          | HTTP/2            |
| ------------ | ----------------- | ----------------- |
| 协议格式     | 文本              | 二进制帧          |
| 多路复用     | 不支持（串行）    | 支持（Stream）    |
| 队头阻塞     | 有                | 无（流级别）      |
| 头部压缩     | 不支持            | HPACK             |
| 服务器推送   | 不支持            | 支持              |
| 流控         | TCP 流控          | HTTP/2 流控 + TCP |
| 连接数       | 6 个/域名         | 1 个/域名         |
| 优先级       | 不支持            | 支持              |

---

## 十二、测试验证

### 12.1 编译运行

```bash
cmake --build build --target http2_server
build/bin/http2_server 8080
```

### 12.2 curl 测试

```bash
curl --http2-prior-knowledge -v http://localhost:8080/
```

输出：
```
> GET / HTTP/2
> Host: localhost:8080
> User-Agent: curl/8.5.0
> Accept: */*

< HTTP/2 200
< content-type: text/html; charset=utf-8
< server: tiny_http2/0.1

<html><body><h1>HTTP/2 Server</h1>
<p>Method: GET</p><p>Path: /</p>
<p>Stream ID: 1</p></body></html>
```

### 12.3 多请求测试

```bash
curl --http2-prior-knowledge http://localhost:8080/test
curl --http2-prior-knowledge http://localhost:8080/api
```

---

## 十三、HTTP/2 的局限

### 13.1 TCP 级队头阻塞

HTTP/2 解决了 HTTP 级队头阻塞，但 TCP 级队头阻塞仍在：
如果一个 TCP 包丢失，所有流都被阻塞直到重传成功。

这就是 HTTP/3 用 QUIC（UDP）的原因。

### 13.2 实现复杂

HTTP/2 比 HTTP/1.1 复杂得多：
- 二进制帧解析
- HPACK 压缩
- 流控
- 流优先级
- 服务器推送

### 13.3 Huffman 编码

本实现不支持 Huffman 解码（教学简化）。
生产实现需要完整的 Huffman 解码器。

---

## 十四、从 HTTP/2 到 HTTP/3

HTTP/3（RFC 9114，2022 年）用 QUIC 替代 TCP：

| 特性         | HTTP/2     | HTTP/3     |
| ------------ | ---------- | ---------- |
| 传输层       | TCP        | QUIC (UDP) |
| 队头阻塞     | TCP 级有   | 完全消除   |
| 连接建立     | TCP+TLS    | 1-RTT/0-RTT|
| 连接迁移     | 不支持     | 支持       |
| 多路复用     | Stream     | Stream     |
| 头部压缩     | HPACK      | QPACK      |

---

## 十五、代码结构

```
stage14_http2/
├── http2_server.c      # HTTP/2 服务器
├── test_http2.sh       # 测试脚本
└── CMakeLists.txt      # 构建配置
```

---

## 十六、完整代码

```c
/* http2_server.c 核心部分 */

/* 帧头解析 */
static int parse_frame_hdr(const uint8_t *buf, h2_frame_hdr_t *hdr)
{
    hdr->length = ((uint32_t)buf[0] << 16) |
                  ((uint32_t)buf[1] << 8)  |
                  (uint32_t)buf[2];
    hdr->type      = buf[3];
    hdr->flags     = buf[4];
    hdr->stream_id = ((uint32_t)(buf[5] & 0x7F) << 24) |
                     ((uint32_t)buf[6] << 16) |
                     ((uint32_t)buf[7] << 8)  |
                     (uint32_t)buf[8];
    return 0;
}

/* 帧发送 */
static int h2_send_frame(int fd, uint8_t type, uint8_t flags,
                         uint32_t stream_id,
                         const uint8_t *payload, uint32_t payload_len)
{
    uint8_t frame[BUF_SIZE];
    build_frame_hdr(frame, payload_len, type, flags, stream_id);
    if (payload_len > 0)
        memcpy(frame + 9, payload, payload_len);
    write(fd, frame, 9 + payload_len);
    return 0;
}

/* HPACK 编码头部 */
static uint8_t *hpack_encode_header(uint8_t *buf, const char *name,
                                    const char *value)
{
    *buf++ = 0x40;  /* 字面头部，不索引 */
    buf = hpack_encode_str(buf, name);
    buf = hpack_encode_str(buf, value);
    return buf;
}

/* 连接处理 */
static void handle_read(int epfd, h2_conn_t *conn)
{
    int n = read(conn->fd, buf, sizeof(buf));

    /* 验证连接前言 */
    if (!conn->preface_done) {
        if (memcmp(buf, H2_PREFACE, 24) != 0) return;
        conn->preface_done = 1;
        send_settings(conn->fd);
    }

    /* 逐帧处理 */
    while (offset + 9 <= n) {
        parse_frame_hdr(buf + offset, &hdr);
        handle_frame(conn, &hdr, buf + offset + 9);
        offset += 9 + hdr.length;
    }
}
```

---

## 十七、总结

### 17.1 本阶段实现

- HTTP/2 连接前言验证
- 二进制帧解析与生成（9 字节帧头 + payload）
- SETTINGS 帧握手与 ACK
- HEADERS 帧与 HPACK 解码（静态表 + 字面头部）
- DATA 帧处理
- PING/PONG 心跳
- GOAWAY 连接关闭
- WINDOW_UPDATE 流控
- Stream 多路复用
- HTTP/2 响应生成

### 17.2 phase2 全阶段回顾

| 阶段 | 主题 | 关键技术 |
|------|------|----------|
| stage8 | TLS/HTTPS | OpenSSL, TLS 1.3 |
| stage9 | io_uring | 异步 IO, SQ/CQ |
| stage10 | 内存池+日志 | Nginx 风格池, 双缓冲日志 |
| stage11 | SO_REUSEPORT | 多核负载均衡 |
| stage12 | WebSocket | 全双工, 帧协议 |
| stage13 | 协程 | ucontext, 同步写异步 |
| stage14 | HTTP/2 | 二进制帧, 多路复用, HPACK |

---

## 附录：RFC 参考

| RFC | 内容 |
|-----|------|
| 7540 | HTTP/2 协议 |
| 7541 | HPACK 头部压缩 |
| 9113 | HTTP/2 (2022 修订) |
| 9114 | HTTP/3 |
| 9000 | QUIC |

---

## 附录 A：HTTP/2 帧实战分析

### A.1 连接前言

```
客户端发送（24 字节）:
50 52 49 20 2A 20 48 54 54 50 2F 32 2E 30 0D 0A 0D 0A 53 4D 0D 0A 0D 0A
P  R  I  _  *  _  H  T  T  P  /  2  .  0  \r \n \r \n S  M  \r \n \r \n
```

### A.2 SETTINGS 帧

```
客户端发送:
00 00 12 04 00 00 00 00 00  ← 帧头: length=18, type=4(SETTINGS), flags=0, stream=0
00 01 00 00 10 00           ← 设置1: HEADER_TABLE_SIZE=4096
00 03 00 00 64 00           ← 设置3: MAX_CONCURRENT_STREAMS=100
00 05 00 00 40 00           ← 设置5: MAX_FRAME_SIZE=16384

服务器回复 ACK:
00 00 00 04 01 00 00 00 00  ← length=0, type=4, flags=1(ACK), stream=0
```

### A.3 HEADERS 帧（GET /）

```
客户端发送:
00 00 0B 01 05 00 00 00 01  ← 帧头: length=11, type=1(HEADERS), flags=5(END_STREAM|END_HEADERS), stream=1
82 86 41 8C 76 9E 0F 2C 3E 44 1F  ← HPACK payload

HPACK 解码:
82 → 索引 2 = :method: GET
86 → 索引 6 = :scheme: http
41 8C ... → 字面头部: :authority: localhost:8080 (Huffman 编码)
84 → 索引 4 = :path: /
```

### A.4 响应 HEADERS 帧

```
服务器发送:
00 00 2A 01 04 00 00 00 01  ← 帧头: length=42, type=1(HEADERS), flags=4(END_HEADERS), stream=1
88                          ← 索引 8 = :status: 200
40 0C 63 6F 6E 74 65 6E 74 2D 74 79 70 65  ← 字面: content-type
09 74 65 78 74 2F 68 74 6D 6C              ← text/html
40 06 73 65 72 76 65 72                    ← 字面: server
0D 74 69 6E 79 5F 68 74 74 70 32 2F 30 2E 31  ← tiny_http2/0.1
```

### A.5 DATA 帧

```
服务器发送:
00 00 7C 00 01 00 00 00 01  ← 帧头: length=124, type=0(DATA), flags=1(END_STREAM), stream=1
3C 68 74 6D 6C 3E 3C 62 6F 64 79 3E ...  ← "<html><body>..."
```

---

## 附录 B：HPACK 静态表完整列表

| 索引 | 名称 | 值 |
|------|------|-----|
| 1 | :authority | |
| 2 | :method | GET |
| 3 | :method | POST |
| 4 | :path | / |
| 5 | :path | /index.html |
| 6 | :scheme | http |
| 7 | :scheme | https |
| 8 | :status | 200 |
| 9 | :status | 204 |
| 10 | :status | 206 |
| 11 | :status | 304 |
| 12 | :status | 400 |
| 13 | :status | 404 |
| 14 | accept-charset | |
| 15 | accept-encoding | gzip, deflate |
| 16 | accept-language | |
| 17 | accept-ranges | |
| 18 | accept | |
| 19 | access-control-allow-origin | |
| 20 | age | |
| 21 | allow | |
| 22 | authorization | |
| 23 | cache-control | |
| 24 | content-disposition | |
| 25 | content-encoding | |
| 26 | content-language | |
| 27 | content-length | |
| 28 | content-location | |
| 29 | content-range | |
| 30 | content-type | |
| 31 | cookie | |
| 32 | date | |
| 33 | etag | |
| 34 | expect | |
| 35 | expires | |
| 36 | from | |
| 37 | host | |
| 38 | if-match | |
| 39 | if-modified-since | |
| 40 | if-none-match | |
| 41 | if-range | |
| 42 | if-unmodified-since | |
| 43 | last-modified | |
| 44 | link | |
| 45 | location | |
| 46 | max-forwards | |
| 47 | proxy-authenticate | |
| 48 | proxy-authorization | |
| 49 | range | |
| 50 | referer | |
| 51 | refresh | |
| 52 | retry-after | |
| 53 | server | |
| 54 | set-cookie | |
| 55 | strict-transport-security | |
| 56 | transfer-encoding | |
| 57 | user-agent | |
| 58 | vary | |
| 59 | via | |
| 60 | www-authenticate | |

---

## 附录 C：HTTP/2 流状态机

```
                    ┌────────┐
                    │  IDLE  │
                    └───┬────┘
                        │ 收到 HEADERS
                        ▼
                    ┌────────┐
        ┌──────────→│  OPEN  │←──────────┐
        │           └───┬────┘            │
        │   END_STREAM  │                 │
        │               ▼                 │
        │      ┌───────────────┐          │
        │      │ HALF_CLOSED   │          │
        │      │ (remote)      │          │
        │      └───────┬───────┘          │
        │         END_STREAM │            │
        │                    ▼            │
        │           ┌────────────┐        │
        │           │   CLOSED   │        │
        │           └────────────┘        │
        │                                 │
        │   RST_STREAM                    │
        └─────────────────────────────────┘
```

流状态转换：
- IDLE → OPEN：收到/发送 HEADERS
- OPEN → HALF_CLOSED：收到/发送 END_STREAM
- HALF_CLOSED → CLOSED：收到/发送 END_STREAM
- 任意 → CLOSED：收到/发送 RST_STREAM

---

## 附录 D：HTTP/2 vs HTTP/1.1 性能对比

### D.1 连接开销

```
HTTP/1.1 (6 连接):
  6 × (TCP 握手 + TLS 握手) = 6 × 3-RTT = 18-RTT

HTTP/2 (1 连接):
  1 × (TCP 握手 + TLS 握手) = 1 × 3-RTT = 3-RTT
  节省 83%
```

### D.2 头部开销

```
HTTP/1.1 请求头（每次请求）:
  GET / HTTP/1.1\r\n           (16 字节)
  Host: example.com\r\n        (21 字节)
  User-Agent: Mozilla/5.0...\r\n (100+ 字节)
  Accept: */*\r\n              (12 字节)
  总计: ~150-300 字节

HTTP/2 请求头（HPACK 编码，后续请求）:
  82 84 86 ...                (3-10 字节)
  总计: ~10-50 字节
  节省 80-95%
```

### D.3 队头阻塞

```
HTTP/1.1:
  请求1 (慢, 2s) ──→ 响应1
  请求2 (快, 0.1s) ──→ 等待 2s ──→ 响应2
  总时间: 2.1s

HTTP/2:
  请求1 (慢, 2s) ──→ 响应1
  请求2 (快, 0.1s) ──→ 响应2 (立即开始)
  总时间: 2.0s (请求2 不被阻塞)
```
