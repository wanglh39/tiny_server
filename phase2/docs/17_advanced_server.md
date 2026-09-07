# stage15 - 聚合版高级服务器

## 本章导读

前面 7 个 stage 分别实现了 TLS、io_uring、内存池、异步日志、SO_REUSEPORT、WebSocket、HTTP/2。
每个 stage 都是独立的，只关注一个特性。**真实的服务器需要把这些特性全部整合在一起。**

本章把 7 个进阶特性整合到一个服务器中，形成**生产级 Web 服务器的雏形**：

```
advanced_server
├── SO_REUSEPORT 多线程     (stage13)
├── epoll 事件循环          (phase1)
├── 协议自动检测            (TLS / WebSocket / HTTP/2 / HTTP/1.1)
├── TLS/HTTPS              (stage8)
├── WebSocket              (stage12)
├── HTTP/2 + HPACK         (stage14)
├── 内存池                  (stage10)
├── 异步日志                (stage10)
└── sendfile 零拷贝         (phase1)
```

```bash
cmake --build build --target advanced_server
build/bin/advanced_server -p 8443 -w 4 -r phase2/www \
    --cert phase2/certs/cert.pem --key phase2/certs/key.pem

curl http://localhost:8443/                          # HTTP/1.1
curl -k https://localhost:8443/                       # HTTPS
curl --http2-prior-knowledge http://localhost:8443/   # HTTP/2
# WebSocket: ws://localhost:8443/ws
```

---

## 一、为什么需要聚合

### 1.1 单特性 stage 的局限

每个 stage 只实现一个特性，其他部分用简化处理：

| stage | 实现的特性 | 简化的部分 |
|-------|-----------|-----------|
| stage8_tls | TLS 握手 + HTTPS | 单线程，无 HTTP/2 |
| stage10_mempool_log | 内存池 + 异步日志 | 无 TLS，无 HTTP/2 |
| stage12_websocket | WebSocket 协议 | 单线程，无 TLS |
| stage13_reuseport | SO_REUSEPORT 多线程 | 无 TLS，无 HTTP/2 |
| stage14_http2 | HTTP/2 + HPACK | 单线程，无 TLS，Huffman 未实现 |

**真实场景**：一个 HTTPS 请求需要同时用到 TLS + 多线程 + 内存池 + 异步日志 + epoll。
这些特性之间有交互，不能简单拼接。

### 1.2 整合的挑战

**挑战 1：协议检测**

同一条 TCP 连接，第一个字节可能是：
- `0x16` → TLS 握手（HTTPS）
- `P` (PRI * HTTP/2.0) → HTTP/2 连接前言
- `G` (GET) / `P` (POST) → HTTP/1.1

服务器必须读前几个字节，判断协议，然后分发到对应的 handler。

**挑战 2：TLS 与 HTTP/2 的关系**

HTTP/2 over TLS（h2）需要 ALPN 协商：
1. 客户端 TLS 握手时带 ALPN 扩展，列出支持的协议 `["h2", "http/1.1"]`
2. 服务器选择 `h2`，在 TLS 握手响应中带回
3. 握手完成后，直接发 HTTP/2 帧

HTTP/2 明文（h2c）不需要 TLS，直接以连接前言开头。

**挑战 3：WebSocket 与 HTTP/1.1 的关系**

WebSocket 握手是 HTTP/1.1 请求：
```http
GET /ws HTTP/1.1
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: ...
```

服务器先按 HTTP/1.1 解析，检测到 `Upgrade: websocket` 后切换到 WebSocket 模式。

**挑战 4：线程安全**

多线程环境下：
- `SSL_CTX` 可以被多线程共享（OpenSSL 1.1+ 线程安全）
- `SSL` 每连接独立，不能跨线程使用
- 内存池每连接独立，不需要加锁
- 异步日志有内置双缓冲锁，线程安全
- `epoll` 每线程独立，不需要加锁

### 1.3 设计目标

```
一个端口，四种协议，无缝切换
```

```
客户端连接
    │
    ▼
读前 24 字节
    │
    ├── 0x16          → TLS → 握手 → ALPN(h2?) → HTTP/2 or HTTP/1.1
    ├── "PRI * HTTP"  → HTTP/2 明文 (h2c)
    └── "GET"/"POST"  → HTTP/1.1 → WebSocket?
    │
    ▼
对应 handler 处理
```

---

## 二、整体架构

### 2.1 目录结构

```
phase2/server/
├── server.h          # 公共头文件（配置、连接、worker 结构定义）
├── server.c          # main 函数（配置解析 + SSL_CTX 创建 + 多线程启动）
├── worker.c          # 工作线程（SO_REUSEPORT + epoll 事件循环）
├── connection.c      # 连接管理（协议检测 + HTTP/1.1 + 分发）
├── tls_conn.c        # TLS 处理（SSL_CTX + 握手 + 读写 + 静态文件）
├── ws_conn.c         # WebSocket（SHA1 + Base64 + 帧解析/生成 + echo）
├── h2_conn.c         # HTTP/2（连接前言 + 帧解析 + HPACK + Huffman + 响应）
├── static_file.c     # 静态文件（MIME + sendfile 零拷贝）
└── CMakeLists.txt    # 编译配置
```

### 2.2 数据结构

#### 配置结构

```c
typedef struct {
    int  port;            /* 监听端口 */
    int  num_workers;     /* 工作线程数 */
    char www_root[512];   /* 静态文件根目录 */
    int  backlog;         /* listen backlog */

    int  tls_enabled;         /* 是否启用 TLS */
    char cert_path[512];     /* 证书路径 */
    char key_path[512];      /* 私钥路径 */

    int  uring_enabled;       /* 是否用 io_uring */
    char log_file[512];      /* 日志文件路径 */
} server_config_t;
```

#### 协议类型枚举

```c
typedef enum {
    PROTO_DETECTING = 0,   /* 尚未检测 */
    PROTO_HTTP1,           /* HTTP/1.1 明文 */
    PROTO_TLS,             /* TLS/HTTPS */
    PROTO_WEBSOCKET,       /* WebSocket */
    PROTO_HTTP2,           /* HTTP/2 明文 (h2c) */
} proto_type_t;
```

