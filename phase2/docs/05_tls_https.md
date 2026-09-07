# stage8 - TLS/HTTPS 服务器

## 本章导读

在 phase1 的七个阶段里，我们写了一个能跑的 Web 服务器——但它有一个致命问题：
**所有数据在网络上明文传输**。

你在咖啡馆连 WiFi，打开 `http://localhost:8080/`，旁边的人用 Wireshark 抓包，
就能看到你请求的 URL、发送的表单、返回的页面内容——一览无余。

**TLS（Transport Layer Security）** 就是解决这个问题的。
它在 TCP 之上、HTTP 之下加了一层加密，让数据变成密文传输。
加了 TLS 的 HTTP 就是 **HTTPS**。

本章我们给 phase1 的 Web 服务器穿上 TLS 的盔甲，让它变成 `https://` 服务器。

```bash
# 编译
cmake --build build --target tls_server

# 生成自签名证书
bash phase2/stage8_tls/gen_cert.sh

# 启动 HTTPS 服务器
build/bin/tls_server 8443 4 ./phase2/www \
  phase2/stage8_tls/certs/cert.pem \
  phase2/stage8_tls/certs/key.pem

# 测试（-k 跳过证书验证，因为自签名证书不被浏览器信任）
curl -k https://localhost:8443/
```

---

## 一、为什么需要 TLS

### 1.1 HTTP 的三大罪

HTTP/1.1 在网络上裸奔，有三个致命问题：

| 问题 | 说明 | 后果 |
|------|------|------|
| **窃听** | 数据明文传输 | 密码、信用卡号被抓包看到 |
| **篡改** | 没有完整性校验 | 返回的页面被注入广告/恶意脚本 |
| **冒充** | 没有身份验证 | 连到了假 WiFi → 假网站 → 钓鱼 |

TLS 三个对策：

| TLS 功能 | 解决 | 怎么做 |
|----------|------|--------|
| **加密** | �窃听 | 协商对称密钥，加密所有应用数据 |
| **MAC/AEAD** | 篡改 | 每条消息带认证标签，改一个字节就失败 |
| **证书** | 冒充 | 服务器出示 CA 签名的证书，证明"我是我" |

### 1.2 TLS 在协议栈中的位置

```
┌─────────────────────────────┐
│         HTTP / RPC          │   ← 应用层
├─────────────────────────────┤
│           TLS               │   ← 加密层（本章新增）
├─────────────────────────────┤
│            TCP              │   ← 传输层
├─────────────────────────────┤
│            IP               │   ← 网络层
├─────────────────────────────┤
│        以太网 / WiFi         │   ← 链路层
└─────────────────────────────┘
```

TLS 是一层"壳"：把 HTTP 的明文包进去，变成密文，再交给 TCP 传输。
接收端从 TCP 拿到密文，TLS 解密还原成明文，交给 HTTP 处理。

从 HTTP 的角度看，底下是 TCP 还是 TLS over TCP 它根本不知道——
**TLS 对应用层透明**（虽然实际上 HTTPS 端口是 443 而不是 80）。

### 1.3 HTTPS = HTTP + TLS

```
HTTP 请求:  GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n
           ↓ TLS 加密
TLS 记录:   [加密后的密文]
           ↓ TCP 传输
网络:       0x17 0x03 0x03 0x00 0x2a [密文...]  ← 抓包只能看到这些
           ↓ TCP 接收
           ↓ TLS 解密
HTTP 请求:  GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n
```

---

## 二、TLS 握手：加密前的"暗号对齐"

TLS 通信之前，双方必须先**握手**——协商加密参数、验证身份、交换密钥。
握手完成后才能开始加密传输应用数据。

### 2.1 TLS 1.2 握手（2-RTT）

TLS 1.2 的握手需要 2 个往返时间（RTT）：

```
Client                              Server
  |                                   |
  |  ── ClientHello ──────────────→  |  RTT 1 开始
  |    (支持的版本、密码套件、随机数)   |
  |                                   |
  |  ←── ServerHello ──────────────  |
  |    (选定的版本、密码套件、随机数)   |
  |  ←── Certificate ──────────────  |
  |    (服务器证书 = 公钥 + CA 签名)    |
  |  ←── ServerKeyExchange ────────  |
  |    (DH 参数)                      |
  |  ←── ServerHelloDone ──────────  |  RTT 1 结束
  |                                   |
  |  ── ClientKeyExchange ────────→  |  RTT 2 开始
  |    (加密的 pre-master-secret)     |
  |  ── ChangeCipherSpec ──────────→  |
  |  ── Finished ─────────────────→  |  RTT 2 结束
  |                                   |
  |  ←── ChangeCipherSpec ─────────  |
  |  ←── Finished ─────────────────  |
  |                                   |
  |  ═══ 加密应用数据 ════════════   |  可以通信了
```

总共 2-RTT 才能开始发数据。如果 RTT = 100ms，握手就要 200ms。

### 2.2 TLS 1.3 握手（1-RTT）

TLS 1.3 大幅简化，只需 1 个往返时间：

```
Client                              Server
  |                                   |
  |  ── ClientHello ──────────────→  |  RTT 1 开始
  |    (版本、密码套件、随机数、        |
  |     DH 公钥——直接带上!)           |
  |                                   |
  |  ←── ServerHello ──────────────  |
  |    (选定的套件、随机数、            |
  |     DH 公钥——直接带上!)           |
  |  ←── Encrypted Extensions ────   |
  |  ←── Certificate ──────────────  |
  |  ←── CertificateVerify ───────   |
  |  ←── Finished ─────────────────  |  RTT 1 结束
  |                                   |
  |  ── Finished ─────────────────→  |
  |                                   |
  |  ═══ 加密应用数据 ════════════   |  可以通信了
```

**关键优化**：ClientHello 里直接带上 DH 公钥（key_share），
ServerHello 也直接带上 DH 公钥——第一个 RTT 就完成了密钥协商。

TLS 1.3 还支持 **0-RTT 恢复**（session resumption）：
如果客户端之前连过，可以在 ClientHello 里直接带上加密数据，
服务器 0-RTT 就能解密——但牺牲了前向安全性。

### 2.3 密钥协商：Diffie-Hellman 交换

TLS 1.3 用 **ECDHE**（椭圆曲线 Diffie-Hellman Ephemeral）协商密钥：

```
Alice (客户端)                     Bob (服务器)
  私钥: a (随机数)                   私钥: b (随机数)
  公钥: A = a·G                     公钥: B = b·G
                                     (G 是椭圆曲线的基点)

  Alice 发 A 给 Bob
  Bob  发 B 给 Alice

  Alice 计算: S = a·B = a·b·G
  Bob  计算: S = b·A = b·a·G

  双方得到相同的 S！S 就是共享密钥。
  窃听者只看到 A 和 B，但无法从 A 推出 a（椭圆曲线离散对数难题）
```

这就是为什么 TLS 能在公开信道上协商出只有双方知道的密钥。

### 2.4 证书：证明"我是我"

DH 交换只解决了密钥协商，但没解决身份验证——
你怎么知道对面是真正的 example.com 而不是中间人？

**证书（Certificate）** 解决这个问题：

```
证书内容:
  ┌─────────────────────────────┐
  │ 颁发给: example.com          │
  │ 公钥:   04 A3 B2 C1 ...      │
  │ 有效期: 2026-01-01 ~ 2027-01-01 │
  │ 颁发者: Let's Encrypt        │  ← CA（证书颁发机构）
  │ 签名:   RSA(Hash(以上内容),   │
  │          CA 的私钥)           │
  └─────────────────────────────┘
```

验证流程：
1. 客户端收到证书
2. 用 CA 的公钥（预装在系统/浏览器里）验证证书签名
3. 签名有效 → 证书确实是 CA 签发的 → 公钥可信
4. 检查域名匹配、有效期
5. 用证书里的公钥加密/验证 DH 交换

**自签名证书**：没有 CA 签名，自己签自己。
教学用没问题，但浏览器不信任会报警。生产环境用 Let's Encrypt 免费签发。

---

## 三、OpenSSL 库

OpenSSL 是最广泛使用的 TLS 库（Nginx、Apache、curl 都用它）。

### 3.1 核心对象

```
SSL_CTX  ── "工厂"（全局唯一）
  │  存放证书、私钥、TLS 版本、密码套件配置
  │
  ├── SSL ── "连接"（每个 TLS 连接一个）
  │    绑定到一个 socket fd
  │    存放握手状态、会话密钥
  │
  ├── SSL ── "连接"
  │
  └── SSL ── "连接"
```

### 3.2 服务器端 API 流程

```c
/* 1. 初始化（全局一次） */
SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM);
SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM);

/* 2. 每个新连接 */
SSL *ssl = SSL_new(ctx);        // 从 CTX 创建 SSL 对象
SSL_set_fd(ssl, client_fd);     // 绑定到 socket
SSL_accept(ssl);                // TLS 握手（阻塞直到完成）

/* 3. 读写数据（自动加密/解密） */
int n = SSL_read(ssl, buf, sizeof(buf));   // 解密后返回明文
SSL_write(ssl, response, len);             // 加密后发送

/* 4. 关闭 */
SSL_shutdown(ssl);              // 发送 close_notify
SSL_free(ssl);                  // 释放 SSL 对象
close(client_fd);               // 关闭 TCP
```

### 3.3 SSL_read / SSL_write vs read / write

```
普通 HTTP:
  read(fd, buf, n)   →  直接从 TCP 读数据
  write(fd, buf, n)  →  直接写 TCP

HTTPS:
  SSL_read(ssl, buf, n)   →  从 TCP 读密文 → 解密 → 返回明文
  SSL_write(ssl, buf, n)  →  加密明文 → 写 TCP
```

从应用层看，SSL_read 和 read 的接口几乎一样——返回读到的字节数。
但内部多了一层加密/解密。

### 3.4 非阻塞 SSL 的复杂性

在非阻塞模式下，SSL_read/SSL_write 的返回值更复杂：

```c
int n = SSL_read(ssl, buf, sizeof(buf));
if (n <= 0) {
    int err = SSL_get_error(ssl, n);
    switch (err) {
    case SSL_ERROR_WANT_READ:   // 需要更多数据才能解密
        // 等 EPOLLIN
        break;
    case SSL_ERROR_WANT_WRITE:  // TLS 需要写（比如发重传）
        // 等 EPOLLOUT
        break;
    case SSL_ERROR_SSL:         // TLS 协议错误
        // 关闭连接
        break;
    case SSL_ERROR_SYSCALL:     // 系统调用错误
        // 检查 errno
        break;
    }
}
```