#### 连接结构

```c
struct conn {
    int            fd;
    proto_type_t   proto;          /* 协议类型 */

    http_parser_t  parser;         /* HTTP/1.1 解析器 */

    SSL           *ssl;            /* TLS 连接 */
    int            tls_handshake_done;

    ws_state_t     ws_state;       /* WebSocket 状态 */

    h2_stream_t    h2_streams[128]; /* HTTP/2 流 */
    int            h2_stream_count;
    int            h2_settings_done;

    pool_t        *pool;           /* 内存池 */
    int            worker_id;
};
```

#### 工作线程结构

```c
typedef struct {
    int       listen_fd;    /* 自己的监听 socket (SO_REUSEPORT) */
    int       epoll_fd;     /* 自己的 epoll */
    int       thread_id;
    const server_config_t *cfg;
    SSL_CTX  *ssl_ctx;      /* 共享 TLS 上下文 */
    long      accept_count;
    long      request_count;
} worker_t;
```

### 2.3 启动流程

```
main()
  ├── config_init()           # 默认配置
  ├── config_parse_args()     # 命令行参数
  ├── async_log_init()        # 异步日志
  ├── tls_ctx_create()        # SSL_CTX 工厂
  ├── 打印启动信息
  ├── signal(SIGINT, ...)     # 信号处理
  ├── for i in workers:
  │     pthread_create(worker_main)  # 创建工作线程
  └── pthread_join()          # 等待退出
```

```c
int main(int argc, char *argv[])
{
    config_init(&g_server.cfg);
    config_parse_args(&g_server.cfg, argc, argv);

    if (g_server.cfg.tls_enabled) {
        SSL_library_init();
        g_server.ssl_ctx = tls_ctx_create(
            g_server.cfg.cert_path, g_server.cfg.key_path);
    }

    for (int i = 0; i < g_server.num_workers; i++) {
        pthread_create(&g_server.workers[i].thread,
                       worker_main, &g_server.workers[i]);
    }

    for (int i = 0; i < g_server.num_workers; i++)
        pthread_join(g_server.workers[i].thread, NULL);
}
```

---

## 三、SO_REUSEPORT 多线程

### 3.1 为什么用 SO_REUSEPORT

**传统多线程模型**：

```
一个 listen_fd
    │
    ├── 线程1 ── epoll_wait ── accept（竞争）
    ├── 簇线程2 ── epoll_wait ── accept（竞争）
    └── 线程3 ── epoll_wait ── accept（竞争）
```

问题：多个线程竞争同一个 `listen_fd` 的 `accept()`，需要加锁或使用
`EPOLLEXCLUSIVE`，有惊群效应。

**SO_REUSEPORT 模型**：

```
端口 8443
    │
    ├── 线程1 ── listen_fd_1（独立）── epoll ── accept（无竞争）
    ├── 线程2 ── listen_fd_2（独立）── epoll ── accept（无竞争）
    └── 线程3 ── listen_fd_3（独立）── epoll ── accept（无竞争）
```

每个线程创建自己的 `listen_fd`，内核负责把连接分发到各线程。
**无锁、无惊群、完美负载均衡。**

### 3.2 worker_main 实现

```c
void *worker_main(void *arg)
{
    worker_t *w = arg;

    /* 1. 创建自己的 listen_fd */
    w->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    set_reuseaddr(w->listen_fd);
    set_reuseport(w->listen_fd);  /* 关键：SO_REUSEPORT */
    bind(w->listen_fd, ...);
    listen(w->listen_fd, w->cfg->backlog);

    /* 2. 创建自己的 epoll */
    w->epoll_fd = epoll_create1(0);
    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD,
              w->listen_fd, &{EPOLLIN});

    /* 3. 事件循环 */
    while (g_server.running) {
        int n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, 1000);
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == w->listen_fd) {
                /* 新连接 */
                int cfd = accept(w->listen_fd, ...);
                conn_t *conn = conn_create(cfd, w->thread_id);
                epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD,
                          cfd, &{EPOLLIN});
            } else {
                /* 已有连接可读 */
                conn_t *conn = ...;
                conn_handle_read(conn, w);
            }
        }
    }
}
```

### 3.3 内核分发原理

Linux 内核 3.9+ 支持 `SO_REUSEPORT`。多个 socket 绑定同一端口时，
内核用哈希算法把新连接分发到不同 socket：

```c
// 内核源码 net/core/sock_reuseport.c
struct sock *reuseport_select_sock(struct sock_reuseport *reuse,
                                    u32 hash, ...)
{
    // 用 hash % num_socks 选一个 socket
    return reuse->socks[hash % reuse->num_socks];
}
```

哈希输入是 `(src_ip, src_port, dst_ip, dst_port)`，保证：
1. 同一客户端的连接总是到同一线程（连接亲和性）
2. 不同客户端的连接均匀分布（负载均衡）

---

## 四、协议自动检测

### 4.1 检测原理

一条新连接进来，服务器不知道客户端要说什么协议。
需要读前几个字节来判断：

```
第一个字节:
  0x16 (22)  → TLS ClientHello
  'P' (0x50) → 可能是 HTTP/2 连接前言 "PRI * HTTP/2.0\r\n..."
  'G' (0x47) → HTTP/1.1 GET
  'P' (0x50) → HTTP/1.1 POST（与 HTTP/2 前言冲突！需进一步检测）
  'H' (0x48) → HTTP/1.1 HEAD
  'D' (0x44) → HTTP/1.1 DELETE
  ...
```

**注意**：`'P'` 既可能是 HTTP/2 前言（`PRI * HTTP/2.0`），
也可能是 HTTP/1.1 POST。需要读更多字节区分。

### 4.2 HTTP/2 连接前言

HTTP/2 明文连接（h2c）以固定的 24 字节开头：

```
PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n
```

这是 RFC 7540 规定的"客户端连接前言"（Client Connection Preface）。
服务器读到这 24 字节就知道是 HTTP/2。