**为什么 SSL_read 返回 WANT_READ？**
TLS 记录可能跨多个 TCP 包。比如一个 TLS 记录说"我有 1000 字节"，
但 TCP 只到了 600 字节。SSL_read 无法返回解密结果，需要等更多数据。

**为什么 SSL_read 返回 WANT_WRITE？**
TLS 握手或重传可能需要发送数据。比如 SSL_accept 在非阻塞模式下
可能需要发 ServerHello 但 socket 写缓冲满了，就需要等 EPOLLOUT。

本章的教学代码简化了非阻塞处理：握手时临时设为阻塞模式。
生产代码需要完整处理 WANT_READ/WANT_WRITE（参考 Nginx 的 ngx_event_openssl.c）。

---

## 四、代码解读

### 4.1 整体结构

```
tls_server.c 和 phase1/stage7_webserver/webserver.c 的对比:

  webserver.c              tls_server.c
  ───────────              ────────────
  read(fd, ...)      →     SSL_read(ssl, ...)
  write(fd, ...)     →     SSL_write(ssl, ...)
  sendfile(fd, ...)  →     read + SSL_write（SSL 不支持 sendfile）
  close(fd)          →     SSL_shutdown + SSL_free + close(fd)
  (无握手)           →     SSL_accept（TLS 握手）
  (无初始化)         →     SSL_CTX_new + 加载证书
```

核心改动就这五处，其他业务逻辑（路由、HTTP 解析、静态文件）完全不变。

### 4.2 OpenSSL 初始化

```c
static SSL_CTX *tls_init(const char *cert_path, const char *key_path)
{
    // 创建 SSL_CTX，用服务端模式
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());

    // 要求 TLS 1.2 以上（TLS 1.3 优先）
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // 加载证书（公钥 + 证书链）
    SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM);

    // 加载私钥
    SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM);

    // 验证私钥和证书匹配
    SSL_CTX_check_private_key(ctx);

    return ctx;
}
```

**SSL_CTX 是全局共享的**——所有连接共用一个 CTX。
CTX 里存着证书和私钥，每个新连接从 CTX 创建 SSL 对象时自动继承这些配置。

### 4.3 TLS 握手

```c
static SSL *tls_accept(SSL_CTX *ctx, int fd)
{
    SSL *ssl = SSL_new(ctx);       // 创建 SSL 对象
    SSL_set_fd(ssl, fd);           // 绑定到 socket

    // 临时设为阻塞，简化握手
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    SSL_accept(ssl);               // TLS 握手

    // 恢复非阻塞
    fcntl(fd, F_SETFL, flags);

    return ssl;
}
```

**SSL_accept 做了什么？**
1. 等待 ClientHello
2. 选择 TLS 版本和密码套件
3. 发送 ServerHello + 证书 + DH 公钥 + Finished
4. 等待客户端的 Finished
5. 握手完成，可以 SSL_read/SSL_write 了

握手期间会多次读写 socket，所以需要阻塞模式（或完整处理非阻塞）。
教学代码选择"临时阻塞"简化，握手完成后恢复非阻塞。

### 4.4 连接结构体

```c
typedef struct {
    int            fd;
    SSL           *ssl;       // ← 新增：TLS 连接对象
    http_parser_t  parser;
} conn_t;
```

和 phase1 相比只多了一个 `SSL *ssl` 字段。
所有读写操作通过 `conn->ssl` 而不是 `conn->fd`。

### 4.5 读写替换

```c
// phase1 (HTTP):
ssize_t nread = read(fd, buf, sizeof(buf));

// phase2 (HTTPS):
int nread = SSL_read(conn->ssl, buf, sizeof(buf));
if (nread <= 0) {
    int err = SSL_get_error(conn->ssl, nread);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        break;  // 非阻塞，等下次事件
    // 真正的错误
    tls_close(conn);
    ...
}
```

SSL_read 返回值的处理比 read 复杂：
- `> 0`：读到了 nread 字节明文
- `0`：对端关闭
- `< 0`：需要用 SSL_get_error 判断具体原因

### 4.6 发送文件：sendfile 的替代

```c
// phase1 (HTTP) —— 零拷贝:
sendfile(fd, file_fd, &offset, st.st_size);
// 磁盘 → 内核缓冲 → 网卡，数据不经过用户空间

// phase2 (HTTPS) —— 不能用 sendfile:
char buf[FILE_BUF];
while ((n = read(file_fd, buf, sizeof(buf))) > 0) {
    ssl_write_all(ssl, buf, n);  // 加密后发送
}
// 磁盘 → 内核缓冲 → 用户缓冲(read) → OpenSSL加密 → 内核 → 网卡
```

**为什么 SSL 不支持 sendfile？**

sendfile 在内核里直接把文件数据搬到 socket，不经过用户空间。
但 TLS 加密必须由 OpenSSL 库在用户空间完成——内核看不到加密后的数据。

这是 HTTPS 比 HTTP 慢的**核心原因之一**：
- HTTP：sendfile 零拷贝，2 次拷贝
- HTTPS：read + SSL_write，4+ 次拷贝 + 加密计算

### 4.7 关闭连接

```c
static void tls_close(conn_t *conn)
{
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);  // 发送 close_notify 警告
        SSL_free(conn->ssl);      // 释放 SSL 对象
        conn->ssl = NULL;
    }
    close(conn->fd);              // 关闭 TCP
}
```

**SSL_shutdown** 发送 TLS close_notify 警告，通知对端"我要关了"。
这是优雅关闭——对端知道是正常关闭，不是网络中断。

如果不调 SSL_shutdown 直接 close(fd)，对端 SSL_read 会报错
`SSL_ERROR_SSL`（连接被中断），可能丢失最后几条消息。

---

## 五、证书生成

### 5.1 自签名证书（教学用）

```bash
# 生成 RSA 私钥（2048 位）
openssl genrsa -out key.pem 2048

# 生成自签名证书（365 天有效）
openssl req -new -x509 -key key.pem -out cert.pem -days 365 \
  -subj "/C=CN/ST=Teaching/L=WSL2/O=tiny_server/CN=localhost"
```

证书内容：
```
subject: C=CN, ST=Teaching, L=WSL2, O=tiny_server, CN=localhost
issuer:  C=CN, ST=Teaching, L=WSL2, O=tiny_server, CN=localhost
```

subject 和 issuer 相同 = 自签名（自己签自己）。
浏览器不信任自签名证书，会显示"您的连接不是私密连接"。
用 `curl -k` 跳过验证。

### 5.2 生产证书（Let's Encrypt）

```bash
# 安装 certbot
sudo apt install certbot

# 申请证书（需要有域名 + 80 端口可用）
sudo certbot certonly --standalone -d example.com

# 证书位置
# /etc/letsencrypt/live/example.com/fullchain.pem  ← 证书
# /etc/letsencrypt/live/example.com/privkey.pem    ← 私钥
```

Let's Encrypt 免费签发受信任的证书，90 天有效，自动续期。

### 5.3 证书文件格式

```
cert.pem (PEM 格式):
-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIQA...
... Base64 编码的 DER 数据 ...
-----END CERTIFICATE-----

key.pem (PEM 格式):
-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0...
... Base64 编码的 DER 数据 ...
-----END PRIVATE KEY-----
```

PEM 就是 Base64 编码的 DER 加上头尾标记。
OpenSSL 的 `SSL_FILETYPE_PEM` 表示用 PEM 格式加载。

---

## 六、HTTPS vs HTTP 性能

### 6.1 延迟对比

```
HTTP 请求延迟:
  TCP 握手: 1-RTT
  HTTP 请求+响应: 1-RTT
  总计: 2-RTT

HTTPS 请求延迟 (TLS 1.2):
  TCP 握手: 1-RTT
  TLS 握手: 2-RTT
  HTTP 请求+响应: 1-RTT
  总计: 4-RTT

HTTPS 请求延迟 (TLS 1.3):
  TCP 握手: 1-RTT
  TLS 握手: 1-RTT
  HTTP 请求+响应: 1-RTT
  总计: 3-RTT

HTTPS 请求延迟 (TLS 1.3 + 0-RTT):
  TCP 握手: 1-RTT
  TLS 握手 + HTTP 请求: 0-RTT（数据随 ClientHello 发出）
  总计: 1-RTT（但牺牲前向安全性）
```

如果 RTT = 100ms：
- HTTP: 200ms
- HTTPS TLS 1.2: 400ms（多了 200ms）
- HTTPS TLS 1.3: 300ms（多了 100ms）

### 6.2 吞吐量对比

```
HTTP:
  sendfile 零拷贝，CPU 几乎不参与
  瓶颈在网卡带宽

HTTPS:
  read + SSL_write，每次都要加密
  AES-256-GCM 加密 ~3 GB/s（硬件加速）
  瓶颈在 CPU 加密速度
```

现代 CPU 有 AES-NI 指令集，加密很快。
但仍然比 sendfile 慢 2-5 倍。

### 6.3 优化手段

| 优化 | 效果 | 说明 |
|------|------|------|
| TLS 1.3 | -1 RTT | 握手从 2-RTT 降到 1-RTT |
| Session Resumption | -1 RTT | 复用之前的会话，0-RTT |
| HTTP/2 多路复用 | 省连接 | 一个 TLS 连接跑多个请求 |
| AES-NI 硬件加速 | 5-10x 吞吐 | CPU 指令集加速 AES |
| OCSP Stapling | -1 RTT | 证书状态检查内嵌在握手里 |
| False Start | -1 RTT | 握手没完就提前发数据（TLS 1.2） |

---

## 七、非阻塞 SSL 的完整处理

本章教学代码在握手时临时设为阻塞模式，简化了实现。
生产代码需要完整处理非阻塞 SSL。

### 7.1 非阻塞握手

```c
int do_tls_handshake(SSL *ssl) {
    int ret = SSL_accept(ssl);
    if (ret > 0) return 1;  // 握手完成

    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        // 需要 EPOLLIN，等下次读事件
        return 0;  // 握手未完成，继续等
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        // 需要 EPOLLOUT，修改 epoll 事件
        // epoll_ctl(epfd, EPOLL_CTL_MOD, fd, EPOLLOUT);
        return 0;  // 握手未完成，继续等
    }
    return -1;  // 握手失败
}
```

握手可能需要多次 epoll 事件才能完成。
连接状态需要记录"正在握手"：