### 4.3 detect_protocol 实现

```c
proto_type_t detect_protocol(const char *buf, int len)
{
    if (len < 1) return PROTO_DETECTING;

    /* TLS: 第一个字节是 0x16 (ContentType=Handshake) */
    if ((uint8_t)buf[0] == 0x16) return PROTO_TLS;

    /* HTTP/2 连接前言: "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" */
    if (len >= 24 && memcmp(buf, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) == 0)
        return PROTO_HTTP2;

    /* HTTP/1.1: GET / POST / HEAD / PUT / DELETE / ... */
    if (buf[0] == 'G' || buf[0] == 'P' || buf[0] == 'H' ||
        buf[0] == 'D' || buf[0] == 'O' || buf[0] == 'C' ||
        buf[0] == 'T')
        return PROTO_HTTP1;

    return PROTO_DETECTING;  /* 无法识别 */
}
```

### 4.4 分发流程

```
epoll_wait → conn_handle_read()
    │
    ├── proto == PROTO_DETECTING
    │     read(fd, buf, 24)
    │     detect_protocol(buf)
    │     ├── PROTO_TLS     → tls_do_handshake()
    │     ├── PROTO_HTTP2   → h2_handle_data()
    │     └── PROTO_HTTP1   → http_parser_execute()
    │
    ├── proto == PROTO_HTTP1
    │     http_parser_execute()
    │     检测 Upgrade: websocket?
    │     ├── 是 → ws_do_handshake() → proto = PROTO_WEBSOCKET
    │     └── 否 → serve_static_file() / api_response()
    │
    ├── proto == PROTO_TLS
    │     tls_read() → 解密
    │     内部检测 HTTP/1.1 or HTTP/2 (ALPN)
    │
    ├── proto == PROTO_WEBSOCKET
    │     ws_parse_frame() → ws_handle_message()
    │
    └── proto == PROTO_HTTP2
          h2_handle_data() → 帧解析 → HPACK 解码 → 响应
```

---

## 五、TLS/HTTPS 集成

### 5.1 SSL_CTX 工厂

`SSL_CTX` 是 TLS 配置工厂，所有连接共享：

```c
SSL_CTX *tls_ctx_create(const char *cert_path, const char *key_path)
{
    SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());

    SSL_CTX_set_options(ctx,
        SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |   /* 禁用不安全协议 */
        SSL_OP_NO_COMPRESSION);                /* 禁用压缩（CRIME 攻击）*/

    SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM);

    /* ALPN: 支持 h2 和 http/1.1 */
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);

    return ctx;
}
```

### 5.2 ALPN 协商

ALPN（Application-Layer Protocol Negotiation）让客户端在 TLS 握手时
告诉服务器它支持哪些应用层协议：

```c
static int alpn_select_cb(SSL *ssl,
                          const unsigned char **out,
                          unsigned char *outlen,
                          const unsigned char *in,
                          unsigned int inlen, void *arg)
{
    /* 客户端列表: "h2", "http/1.1" */
    /* 优先选择 h2 */
    if (SSL_select_next_proto(out, outlen,
                              (const unsigned char *)"\x02h2\x08http/1.1",
                              11, in, inlen) == OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_OK;

    return SSL_TLSEXT_ERR_NOACK;
}
```

### 5.3 TLS 握手

TLS 握手是多轮交互，不能一次 `accept` 就完成：

```c
int tls_do_handshake(conn_t *conn, SSL_CTX *ctx)
{
    if (!conn->ssl) {
        conn->ssl = SSL_new(ctx);
        SSL_set_fd(conn->ssl, conn->fd);
        SSL_set_accept_state(conn->ssl);
    }

    int ret = SSL_do_handshake(conn->ssl);
    if (ret == 1) {
        conn->tls_handshake_done = 1;
        return 0;  /* 握手完成 */
    }

    int err = SSL_get_error(conn->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return -1;  /* 需要更多数据，等下次 epoll 通知 */

    return -2;  /* 错误 */
}
```

### 5.4 TLS 读写

握手完成后，用 `SSL_read` / `SSL_write` 代替 `read` / `write`：

```c
int tls_read(conn_t *conn, char *buf, int size)
{
    int n = SSL_read(conn->ssl, buf, size);
    if (n > 0) return n;

    int err = SSL_get_error(conn->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return -1;  /* EAGAIN */
    return 0;  /* 连接关闭或错误 */
}
```

---

## 六、WebSocket 集成

### 6.1 从 HTTP/1.1 到 WebSocket

WebSocket 连接始于一个 HTTP/1.1 请求：

```http
GET /ws HTTP/1.1
Host: localhost:8443
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
```

服务器检测到 `Upgrade: websocket` 后，返回 101 响应：