```c
typedef enum {
    CONN_HANDSHAKING,  // TLS 握手中
    CONN_READING,      // 读 HTTP 请求
    CONN_WRITING,      // 写 HTTP 响应
} conn_state_t;

typedef struct {
    int          fd;
    SSL         *ssl;
    conn_state_t state;  // ← 记录当前状态
    http_parser_t parser;
} conn_t;
```

### 7.2 非阻塞读写

```c
int do_ssl_read(conn_t *conn) {
    int n = SSL_read(conn->ssl, buf, sizeof(buf));
    if (n > 0) return n;  // 读到数据

    int err = SSL_get_error(conn->ssl, n);
    if (err == SSL_ERROR_WANT_READ) {
        // 等下次 EPOLLIN
        return 0;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        // SSL 需要写（比如发重传）
        // 切换到 EPOLLOUT
        return 0;
    }
    return -1;  // 错误
}
```

### 7.3 为什么 SSL_read 可能返回 WANT_WRITE？

TLS 协议在数据传输中可能需要发送控制消息：
- 重传丢失的 TLS 记录
- 发送 key update（密钥更新）
- 发送 close_notify

这些都需要写 socket。如果 socket 写缓冲满了，SSL_read 就会返回 WANT_WRITE，
表示"我需要写东西才能继续读"。

这是 SSL 和普通 socket 最大的区别——**读写不再独立**。

---

## 八、TLS 记录层

TLS 不是把数据加密了直接发，而是封装成**记录（Record）**：

```
TLS 记录格式:
┌──────────┬──────────┬──────────────┬─────────────────┐
│ type (1) │ ver (2)  │ length (2)   │ payload (变长)  │
└──────────┴──────────┴──────────────┴─────────────────┘

type:
  0x16 = Handshake（握手消息）
  0x17 = Application Data（应用数据，加密的）
  0x15 = Alert（警告，如 close_notify）
  0x14 = Change Cipher Spec（切换密码套件，TLS 1.2）

payload:
  握手阶段 = 明文的握手消息
  应用数据 = 密文 + AEAD 认证标签
```

抓包看到的 TLS 数据：

```
16 03 03 00 7a   ← Handshake, TLS 1.2, length=122
  01 00 00 76    ← ClientHello, length=118
  03 03          ← 版本 TLS 1.2
  a4 b3 c1 ...   ← 客户端随机数 (32 字节)
  ...

17 03 03 00 2a   ← Application Data, TLS 1.2, length=42
  8f 2a b1 ...   ← 加密后的密文
```

**0x17 = Application Data** 是加密后的数据。
抓包只能看到密文，看不到 HTTP 请求内容——这就是 TLS 的意义。

---

## 九、密码套件

TLS 1.3 的密码套件命名格式：
```
TLS_<密钥交换>_<认证>_<加密>_<MAC>
```

本章测试结果：`TLS_AES_256_GCM_SHA384`

| 组件 | 值 | 说明 |
|------|-----|------|
| 密钥交换 | ECDHE | 椭圆曲线 DH 临时密钥（前向安全） |
| 认证 | RSA-PSS | RSA 概率签名方案 |
| 加密 | AES-256-GCM | AES 256 位 + GCM 模式 |
| MAC | SHA384 | GCM 自带认证，SHA384 用于握手 |

**AEAD（Authenticated Encryption with Associated Data）**：
加密和认证一步完成——密文带一个认证标签，
改一个字节标签就不匹配，防止篡改。

TLS 1.3 支持的密码套件（只有 5 个，比 TLS 1.2 的几十个精简很多）：
```
TLS_AES_256_GCM_SHA384
TLS_CHACHA20_POLY1305_SHA256
TLS_AES_128_GCM_SHA256
TLS_AES_128_CCM_SHA256
TLS_AES_128_CCM_8_SHA256
```

---

## 十、运行与测试

### 10.1 完整流程

```bash
# 1. 编译
cmake -B build -S .
cmake --build build --target tls_server

# 2. 生成自签名证书
bash phase2/stage8_tls/gen_cert.sh

# 3. 启动 HTTPS 服务器
build/bin/tls_server 8443 4 ./phase2/www \
  phase2/stage8_tls/certs/cert.pem \
  phase2/stage8_tls/certs/key.pem

# 4. 测试
curl -k https://localhost:8443/           # 首页
curl -k https://localhost:8443/api/test   # API
curl -k https://localhost:8443/test.txt   # 静态文件
```

### 10.2 查看握手详情

```bash
curl -kv https://localhost:8443/ 2>&1 | grep -E 'TLS|SSL|cipher|subject'
```

输出：
```
* TLSv1.3 (OUT), TLS handshake, Client hello (1):
* TLSv1.3 (IN), TLS handshake, Server hello (2):
* TLSv1.3 (IN), TLS handshake, Encrypted Extensions (8):
* TLSv1.3 (IN), TLS handshake, Certificate (11):
* TLSv1.3 (IN), TLS handshake, CERT verify (15):
* TLSv1.3 (IN), TLS handshake, Finished (20):
* TLSv1.3 (OUT), TLS handshake, Finished (20):
* SSL connection using TLSv1.3 / TLS_AES_256_GCM_SHA384 / X25519 / RSASSA-PSS
```

可以看到 TLS 1.3 握手只用了 1-RTT（Client hello → Server hello + ... → Finished）。

### 10.3 用 openssl s_client 查看握手

```bash
openssl s_client -connect localhost:8443 -showcerts
```

这会打印完整的握手过程和证书链，比 curl 更详细。

### 10.4 压测对比

```bash
# HTTP (phase1)
build/bin/webserver 8080 4 ./phase1/www &
wrk -c100 -t4 -d10s http://localhost:8080/

# HTTPS (phase2)
build/bin/tls_server 8443 4 ./phase2/www \
  phase2/stage8_tls/certs/cert.pem \
  phase2/stage8_tls/certs/key.pem &
wrk -c100 -t4 -d10s https://localhost:8443/
```

HTTPS 的 QPS 通常是 HTTP 的 1/3 到 1/2（加密开销 + 不能用 sendfile）。

---

## 十一、常见问题

### Q1: 为什么浏览器显示"不安全"？

因为用的是自签名证书，没有 CA 签名。
浏览器只信任系统/浏览器预装的 CA 列表里的 CA 签发的证书。
解决：用 Let's Encrypt 签发受信任的证书，或把自签名证书加入系统信任列表。

### Q2: SSL_accept 一直阻塞怎么办？

可能是客户端没发 ClientHello（比如用 HTTP 连了 HTTPS 端口）。
生产代码应该给 SSL_accept 加超时（用 epoll + timeout）。

### Q3: 为什么 SSL_read 返回 0 但连接没断？

SSL_read 返回 0 通常表示对端发了 close_notify（正常关闭）。
但也可能是 SSL 重协商（renegotiation）的边界——检查 SSL_get_error。

### Q4: 能不能同时支持 HTTP 和 HTTPS？

可以。监听两个端口（80 和 443），80 走普通 HTTP，443 走 TLS。
或者用 ALPN（Application-Layer Protocol Negotiation）在 TLS 握手时协商协议。

### Q5: TLS 1.3 为什么删了 RSA 密钥交换？

RSA 密钥交换没有前向安全性（Forward Secrecy）：
如果服务器的 RSA 私钥泄露，所有之前的通信都能解密。
ECDHE 每次握手用新的临时密钥，私钥泄露不影响之前的通信。

---

## 十二、本章总结

### 学到了什么

1. **TLS 在协议栈中的位置**：TCP 之上、HTTP 之下的加密层
2. **TLS 握手流程**：ClientHello → ServerHello + 证书 → 密钥协商 → Finished
3. **Diffie-Hellman 密钥交换**：在公开信道协商共享密钥
4. **证书和 PKI**：CA 签名证明身份，建立信任链
5. **OpenSSL API**：SSL_CTX → SSL → SSL_accept → SSL_read/write
6. **SSL vs POSIX IO**：SSL_read/write 多了加密/解密层
7. **sendfile 不可用**：SSL 加密必须在用户空间完成
8. **非阻塞 SSL**：WANT_READ/WANT_WRITE 的处理
9. **TLS 1.3 优化**：1-RTT 握手、0-RTT 恢复、删掉不安全套件
10. **性能影响**：HTTPS 比 HTTP 慢 2-5 倍，但 AES-NI 可大幅缓解

### 代码改动量

和 phase1/stage7_webserver 相比：
- 新增 ~150 行（OpenSSL 初始化、握手、SSL 读写封装）
- 修改 ~20 行（read→SSL_read, write→SSL_write, close→tls_close）
- 删除 ~10 行（sendfile 替换为 read+SSL_write）
- **业务逻辑零改动**（路由、HTTP 解析、静态文件处理不变）

这体现了良好的架构设计：IO 层和业务层分离，换 IO 模型不影响业务。

### 下一站

下一章我们做 **io_uring**——Linux 5.1+ 的异步 IO 接口。
epoll 本质还是"同步等就绪 + 同步读写"，io_uring 是真正的异步：
提交读写请求，内核完成后通知你，全程不阻塞。

io_uring 是 Linux IO 的未来，Redis、Nginx 都在逐步支持。

---

## 十三、TLS 错误处理详解

### 13.1 SSL_get_error 错误码

SSL_read/SSL_write/SSL_accept 返回 <= 0 时，必须用 SSL_get_error 判断原因：

```c
int ret = SSL_read(ssl, buf, sizeof(buf));
if (ret <= 0) {
    int err = SSL_get_error(ssl, ret);
    // err 的可能值：
}
```

| 错误码 | 含义 | 处理方式 |
|--------|------|----------|
| `SSL_ERROR_NONE` | 实际上不会出现（ret > 0 时才正常） | - |
| `SSL_ERROR_WANT_READ` | 需要读更多数据才能完成操作 | 注册 EPOLLIN，等下次事件 |
| `SSL_ERROR_WANT_WRITE` | 需要写数据才能完成操作 | 注册 EPOLLOUT，等下次事件 |
| `SSL_ERROR_SYSCALL` | 系统调用错误 | 检查 errno，通常关闭连接 |
| `SSL_ERROR_SSL` | TLS 协议错误 | 关闭连接，记录日志 |
| `SSL_ERROR_ZERO_RETURN` | 对端发了 close_notify | 正常关闭连接 |
| `SSL_ERROR_WANT_CONNECT` | 非阻塞 connect 未完成 | 继续等连接完成 |
| `SSL_ERROR_WANT_ACCEPT` | 非阻塞 accept 未完成 | 继续等新连接 |