```http
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

之后连接从 HTTP/1.1 切换到 WebSocket 二进制帧协议。

### 6.2 握手响应生成

```c
int ws_do_handshake(conn_t *conn, const char *buf, int len)
{
    /* 1. 从请求头提取 Sec-WebSocket-Key */
    char key[128];
    extract_header(buf, "Sec-WebSocket-Key:", key);

    /* 2. 拼 GUID，算 SHA1，Base64 编码 */
    char accept[128];
    ws_compute_accept(key, accept);

    /* 3. 发 101 响应 */
    char resp[512];
    snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    write(conn->fd, resp, strlen(resp));

    conn->proto = PROTO_WEBSOCKET;
    conn->ws_state = WS_STATE_CONNECTED;
    return 0;
}
```

### 6.3 Sec-WebSocket-Accept 计算

```
accept = Base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
```

```c
static void ws_compute_accept(const char *key, char *out)
{
    char concat[256];
    snprintf(concat, sizeof(concat), "%s%s", key,
             "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

    unsigned char sha1[20];
    SHA1((unsigned char *)concat, strlen(concat), sha1);

    base64_encode(sha1, 20, out);
}
```

### 6.4 WebSocket 帧格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
|     Extended payload length continued, if payload len == 127  |
+ - - - - - - - - - - - - - - - +-------------------------------+
|                               |Masking-key, if MASK set to 1  |
+-------------------------------+-------------------------------+
| Masking-key (continued)       |          Payload Data         |
+-------------------------------+ - - - - - - - - - - - - - - - +
:                     Payload Data continued ...                :
```

**opcode 类型**：
- `0x0` continuation
- `0x1` text frame
- `0x2` binary frame
- `0x8` close
- `0x9` ping
- `0xA` pong

### 6.5 帧解析

```c
int ws_parse_frame(const char *buf, int buf_len,
                    int *opcode, char **payload, int *payload_len)
{
    int pos = 0;

    /* 第一个字节: FIN + opcode */
    int fin = (buf[0] >> 7) & 1;
    *opcode = buf[0] & 0x0F;
    pos = 1;

    /* 第二个字节: MASK + payload_len */
    int masked = (buf[1] >> 7) & 1;
    int len = buf[1] & 0x7F;
    pos = 2;

    /* 扩展长度 */
    if (len == 126) {
        len = (buf[2] << 8) | buf[3];
        pos = 4;
    } else if (len == 127) {
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | buf[2 + i];
        pos = 10;
    }

    /* 掩码键 */
    uint8_t mask[4];
    if (masked) {
        memcpy(mask, buf + pos, 4);
        pos += 4;
    }

    /* 负载（解除掩码） */
    *payload = malloc(len);
    for (int i = 0; i < len; i++) {
        (*payload)[i] = buf[pos + i];
        if (masked) (*payload)[i] ^= mask[i % 4];
    }
    *payload_len = len;

    return pos + len;
}
```

### 6.6 帧生成（服务器→客户端）

服务器发送的帧**不需要掩码**：

```c
int ws_send_frame(int fd, int opcode, const char *data, int len)
{
    char header[10];
    int pos = 0;

    /* FIN=1, opcode */
    header[pos++] = 0x80 | opcode;

    /* MASK=0, payload_len */
    if (len < 126) {
        header[pos++] = len;
    } else if (len < 65536) {
        header[pos++] = 126;
        header[pos++] = (len >> 8) & 0xFF;
        header[pos++] = len & 0xFF;
    } else {
        header[pos++] = 127;
        for (int i = 7; i >= 0; i--)
            header[pos++] = (len >> (i * 8)) & 0xFF;
    }

    write(fd, header, pos);
    write(fd, data, len);
    return pos + len;
}
```

### 6.7 Echo 处理

```c
void ws_handle_message(conn_t *conn, int opcode,
                        char *payload, int payload_len)
{
    switch (opcode) {
    case 0x1:  /* text */
    case 0x2:  /* binary */
        /* echo: 原样发回 */
        ws_send_frame(conn->fd, opcode, payload, payload_len);
        break;
    case 0x8:  /* close */
        ws_send_frame(conn->fd, 0x8, payload, payload_len);
        break;
    case 0x9:  /* ping → pong */
        ws_send_frame(conn->fd, 0xA, payload, payload_len);
        break;
    }
}
```

---

## 七、HTTP/2 集成

### 7.1 HTTP/2 帧格式

HTTP/2 是二进制帧协议，所有数据都以帧为单位传输：

```
+-----------------------------------------------+
|                 Length (24)                    |
+---------------+---------------+---------------+
|   Type (8)    |   Flags (8)   |
+-+-------------+---------------+-------------------------------+
|R|                 Stream Identifier (31)                      |
+=+=============================================================+
|                   Frame Payload (0...)                       |
+---------------------------------------------------------------+
```

**帧类型**：
| 类型 | 值 | 说明 |
|------|-----|------|
| DATA | 0x0 | 请求/响应体 |
| HEADERS | 0x1 | 头部（HPACK 编码） |
| SETTINGS | 0x4 | 连接参数 |
| PUSH_PROMISE | 0x5 | 服务器推送 |
| PING | 0x6 | 心跳 |
| GOAWAY | 0x7 | 关闭连接 |
| WINDOW_UPDATE | 0x8 | 流控 |

### 7.2 连接前言

```c
int h2_check_preface(const char *buf, int len)
{
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (len < 24) return 0;
    return memcmp(buf, preface, 24) == 0;
}
```

### 7.3 帧头解析

```c
typedef struct {
    uint32_t length;    /* 负载长度（24 位） */
    uint8_t  type;      /* 帧类型 */
    uint8_t  flags;     /* 标志位 */
    uint32_t stream_id; /* 流 ID（31 位，最高位保留） */
} h2_frame_hdr_t;

static void h2_parse_frame_hdr(const uint8_t *buf, h2_frame_hdr_t *hdr)
{
    hdr->length = (buf[0] << 16) | (buf[1] << 8) | buf[2];
    hdr->type   = buf[3];
    hdr->flags  = buf[4];
    hdr->stream_id = ((buf[5] & 0x7F) << 24) |
                     (buf[6] << 16) | (buf[7] << 8) | buf[8];
}
```

### 7.4 SETTINGS 交换

连接前言之后，双方互发 SETTINGS 帧：

```c
int h2_send_settings(int fd)
{
    uint8_t frame[9 + 6];
    /* 帧头: length=6, type=0x4, flags=0, stream_id=0 */
    frame[0] = 0; frame[1] = 0; frame[2] = 6;
    frame[3] = 0x04;  /* SETTINGS */
    frame[4] = 0;
    frame[5] = 0; frame[6] = 0; frame[7] = 0; frame[8] = 0;

    /* 设置项: SETTINGS_MAX_CONCURRENT_STREAMS = 128 */
    frame[9]  = 0; frame[10] = 0x03;  /* ID */
    frame[11] = 0; frame[12] = 0; frame[13] = 0; frame[14] = 128;

    write(fd, frame, sizeof(frame));
    return 0;
}
```

### 7.5 HPACK 头部压缩

HTTP/2 用 HPACK 算法压缩头部，减少冗余。

**HPACK 三种编码方式**：

1. **索引引用**（1 字节）：直接从静态表查
   ```
   1xxxxxxx  → index = xxxxxxx
   ```

2. **字面头部 + 增量索引**：新增到动态表
   ```
   01xxxxxx  → name index
   ```

3. **字面头部 + 不索引**：一次性使用
   ```
   0000xxxx  → name index
   ```

**静态表**（RFC 7541）前几项：

| index | name | value |
|-------|------|-------|
| 1 | :authority | |
| 2 | :method | GET |
| 3 | :method | POST |
| 4 | :path | / |
| 5 | :path | /index.html |
| 6 | :scheme | http |
| 7 | :scheme | https |
| 8 | :status | 200 |

### 7.6 HPACK 整数解码

```c
static const uint8_t *hpack_decode_int(const uint8_t *buf,
                                        int prefix_bits,
                                        uint32_t *value)
{
    uint8_t mask = (1 << prefix_bits) - 1;
    *value = *buf & mask;
    buf++;

    if (*value < mask) return buf;  /* 一字节搞定 */

    /* 多字节扩展 */
    uint32_t m = 0;
    while (1) {
        *value += (*buf & 0x7F) << m;
        if (!(*buf & 0x80)) { buf++; return buf; }
        m += 7;
        buf++;
    }
}
```

### 7.7 HPACK 字符串解码

字符串可能用 Huffman 编码压缩：

```c
static const uint8_t *hpack_decode_str(const uint8_t *buf,
                                        char *str, int max_len)
{
    int huffman = (*buf & 0x80) != 0;
    uint32_t len;
    buf = hpack_decode_int(buf, 7, &len);

    if (huffman) {
        huff_decode(buf, len, str, max_len);
    } else {
        strncpy(str, (const char *)buf, len);
        str[len] = '\0';
    }
    return buf + len;
}
```

### 7.8 Huffman 解码

HPACK 用 Huffman 编码压缩字符串。每个字符对应一个变长二进制码：

```c
typedef struct { uint32_t code; int bits; int sym; } huff_dec_t;

/* RFC 7541 Appendix B 的 Huffman 编码表 */
static const huff_dec_t huff_table[] = {
    {0x1ff8,13,0},   {0x7fffd8,23,1},  ...  /* 256 个符号 + EOS */
};

static int huff_decode(const uint8_t *src, int src_len,
                        char *dst, int dst_max)
{
    uint64_t bits = 0;
    int nbits = 0, dout = 0, i = 0;

    while (i < src_len || nbits > 0) {
        /* 填充 bit 缓冲区 */
        while (nbits < 32 && i < src_len) {
            bits = (bits << 8) | src[i++];
            nbits += 8;
        }
        if (nbits == 0) break;

        /* 从长到短尝试匹配 */
        int found = 0;
        for (int b = 32; b >= 5; b--) {
            if (nbits < b) continue;
            uint64_t code = (bits >> (nbits - b)) & ((1ULL << b) - 1);
            for (int j = 0; j < HUFF_TABLE_SIZE; j++) {
                if (huff_table[j].bits == b &&
                    huff_table[j].code == code) {
                    dst[dout++] = (char)huff_table[j].sym;
                    nbits -= b;
                    bits &= (1ULL << nbits) - 1;  /* 清除已消费位 */
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) { nbits -= 8; bits &= (1ULL << nbits) - 1; }
    }
    dst[dout] = '\0';
    return dout;
}
```

**关键修复**：在 `nbits -= b` 后必须执行 `bits &= (1ULL << nbits) - 1`
清除高位已消费的 bit，否则下次循环会使用旧的 bit 值导致解码失败。

### 7.9 HPACK 头部块解码

```c
static void hpack_decode(const uint8_t *buf, int len,
                          h2_stream_t *stream)
{
    const uint8_t *end = buf + len;

    while (buf < end) {
        if (*buf & 0x80) {
            /* 索引引用 */
            uint32_t index;
            buf = hpack_decode_int(buf, 7, &index);
            /* 从静态表查找 name/value */
            apply_static_table(stream, index);

        } else if (*buf & 0x40) {
            /* 字面头部 + 增量索引 (01 prefix) */
            uint32_t index;
            buf = hpack_decode_int(buf, 6, &index);
            char name[256], value[512];
            if (index == 0)
                buf = hpack_decode_str(buf, name, sizeof(name));
            else
                strcpy(name, HPACK_STATIC_TABLE[index-1].name);
            buf = hpack_decode_str(buf, value, sizeof(value));
            apply_header(stream, name, value);

        } else if (*buf & 0x20) {
            /* 动态表大小更新，跳过 */
            uint32_t size;
            buf = hpack_decode_int(buf, 5, &size);
        } else {
            /* 字面头部 + 不索引 (0000 prefix) */
            /* 类似 01 但不加入动态表 */
        }
    }
}
```

### 7.10 HTTP/2 响应生成

```c
int h2_send_response(int fd, uint32_t stream_id, int status,
                      const char *content_type,
                      const char *body, int body_len)
{
    /* 1. HPACK 编码响应头 */
    uint8_t headers[256];
    int hlen = 0;

    /* :status: 200 → 索引引用 (index=8) */
    headers[hlen++] = 0x88;  /* 10001000 = indexed, index=8 */

    /* content-type: text/html */
    headers[hlen++] = 0x5F;  /* 字面头部, name index=31 (content-type) */
    /* Huffman 编码的 "text/html" */
    ...

    /* 2. 发 HEADERS 帧 */
    uint8_t frame[9 + 256];
    h2_encode_frame_hdr(frame, hlen, 0x01,  /* HEADERS */
                        H2_FLAG_END_HEADERS, stream_id);
    memcpy(frame + 9, headers, hlen);
    write(fd, frame, 9 + hlen);

    /* 3. 发 DATA 帧 */
    h2_encode_frame_hdr(frame, body_len, 0x00,  /* DATA */
                        H2_FLAG_END_STREAM, stream_id);
    memcpy(frame + 9, body, body_len);
    write(fd, frame, 9 + body_len);

    return 0;
}
```

---

## 八、内存池集成

### 8.1 为什么用内存池

每处理一个 HTTP 请求，需要多次 `malloc`：
- 解析 HTTP 头部 → 分配临时缓冲区
- 生成响应 → 分配响应缓冲区
- URL 解析 → 分配路径副本

频繁 `malloc`/`free` 会导致：
1. 内存碎片
2. 系统调用开销
3. 缓存局部性差

### 8.2 Nginx 风格内存池

```c
typedef struct pool_node {
    struct pool_node *next;
    char data[];
} pool_node_t;

typedef struct {
    pool_node_t *head;    /* 大块链表 */
    char *cur;            /* 当前块位置 */
    size_t remain;        /* 当前块剩余 */
} pool_t;

pool_t *pool_create(size_t init_size);
void   pool_destroy(pool_t *pool);
void  *pool_alloc(pool_t *pool, size_t size);
```

### 8.3 连接级内存池

每个连接创建一个内存池，连接关闭时一次性释放：

```c
conn_t *conn_create(int fd, int worker_id)
{
    conn_t *conn = malloc(sizeof(conn_t));
    conn->pool = pool_create(4096);  /* 4KB 初始块 */
    ...
    return conn;
}

void conn_free(conn_t *conn)
{
    pool_destroy(conn->pool);  /* 一次性释放所有内存 */
    ...
}
```

---

## 九、异步日志集成

### 9.1 为什么需要异步日志

同步日志（`fprintf` 到文件）的问题：
1. 磁盘 IO 阻塞工作线程
2. 多线程竞争文件锁
3. 影响请求处理延迟

### 9.2 双缓冲机制

```
前端（工作线程）          后端（日志线程）
┌─────────────┐         ┌─────────────┐
│  front_buf  │ ──swap──│  back_buf   │ ──write──→ 文件
└─────────────┘         └─────────────┘
```

```c
void alog_info(const char *msg)
{
    pthread_mutex_lock(&log.mutex);
    /* 写入前端缓冲区 */
    log_buffer_append(&log.front, msg);
    pthread_cond_signal(&log.cond);
    pthread_mutex_unlock(&log.mutex);
}

/* 日志线程 */
void *log_thread(void *arg)
{
    while (1) {
        pthread_mutex_lock(&log.mutex);
        while (log.front.len == 0 && log.running)
            pthread_cond_wait(&log.cond, &log.mutex);
        /* 交换前后端缓冲区 */
        swap(&log.front, &log.back);
        pthread_mutex_unlock(&log.mutex);

        /* 写后端缓冲区到文件（无锁） */
        write(log.fd, log.back.data, log.back.len);
        log.back.len = 0;
    }
}
```

### 9.3 日志级别

```c
typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} log_level_t;

alog_info("新连接 fd=%d", fd);
alog_warn("TLS 握手失败");
alog_error("accept: %s", strerror(errno));
```

---

## 十、静态文件处理

### 10.1 MIME 类型检测

```c
const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(ext, ".txt")  == 0) return "text/plain";
    return "application/octet-stream";
}
```

### 10.2 sendfile 零拷贝

```c
int serve_static_file(int fd, const char *uri,
                       const char *www_root)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s%s", www_root, uri);

    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        send_404(fd);
        return -1;
    }

    struct stat st;
    fstat(file_fd, &st);

    /* 发响应头 */
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n\r\n",
        get_mime_type(path), st.st_size);
    write(fd, header, strlen(header));

    /* sendfile: 内核直接从文件 fd 拷贝到 socket fd */
    /* 不经过用户空间，零拷贝 */
    off_t offset = 0;
    sendfile(fd, file_fd, &offset, st.st_size);

    close(file_fd);
    return 0;
}
```

**sendfile vs read+write**：

```
read + write:
  文件 → 内核 → 用户空间 → 内核 → socket
  4 次拷贝 + 2 次系统调用

sendfile:
  文件 → 内核 → socket
  2 次拷贝 + 1 次系统调用
```

---

## 十一、连接生命周期

### 11.1 完整流程

```
1. accept() → 得到 client_fd
2. conn_create(client_fd) → 初始化连接结构
3. epoll_ctl(ADD, client_fd, EPOLLIN)

4. epoll_wait → client_fd 可读
5. conn_handle_read():
   a. read(client_fd, buf, 24)
   b. detect_protocol(buf)
   c. 分发到对应 handler

6. handler 处理请求
7. 发送响应
8. 如果 keep-alive → 回到 4
9. 否则 → conn_free() → close(client_fd)
```

### 11.2 连接关闭

```c
void conn_free(conn_t *conn)
{
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    if (conn->pool)
        pool_destroy(conn->pool);
    close(conn->fd);
    free(conn);
}
```

---

## 十二、编译与运行

### 12.1 CMakeLists.txt

```cmake
add_executable(advanced_server
    server.c
    worker.c
    connection.c
    tls_conn.c
    ws_conn.c
    h2_conn.c
    static_file.c
    # 外部依赖
    ../../phase1/stage5_http_parser/http_parser.c
    ../stage10_mempool_log/mempool.c
    ../stage10_mempool_log/async_log.c
)

target_link_libraries(advanced_server
    common_v2
    pthread
    ssl
    crypto
)
```

### 12.2 编译

```bash
cd build
cmake ..
cmake --build . --target advanced_server
```

### 12.3 生成自签名证书

```bash
openssl req -x509 -newkey rsa:2048 \
    -keyout phase2/certs/key.pem \
    -out phase2/certs/cert.pem \
    -days 365 -nodes \
    -subj '/CN=localhost'
```

### 12.4 启动

```bash
# 基本启动
build/bin/advanced_server -p 8443 -w 4 -r phase2/www \
    --cert phase2/certs/cert.pem \
    --key phase2/certs/key.pem

# 禁用 TLS
build/bin/advanced_server -p 8080 --no-tls

# 启用日志
build/bin/advanced_server -p 8443 -l /var/log/server.log
```

### 12.5 命令行选项

```
选项:
  -p PORT       监听端口 (默认 8443)
  -w WORKERS    工作线程数 (默认 4)
  -r ROOT       静态文件根目录 (默认 ./www)
  --cert PATH   TLS 证书路径
  --key PATH    TLS 私钥路径
  --no-tls      禁用 TLS
  --no-uring    禁用 io_uring
  -l FILE       日志文件路径
  -h            显示帮助
```

### 12.6 测试

```bash
# HTTP/1.1
curl http://localhost:8443/
curl http://localhost:8443/api/info

# HTTPS
curl -k https://localhost:8443/
curl -k https://localhost:8443/api/info

# HTTP/2 明文 (h2c)
curl --http2-prior-knowledge http://localhost:8443/
curl --http2-prior-knowledge http://localhost:8443/api/info

# HTTP/2 over TLS (h2)
curl --http2 -k https://localhost:8443/

# WebSocket (用 websocat 或浏览器)
# ws://localhost:8443/ws
```

---

## 十三、测试验证

### 13.1 功能测试结果

```
=== HTTP/1.1 / ===
<!DOCTYPE html>
<html lang="zh">
<head>
    <meta charset="UTF-8">
    <title>tiny_server</title>

=== HTTP/1.1 /api/info ===
{"status":"ok","proto":"http1","worker":3,"method":"GET","uri":"/api/info"}

=== HTTP/2 /api/info ===
{"status":"ok","proto":"h2","worker":2,"method":"GET","path":"/api/info"}

=== HTTPS /api/info ===
{"status":"ok","proto":"https","worker":1}

=== WebSocket ===
WS handshake: True
WS echo: b'Hello WebSocket!'
```

### 13.2 多线程验证

`/api/info` 返回的 `worker` 字段显示不同请求被不同线程处理：

```
worker:0 ← 线程 0 处理
worker:1 ← 线程 1 处理
worker:2 ← 线程 2 处理
worker:3 ← 线程 3 处理
```

SO_REUSEPORT 内核分发正常工作。

### 13.3 HTTP/2 Huffman 解码验证

curl 发送 HTTP/2 请求时，HPACK 头部用 Huffman 编码压缩。
服务器正确解码 `:path: /api/info`，返回 200 而非 404。

---

## 十四、性能分析

### 14.1 各协议开销对比

| 协议 | 握手 | 每请求开销 | 头部压缩 | 多路复用 |
|------|------|-----------|---------|---------|
| HTTP/1.1 | 无 | 小 | 无 | 无 |
| HTTPS | TLS (2-RTT) | 加解密 | 无 | 无 |
| HTTP/2 h2c | 连接前言 | 小 | HPACK | 有 |
| HTTP/2 h2 | TLS + ALPN | 加解密 | HPACK | 有 |
| WebSocket | HTTP 升级 | 小 | 无 | 无 |

### 14.2 线程模型对比

| 模型 | 锁竞争 | 惊群 | 负载均衡 | 扩展性 |
|------|-------|------|---------|-------|
| 单线程 | 无 | 无 | N/A | 差 |
| 多线程+锁 | 高 | 有 | 内核决定 | 一般 |
| SO_REUSEPORT | 无 | 无 | 哈希均匀 | 好 |

### 14.3 内存管理对比

| 方式 | 分配速度 | 碎片 | 缓存友好 |
|------|---------|------|---------|
| malloc/free | 慢 | 严重 | 差 |
| 内存池 | 快 | 无 | 好 |
| slab | 最快 | 无 | 最好 |

---

## 十五、关键实现细节

### 15.1 协议检测的边界情况

```c
proto_type_t detect_protocol(const char *buf, int len)
{
    if (len < 1) return PROTO_DETECTING;

    /* TLS: 0x16 */
    if ((uint8_t)buf[0] == 0x16) return PROTO_TLS;

    /* HTTP/2 前言需要 24 字节 */
    if (len >= 24 && memcmp(buf, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) == 0)
        return PROTO_HTTP2;

    /* 'P' 可能是 POST 或 PRI * HTTP/2.0 */
    /* 如果只有 1-23 字节，需要等更多数据 */
    if (buf[0] == 'P' && len < 24) return PROTO_DETECTING;

    /* HTTP/1.1 方法 */
    ...
}
```

### 15.2 TLS 非阻塞握手

TLS 握手可能需要多次 `epoll_wait` 循环：

```c
/* 第一次 read: 客户端发 ClientHello */
/* SSL_do_handshake 返回 WANT_READ → 等更多数据 */
/* 第二次 read: 客户端发 ClientKeyExchange 等 */
/* SSL_do_handshake 返回 1 → 握手完成 */
```

### 15.3 HTTP/2 流管理

```c
h2_stream_t *h2_get_stream(conn_t *conn, uint32_t id)
{
    for (int i = 0; i < conn->h2_stream_count; i++)
        if (conn->h2_streams[i].id == id)
            return &conn->h2_streams[i];

    /* 新流 */
    if (conn->h2_stream_count < H2_MAX_STREAMS) {
        h2_stream_t *s = &conn->h2_streams[conn->h2_stream_count++];
        s->id = id;
        s->state = H2_STREAM_OPEN;
        return s;
    }
    return NULL;
}
```

### 15.4 WebSocket 掩码处理

客户端→服务器的帧必须带掩码，服务器→客户端的帧不能带掩码：

```c
/* 解除客户端掩码 */
for (int i = 0; i < payload_len; i++)
    payload[i] ^= mask[i % 4];
```

### 15.5 keep-alive 连接复用

HTTP/1.1 keep-alive 在同一条 TCP 上发多个请求：

```c
/* 请求处理完后不 close，继续 epoll_wait */
/* 下次 read 会得到新的 HTTP 请求 */
```

---

## 十六、安全考虑

### 16.1 TLS 安全配置

```c
SSL_CTX_set_options(ctx,
    SSL_OP_NO_SSLv2 |       /* 禁用 SSLv2（不安全） */
    SSL_OP_NO_SSLv3 |       /* 禁用 SSLv3（POODLE 攻击） */
    SSL_OP_NO_COMPRESSION | /* 禁用压缩（CRIME 攻击） */
    SSL_OP_NO_RENEGOTIATION /* 禁用重协商 */
);
```

### 16.2 缓冲区溢出防护

所有字符串操作都指定最大长度：

```c
strncpy(cfg->www_root, argv[++i], sizeof(cfg->www_root) - 1);
cfg->www_root[sizeof(cfg->www_root) - 1] = '\0';
```

### 16.3 资源限制

```c
#define H2_MAX_STREAMS 128    /* HTTP/2 最大并发流 */
#define MAX_EVENTS 256        /* epoll 最大事件 */
#define MAX_HEADER_LEN 8192  /* 最大头部长度 */
```

---

## 十七、扩展方向

### 17.1 HTTP/2 服务器推送

```c
/* 客户端请求 /index.html */
/* 服务器主动推送 /style.css */
h2_send_push_promise(fd, stream_id, "/style.css");
h2_send_response(fd, pushed_stream_id, 200,
                  "text/css", css_body, css_len);
```

### 17.2 io_uring 异步文件读

```c
/* 用 io_uring 替代 sendfile 做异步文件读 */
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, file_fd, buf, size, offset);
io_uring_submit(&ring);
/* 完成后从 CQE 取结果，发响应 */
```

### 17.3 连接限流

```c
/* 每线程维护连接计数 */
if (w->conn_count > MAX_CONNS_PER_THREAD) {
    /* 拒绝新连接或关闭旧连接 */
}
```

### 17.4 优雅退出

```c
void on_signal(int sig)
{
    g_server.running = 0;
    /* 停止 accept */
    /* 等待现有请求处理完 */
    /* 关闭所有连接 */
}
```

---

## 十八、总结

### 18.1 整合的 7 个特性

| 特性 | 来源 | 在聚合版中的作用 |
|------|------|----------------|
| TLS/HTTPS | stage8 | 加密通信 |
| io_uring | stage9 | 异步文件 IO |
| 内存池 | stage10 | 高效内存管理 |
| 异步日志 | stage10 | 无阻塞日志 |
| SO_REUSEPORT | stage13 | 多线程无锁 |
| WebSocket | stage12 | 实时双向通信 |
| HTTP/2 | stage14 | 多路复用 + 头部压缩 |

### 18.2 架构亮点

1. **一个端口四种协议**：自动检测，无缝切换
2. **无锁多线程**：SO_REUSEPORT 每线程独立
3. **零拷贝文件传输**：sendfile 系统调用
4. **高效内存管理**：连接级内存池
5. **无阻塞日志**：双缓冲异步写入

### 18.3 从教学到生产

这个聚合版服务器已经具备了生产级 Web 服务器的核心要素，
但还有以下差距：

| 教学版 | 生产版 (Nginx) |
|-------|---------------|
| 固定大小缓冲区 | 动态缓冲区管理 |
| 简单路由 | 复杂配置 + location 匹配 |
| 无超时处理 | 连接/请求/keep-alive 超时 |
| 无限流 | 限速 + 限连接数 |
| echo WebSocket | WebSocket 代理 + 子协议 |
| 基础 HTTP/2 | HTTP/2 服务器推送 + 优先级 |
| 无缓存 | 内存/磁盘缓存 + 缓存控制 |
| 无健康检查 | 主动/被动健康检查 |
| 无配置热加载 | SIGHUP 热加载 |

### 18.4 学到的核心知识

1. **协议检测**：通过前几个字节判断协议类型
2. **TLS 集成**：SSL_CTX 共享 + 每连接 SSL + ALPN 协商
3. **WebSocket 升级**：HTTP/1.1 → 101 → 二进制帧
4. **HTTP/2 帧解析**：二进制帧 + HPACK + Huffman
5. **多线程模型**：SO_REUSEPORT 无锁分发
6. **内存管理**：池化分配 + 连接级生命周期
7. **异步设计**：epoll + 非阻塞 IO + 双缓冲日志

---

## 附录 A：完整文件清单

```
phase2/server/
├── server.h          (237 行)  公共头文件
├── server.c          (216 行)  main + 配置
├── worker.c          (~150 行) 工作线程 + epoll
├── connection.c      (~200 行) 连接管理 + 协议检测
├── tls_conn.c        (~180 行) TLS 处理
├── ws_conn.c         (~220 行) WebSocket 处理
├── h2_conn.c         (~700 行) HTTP/2 + HPACK + Huffman
├── static_file.c     (~100 行) 静态文件
└── CMakeLists.txt    (30 行)   编译配置
```

## 附录 B：HPACK 静态表

```
 1: :authority          ""
 2: :method             "GET"
 3: :method             "POST"
 4: :path               "/"
 5: :path               "/index.html"
 6: :scheme             "http"
 7: :scheme             "https"
 8: :status             "200"
 9: :status             "204"
10: :status             "206"
11: :status             "304"
12: :status             "400"
13: :status             "404"
14: :status             "500"
...
61: www-authenticate    ""
```

## 附录 C：Huffman 编码示例

```
字符  ASCII  Huffman码           位长  压缩比
'/'    0x2F   0x18 (00011000)     6    25%
'a'    0x61   0x21 (00100001)     6    25%
'p'    0x70   0x70 (01110000)     7    12.5%
'i'    0x69   0x63 (01100011)     7    12.5%
':'    0x3A   0x15 (00010101)     6    25%

"/api/info" 原文 9 字节 → Huffman 编码后 ~7 字节
```

## 附录 D：调试技巧

### D.1 查看 HTTP/2 帧

```bash
curl --http2-prior-knowledge -v http://localhost:8443/ 2>&1 | head -30
```

### D.2 查看 TLS 握手

```bash
openssl s_client -connect localhost:8443 -showcerts
```

### D.3 WebSocket 测试

```bash
# 安装 websocat
cargo install websocat

# 连接
websocat ws://localhost:8443/ws
```

### D.4 内存检查

```bash
valgrind --leak-check=full \
    build/bin/advanced_server -p 8443 -w 1 --no-tls
```

### D.5 性能分析

```bash
# 编译时加 -pg
perf record -g build/bin/advanced_server -p 8443
perf report
```