### 13.2 WANT_READ / WANT_WRITE 的本质

普通 socket：read 和 write 是独立的——read 不需要写，write 不需要读。

SSL socket：read 和 write **可能互相依赖**：

```
SSL_read 想读数据
  → TLS 层需要先发一个重传消息
  → write 会阻塞（写缓冲满）
  → SSL_read 返回 WANT_WRITE
  → 你需要等 EPOLLOUT
  → 然后继续调 SSL_read（不是 SSL_write！）
```

反过来也一样：
```
SSL_write 想写数据
  → TLS 层需要先收一个握手消息（比如重协商）
  → read 会阻塞（数据没到）
  → SSL_write 返回 WANT_READ
  → 你需要等 EPOLLIN
  → 然后继续调 SSL_write
```

这就是为什么非阻塞 SSL 代码比普通非阻塞 IO 复杂得多。

### 13.3 错误处理代码模板

```c
/* 非阻塞 SSL_read 完整处理 */
int ssl_read_nb(SSL *ssl, void *buf, int len, int epfd, int fd) {
    int n = SSL_read(ssl, buf, len);
    if (n > 0) return n;  /* 成功读到数据 */

    int err = SSL_get_error(ssl, n);
    switch (err) {
    case SSL_ERROR_WANT_READ: {
        /* 需要更多数据，注册 EPOLLIN */
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = conn;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
        return 0;  /* 还没读完，下次继续 */
    }
    case SSL_ERROR_WANT_WRITE: {
        /* SSL 需要写（重传等），注册 EPOLLOUT */
        struct epoll_event ev;
        ev.events = EPOLLOUT | EPOLLET;
        ev.data.ptr = conn;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
        return 0;
    }
    case SSL_ERROR_ZERO_RETURN:
        /* 对端正常关闭 */
        return -1;
    case SSL_ERROR_SYSCALL:
        /* 系统调用错误 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 非阻塞，数据没到 */
            return 0;
        }
        /* 真正的错误 */
        log_error("SSL_read syscall error: %s", strerror(errno));
        return -1;
    case SSL_ERROR_SSL:
        /* TLS 协议错误 */
        log_error("SSL_read protocol error");
        ERR_print_errors_fp(stderr);
        return -1;
    default:
        log_error("SSL_read unknown error: %d", err);
        return -1;
    }
}
```

---

## 十四、Session Resumption（会话恢复）

### 14.1 为什么需要会话恢复

首次 TLS 1.3 握手：1-RTT
如果复用之前的会话：**0-RTT**——数据随 ClientHello 一起发出！

这对延迟敏感的场景（比如移动端）非常重要。

### 14.2 TLS 1.3 Session Ticket

```
首次连接:
  Client ── ClientHello (无 ticket) ──→ Server
  Client ←── ServerHello + ... + NewSessionTicket ── Server
  （服务器发一个 Session Ticket，里面加密了会话信息）

再次连接:
  Client ── ClientHello + ticket + 早期数据 ──→ Server
  （客户端用 ticket 恢复会话，同时带上加密的 HTTP 请求）
  Client ←── ServerHello + ... + HTTP 响应 ── Server
  （0-RTT！服务器直接处理了早期数据）
```

### 14.3 OpenSSL 启用 Session Ticket

```c
/* 启用 session ticket（默认开启） */
SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);

/* 设置 ticket key（多服务器间共享，保证 ticket 可解密） */
unsigned char keys[16];
RAND_bytes(keys, sizeof(keys));
SSL_CTX_set_tlsext_ticket_key_cb(ctx, ticket_key_cb);
```

### 14.4 0-RTT 的安全风险

0-RTT 早期数据是**重放攻击**的潜在目标——
攻击者截获 ClientHello + 早期数据，重放给服务器，服务器会重复执行。

所以 0-RTT 只应该用于**幂等请求**（GET），不能用于 POST/PUT。
OpenSSL 用 `SSL_CTX_set_max_early_data()` 控制允许的早期数据大小。

---

## 十五、ALPN（应用层协议协商）

### 15.1 什么是 ALPN

ALPN（Application-Layer Protocol Negotiation）让客户端在 TLS 握手时
告诉服务器它想用什么应用协议（HTTP/1.1、HTTP/2、SPDY 等）。

```
Client ── ClientHello + ALPN["h2", "http/1.1"] ──→ Server
Client ←── ServerHello + ALPN["h2"] ────────────── Server
```

握手完成后，双方就知道用什么应用协议，不用再额外协商。

### 15.2 OpenSSL ALPN API

```c
/* 服务器端：设置支持的协议 */
static int alpn_select_cb(SSL *ssl, const unsigned char **out,
                          unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen,
                          void *arg) {
    /* 优先选 HTTP/2，回退 HTTP/1.1 */
    if (SSL_select_next_proto(out, outlen,
            (const unsigned char *)"\x02h2\x08http/1.1", 12,
            in, inlen) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);
```

### 15.3 curl 查看 ALPN

```bash
curl -kv https://localhost:8443/ 2>&1 | grep ALPN
# * ALPN: server did not agree on a protocol. Uses default.
```

本章代码没设 ALPN，所以 curl 报 "did not agree"。
加了 ALPN 之后就能在同一个 TLS 连接上协商 HTTP/2 了。

---

## 十六、调试 TLS

### 16.1 openssl s_client

最强大的 TLS 调试工具：

```bash
# 连接并打印握手详情
openssl s_client -connect localhost:8443

# 指定 TLS 版本
openssl s_client -connect localhost:8443 -tls1_3

# 指定密码套件
openssl s_client -connect localhost:8443 -cipher 'AES256-GCM-SHA384'

# 显示证书链
openssl s_client -connect localhost:8443 -showcerts

# 手动发 HTTP 请求
echo -e "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n" | \
  openssl s_client -connect localhost:8443 -quiet
```

### 16.2 用 tcpdump 抓 TLS 包

```bash
# 抓 8443 端口的包
sudo tcpdump -i lo port 8443 -w tls.pcap

# 用 Wireshark 打开，过滤 tls
# 可以看到：
#   TLS Client Hello
#   TLS Server Hello
#   TLS Certificate
#   TLS Application Data（密文）
```

### 16.3 OpenSSL 错误队列

```c
/* 打印 OpenSSL 错误栈 */
void print_ssl_errors() {
    unsigned long err;
    while ((err = ERR_get_error())) {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        fprintf(stderr, "OpenSSL error: %s\n", buf);
    }
}

/* 在出错的地方调用 */
if (SSL_accept(ssl) <= 0) {
    print_ssl_errors();
}
```

### 16.4 常见错误

| 错误 | 原因 | 解决 |
|------|------|------|
| `SSL_ERROR_SSL` | 协议错误 | 客户端用了不支持的版本/套件 |
| `certificate verify failed` | 证书不受信任 | 用 -k 跳过，或安装 CA |
| `no shared cipher` | 没有共同的密码套件 | 检查 SSL_CTX_set_cipher_list |
| `key values mismatch` | 证书和私钥不配 | 重新生成证书 |
| `handshake failure` | 握手失败 | 用 s_client 调试 |

---

## 十七、从教学到生产

本章代码做了很多简化，生产代码还需要：

### 17.1 安全加固

```c
/* 禁用不安全的版本和套件 */
SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
SSL_CTX_set_cipher_list(ctx,
    "ECDHE+AESGCM:ECDHE+CHACHA20:!DSS:!RSA:!AESCCM");

/* 启用 OCSP Stapling（证书状态在线检查） */
SSL_CTX_set_tlsext_status_type(ctx, TLSEXT_STATUSTYPE_ocsp);

/* 启用 HSTS（HTTP Strict Transport Security） */
// 在响应头加: Strict-Transport-Security: max-age=31536000

/* 限制早期数据（0-RTT）只用于幂等请求 */
SSL_CTX_set_max_early_data(ctx, 0);  // 或限制大小
```

### 17.2 性能优化

```c
/* 启用 Session Cache */
SSL_CTX_set_session_cache_mode(ctx,
    SSL_SESS_CACHE_SERVER | SSL_SESS_CACHE_NO_AUTO_CLEAR);

/* 启用 TLS 1.3 Session Ticket */
SSL_CTX_set_num_tickets(ctx, 2);  // 发 2 个 ticket

/* 用 ECDHE 临时密钥复用（省 1 次 DH 计算） */
SSL_CTX_set_ecdh_auto(ctx, 1);
```

### 17.3 多进程共享 Session

如果用多进程（fork），子进程不共享 SSL_CTX 的 session cache。
需要用外部 session cache（Redis / memcached）或共享内存。

Nginx 用共享内存 + slab allocator 实现 worker 间共享 session。

### 17.4 证书热更新

```c
/* 信号处理：SIGHUP 时重新加载证书 */
void reload_cert(int sig) {
    SSL_CTX *new_ctx = tls_init("new_cert.pem", "new_key.pem");
    if (new_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = new_ctx;
        log_info("证书已重新加载");
    }
}
signal(SIGHUP, reload_cert);
```

这样不重启进程就能更新证书（Let's Encrypt 续期后）。

---

## 十八、完整代码文件

本章代码在 `phase2/stage8_tls/`：

```
phase2/stage8_tls/
├── tls_server.c     # HTTPS 服务器（~400 行）
├── gen_cert.sh      # 自签名证书生成脚本
├── CMakeLists.txt   # 构建配置
└── certs/           # 证书目录（gen_cert.sh 生成）
    ├── cert.pem     # 自签名证书
    └── key.pem      # RSA 私钥
```

### 18.1 编译依赖

```bash
# Ubuntu/Debian
sudo apt install libssl-dev

# CMakeLists.txt 关键配置
target_link_libraries(tls_server ssl crypto)
```

### 18.2 和 phase1 的代码对比

| 模块 | phase1 (HTTP) | phase2 (HTTPS) | 变化 |
|------|---------------|----------------|------|
| 初始化 | 无 | tls_init() | +30 行 |
| 握手 | 无 | tls_accept() | +25 行 |
| 读 | read() | SSL_read() | 改 5 行 |
| 写 | write() | ssl_write_all() | +20 行 |
| 发文件 | sendfile() | read + SSL_write | +10 行 |
| 关闭 | close() | tls_close() | +8 行 |
| 连接结构 | {fd, parser} | {fd, ssl, parser} | +1 字段 |
| **总计** | | | **+~100 行** |

业务逻辑（路由、HTTP 解析、MIME、404/403）**零改动**。