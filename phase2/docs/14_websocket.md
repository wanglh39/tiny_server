# stage12 - WebSocket 协议与实现

## 本章导读

在 phase1 的 stage7，我们实现了一个完整的 HTTP Web 服务器。HTTP 是请求-响应模型：
客户端发请求，服务器回响应，然后连接关闭（HTTP/1.0）或复用（HTTP/1.1 Keep-Alive）。

但很多场景需要**服务器主动推送**数据给客户端：聊天室、实时通知、股票行情、在线协作……
HTTP 的请求-响应模型做这些很别扭——你得用长轮询（long polling）或 SSE（Server-Sent Events），
都是"曲线救国"。

**WebSocket**（RFC 6455，2011 年）就是为这个设计的：
**一条 TCP 连接上，双方随时互发消息，全双工通信。**

```bash
cmake --build build --target ws_server
build/bin/ws_server 8080
# 用浏览器打开 ws://localhost:8080/ 或 wscat -c ws://localhost:8080/
```

---

## 一、为什么需要 WebSocket

### 1.1 HTTP 的痛点

HTTP 是"客户端问，服务器答"的半双工协议。服务器有新数据时，没法主动推给客户端。

**方案 1：轮询（Polling）**

客户端每隔 1 秒发一个 HTTP 请求："有新消息吗？" 服务器回答"没有"或"有，这是数据"。

```
客户端: GET /messages ──→ 服务器
客户端: ←── 200 OK (空)   服务器
(1 秒后)
客户端: GET /messages ──→ 服务器
客户端: ←── 200 OK (3条)  服务器
```

问题：
- 大量空请求，浪费带宽和服务器资源
- 1 秒的延迟——实时性差
- 每次请求都带完整 HTTP 头部（Cookie、User-Agent 等），开销大

**方案 2：长轮询（Long Polling）**

客户端发请求，服务器**不立即回复**，而是 hold 住连接，有数据时才回复。
客户端收到回复后立即再发下一个请求。

```
客户端: GET /messages ──→ 服务器 (hold)
(5 秒后，有新消息)
客户端: ←── 200 OK (1条)  服务器
客户端: GET /messages ──→ 服务器 (hold)  // 立即发下一个
```

问题：
- 每次回复后要重新建 HTTP 请求，头部开销
- 服务器要维护大量"hold 住"的连接，每个连接占一个线程/协程
- 本质上还是半双工——客户端发消息时要先断开长轮询连接

**方案 3：SSE（Server-Sent Events）**

服务器单向推送，客户端只能听不能说。适合通知、股票行情，不适合聊天。

### 1.2 WebSocket 的解法

WebSocket 的思路极其简洁：

1. **用 HTTP 做握手**：客户端发一个 HTTP 请求，带 `Upgrade: websocket` 头
2. **服务器回 101**：同意升级，这条 TCP 连接从此不再是 HTTP
3. **双方自由通信**：在 TCP 上用 WebSocket 帧格式互发消息

```
客户端 ──HTTP Upgrade──→ 服务器
客户端 ←─── 101 ──────── 服务器
    ═══ WebSocket 双向通信 ═══
客户端 ──帧──→ 服务器
客户端 ←──帧── 服务器
客户端 ←──帧── 服务器    (服务器主动推!)
客户端 ──帧──→ 服务器
```

优势：
- **全双工**：双方随时发数据，不用等对方
- **低开销**：帧头只有 2-14 字节，比 HTTP 头部小几个数量级
- **一条连接**：握手后复用 TCP 连接，不用反复建连
- **二进制友好**：可以发文本也可以发二进制数据

### 1.3 WebSocket vs HTTP 对比

| 特性         | HTTP/1.1           | WebSocket          |
| ------------ | ------------------- | ------------------- |
| 通信模式     | 请求-响应（半双工） | 全双工              |
| 服务器推送   | 不支持（需轮询）    | 原生支持            |
| 头部开销     | 每次请求几百字节    | 2-14 字节           |
| 连接生命周期 | 短连接或 Keep-Alive | 长连接，握手后永久  |
| 数据格式     | 文本（HTTP 头）     | 文本或二进制        |
| 协议层       | 应用层              | 应用层（基于 TCP）  |
| 标准化       | RFC 7230            | RFC 6455            |

---

## 二、WebSocket 握手

### 2.1 客户端请求

WebSocket 连接以一个 HTTP 请求开始。这个请求和普通 HTTP 请求几乎一样，
但多了几个头：

```http
GET /chat HTTP/1.1
Host: localhost:8080
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
Origin: http://localhost:3000
```

逐行解释：

- `GET /chat`：请求路径，可以是任意路径，如 `/chat`、`/ws`、`/socket.io`
- `Upgrade: websocket`：要求协议升级到 WebSocket
- `Connection: Upgrade`：指示这是一个升级请求
- `Sec-WebSocket-Key`：客户端生成的随机 Base64 编码字符串（16 字节编码后 24 字符）
  服务器用这个值计算握手响应
- `Sec-WebSocket-Version: 13`：WebSocket 协议版本（RFC 6455 是版本 13）
- `Origin`：浏览器自动添加，用于安全检查（防止跨站 WebSocket 劫持）

### 2.2 服务器响应

服务器收到握手请求后，验证关键头部，然后返回 101 响应：

```http
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

- `101 Switching Protocols`：状态码 101 表示协议切换
- `Sec-WebSocket-Accept`：服务器用客户端的 Key 计算出的值
  客户端验证这个值是否正确，确保服务器理解 WebSocket 协议

### 2.3 Sec-WebSocket-Accept 的计算

这是握手的核心。RFC 6455 规定的算法：

```
1. 取客户端的 Sec-WebSocket-Key 值
2. 拼接固定 GUID："258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
3. 对拼接结果计算 SHA-1 摘要（20 字节）
4. 对 SHA-1 摘要做 Base64 编码
5. 得到 Sec-WebSocket-Accept 值
```

为什么要有这个计算？

- **证明服务器理解 WebSocket**：只有知道这个算法的服务器才能算出正确的值
- **防止误升级**：普通 HTTP 服务器不知道这个算法，不会返回正确的 Accept
- **GUID 是魔法数字**：`258EAFA5-E914-47DA-95CA-C5AB0DC85B11` 是 RFC 6455 规定的，
  没有特殊含义，就是个"暗号"

用代码验证：

```c
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static char *compute_accept_key(const char *client_key)
{
    /* 1. 拼接 Key + GUID */
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", client_key, WS_GUID);

    /* 2. SHA1 */
    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)combined, strlen(combined), sha1_hash);

    /* 3. Base64 */
    return base64_encode(sha1_hash, SHA_DIGEST_LENGTH);
}
```

手动验证：

```bash
# 客户端 Key: dGhlIHNhbXBsZSBub25jZQ==
echo -n "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11" \
  | sha1sum \
  | awk '{print $1}' \
  | xxd -r -p \
  | base64
# 输出: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

### 2.4 Base64 编码

> **为什么手动实现 Base64？**
>
> OpenSSL 3.0 的 BIO API（`BIO_f_base64()` 等）有兼容问题：
> `BIO_f_base64()` 在某些版本中隐式声明返回 `int` 而非指针，
> 导致段错误。手动实现更可靠，也更适合教学。

Base64 把 3 字节（24 位）数据编码成 4 个 ASCII 字符：

```
原始数据:  [byte0]    [byte1]    [byte2]
           aaaaaaaa   bbbbbbbb   cccccccc
编码后:    [aaaaaa]  [aabbbb]  [bbbbcc]  [cccccc]
             ↑6位       ↑6位      ↑6位      ↑6位
           → 查表 →   → 查表 →  → 查表 →  → 查表 →
```

Base64 码表：

```c
static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
```

编码过程：

```c
static char *base64_encode(const unsigned char *data, size_t len)
{
    size_t out_len = 4 * ((len + 2) / 3);
    char *result = malloc(out_len + 1);

    int j = 0;
    for (size_t i = 0; i < len;) {
        unsigned int octet_a = i < len ? data[i++] : 0;
        unsigned int octet_b = i < len ? data[i++] : 0;
        unsigned int octet_c = i < len ? data[i++] : 0;
        unsigned int triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        result[j++] = base64_table[(triple >> 18) & 0x3F];
        result[j++] = base64_table[(triple >> 12) & 0x3F];
        result[j++] = base64_table[(triple >> 6) & 0x3F];
        result[j++] = base64_table[triple & 0x3F];
    }

    /* 不足 3 字节的用 '=' 填充 */
    for (size_t i = 0; i < (3 - len % 3) % 3; i++) {
        result[out_len - 1 - i] = '=';
    }
    result[out_len] = '\0';

    return result;
}
```

举例：SHA-1 输出 20 字节，20 / 3 = 6 余 2，所以编码后 28 字符（最后 1 个 `=`）。

### 2.5 握手实现

服务器收到 HTTP 请求后，提取 `Sec-WebSocket-Key`，计算 Accept，返回 101：

```c
static int do_handshake(int fd, char *request, int req_len)
{
    /* 提取 Sec-WebSocket-Key */
    char *key_start = strstr(request, "Sec-WebSocket-Key: ");
    if (!key_start) {
        log_error("缺少 Sec-WebSocket-Key");
        return -1;
    }
    key_start += strlen("Sec-WebSocket-Key: ");
    char *key_end = strstr(key_start, "\r\n");
    if (!key_end) return -1;

    char client_key[128];
    int key_len = key_end - key_start;
    memcpy(client_key, key_start, key_len);
    client_key[key_len] = '\0';

    /* 计算 Accept Key */
    char *accept_key = compute_accept_key(client_key);

    /* 构造 101 响应 */
    char response[512];
    int resp_len = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept_key);

    free(accept_key);

    /* 发送握手响应 */
    write(fd, response, resp_len);
    log_info("WebSocket 握手完成 fd=%d", fd);
    return 0;
}
```

握手完成后，这条 TCP 连接就"升级"为 WebSocket 连接了。
之后双方不再用 HTTP 格式通信，而是用 **WebSocket 帧**格式。

---

## 三、WebSocket 帧格式

握手完成后，双方用帧（frame）通信。帧是 WebSocket 的基本数据单元。

### 3.1 帧结构

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
+-------------------------------- - - - - - - - - - - - - - - - +
:                     Payload Data continued ...                :
+ - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
|                     Payload Data continued ...                |
+---------------------------------------------------------------+
```

逐字段解释：

### 3.2 第 1 字节

```
+-+-+-+-+-------+
|F|R|R|R| opcode|
|I|S|S|S|  (4)  |
|N|V|V|V|       |
| |1|2|3|       |
+-+-+-+-+-------+
```

- **FIN (1 bit)**：是否是消息的最后一个帧。
  - `1` = 这是完整消息（或分片的最后一帧）
  - `0` = 后面还有分片
  - 大多数消息一个帧就够了，FIN=1
  - 大消息可以分成多个帧：第一个帧 FIN=0，中间帧 FIN=0，最后帧 FIN=1

- **RSV1, RSV2, RSV3 (各 1 bit)**：保留位，必须为 0。
  除非协商了扩展（如压缩 `permessage-deflate`，RSV1=1）

- **opcode (4 bits)**：帧类型（操作码）

  | opcode | 含义              | 说明                     |
  |--------|-------------------|--------------------------|
  | 0x0    | continuation      | 分片消息的后续帧         |
  | 0x1    | text frame        | 文本数据（UTF-8）        |
  | 0x2    | binary frame      | 二进制数据               |
  | 0x8    | close             | 关闭连接                 |
  | 0x9    | ping              | 心跳请求                 |
  | 0xA    | pong              | 心跳响应                 |
  | 其他   | 保留              | 未定义，收到应关闭连接   |

### 3.3 第 2 字节

```
+-+-------------+
|M| Payload len |
|A|     (7)     |
|S|             |
|K|             |
+-+-------------+
```

- **MASK (1 bit)**：payload 是否掩码。
  - **客户端→服务器：必须掩码**（RFC 6455 强制要求）
  - **服务器→客户端：不掩码**（RFC 6455 禁止服务器掩码）
  - 掩码的目的是防止中间代理缓存污染攻击

- **Payload len (7 bits)**：payload 长度
  - `0-125`：实际长度
  - `126`：实际长度在接下来的 **2 字节**（16 位无符号大端）
  - `127`：实际长度在接下来的 **8 字节**（64 位无符号大端）

### 3.4 扩展长度

当 Payload len = 126 或 127 时，后面有扩展长度字段：

```
Payload len = 126 (0-125 不用扩展):
  +-+-------------+-------------------------------+
  |M|    126      |    Extended payload length    |
  |A|             |             (16)              |
  |S|             |                               |
  |K|             |                               |
  +-+-------------+-------------------------------+
  payload 长度范围: 126 ~ 65535

Payload len = 127:
  +-+-------------+-------------------------------+
  |M|    127      |    Extended payload length    |
  |A|             |             (64)              |
  |S|             |                               |
  |K|             |                               |
  +-+-------------+ - - - - - - - - - - - - - - - +
  |     Extended payload length continued          |
  + - - - - - - - - - - - - - - - - - - - - - - - +
  payload 长度范围: 65536 ~ 2^63-1
```

### 3.5 掩码键（Masking-key）

如果 MASK=1，扩展长度之后有 **4 字节掩码键**：

```
+-------------------------------+-------------------------------+
|                               |Masking-key, if MASK set to 1  |
+-------------------------------+-------------------------------+
| Masking-key (continued)       |          Payload Data         |
+-------------------------------- - - - - - - - - - - - - - - - +
```

掩码键用于对 payload 做 XOR 解密：

```
decoded[i] = encoded[i] XOR mask[i % 4]
```

### 3.6 完整帧长度计算

根据以上字段，帧的总长度：

```
帧总长度 = 头部长度 + payload 长度

头部长度 = 2                          (基本头部)
        + (2 if payload_len == 126)   (16 位扩展长度)
        + (8 if payload_len == 127)   (64 位扩展长度)
        + (4 if masked)               (掩码键)
```

举例：
- 客户端发 "Hello"（5 字节，文本帧）
  - 头部：2 + 4 = 6 字节（5 < 126，不用扩展长度，客户端必须掩码）
  - 帧总长：6 + 5 = 11 字节

- 服务器发 "Hello"（5 字节，文本帧）
  - 头部：2 字节（5 < 126，不用扩展长度，服务器不掩码）
  - 帧总长：2 + 5 = 7 字节

- 客户端发 200 字节的数据
  - 头部：2 + 4 = 6 字节（200 > 125，用 16 位扩展长度，客户端必须掩码）
  - 帧总长：6 + 200 = 206 字节

---

## 四、掩码机制

### 4.1 为什么要掩码

RFC 6455 §5.3 解释了掩码的原因：**防止中间代理缓存污染攻击**。

场景：
1. 客户端通过 HTTP 代理访问服务器
2. 代理可能缓存 HTTP 响应
3. 恶意客户端构造特殊数据，让代理误以为是 HTTP 响应
4. 代理缓存这个"响应"，后续请求返回恶意数据

掩码让客户端发送的数据看起来是随机的，代理无法误解为 HTTP 头部。

### 4.2 掩码算法

掩码算法极其简单：**逐字节 XOR**。

```c
/* 掩码键 mask[4]，payload payload[0..n-1] */
for (int i = 0; i < n; i++) {
    payload[i] ^= mask[i % 4];
}
```

XOR 的美妙之处：**加密解密用同一个操作**。

```
encoded = plain  XOR mask   (加密)
plain  = encoded XOR mask   (解密)
```

因为 `(x XOR m) XOR m = x`。

### 4.3 掩码示例

客户端发 "Hello"：

```
掩码键: [0x37, 0xfa, 0x21, 0x3d]

原始:  H(0x48) e(0x65) l(0x6c) l(0x6c) o(0x6f)
掩码:  0x48^0x37=0x7f  0x65^0xfa=0x9f  0x6c^0x21=0x4d  0x6c^0x3d=0x51  0x6f^0x37=0x58
编码后: [0x7f, 0x9f, 0x4d, 0x51, 0x58]
```

服务器收到后用同样的掩码键解密：

```
编码:  [0x7f, 0x9f, 0x4d, 0x51, 0x58]
解密:  0x7f^0x37=0x48(H)  0x9f^0xfa=0x65(e)  0x4d^0x21=0x6c(l)  0x51^0x3d=0x6c(l)  0x58^0x37=0x6f(o)
原始:  "Hello"
```

---

## 五、帧解析实现

### 5.1 解析函数

```c
/*
 * ws_parse_frame —— 解析一个 WebSocket 帧
 *
 * 返回值：
 *   > 0: 帧的完整长度（已解析成功）
 *   0:  数据不够，需要更多数据
 *   -1: 协议错误
 */
static int ws_parse_frame(const char *buf, int buf_len,
                          int *opcode, char **payload, int *payload_len)
{
    if (buf_len < 2) return 0;  /* 至少需要 2 字节头部 */

    /* 第 1 字节 */
    int fin    = (buf[0] >> 7) & 0x1;
    int op     = buf[0] & 0x0F;

    /* 第 2 字节 */
    int masked = (buf[1] >> 7) & 0x1;
    int len    = buf[1] & 0x7F;

    /* 计算 payload 长度和头部长度 */
    int header_len = 2;
    int payload_length;

    if (len < 126) {
        payload_length = len;
    } else if (len == 126) {
        /* 扩展长度：2 字节 */
        if (buf_len < 4) return 0;
        payload_length = ((unsigned char)buf[2] << 8) |
                         (unsigned char)buf[3];
        header_len = 4;
    } else {
        /* len == 127：8 字节扩展长度 */
        if (buf_len < 10) return 0;
        payload_length = 0;
        for (int i = 0; i < 8; i++) {
            payload_length = (payload_length << 8) |
                             (unsigned char)buf[2 + i];
        }
        header_len = 10;
    }

    /* 掩码键（4 字节） */
    if (masked) {
        header_len += 4;
    }

    /* 检查数据是否完整 */
    if (buf_len < header_len + payload_length) return 0;

    /* 提取 payload 并解掩码 */
    char *p;
    if (masked) {
        unsigned char mask[4];
        memcpy(mask, buf + header_len - 4, 4);
        p = (char *)buf + header_len;
        for (int i = 0; i < payload_length; i++) {
            p[i] ^= mask[i % 4];
        }
    } else {
        p = (char *)buf + header_len;
    }

    *opcode      = op;
    *payload     = p;
    *payload_len = payload_length;

    (void)fin;  /* 教学简化：不处理分片 */
    return header_len + payload_length;
}
```

### 5.2 解析流程图

```
收到数据 buf[0..n-1]
        │
        ▼
    n < 2? ──── 是 ──→ 返回 0（数据不够）
        │
        否
        ▼
    解析第 1 字节: FIN, opcode
    解析第 2 字节: MASK, payload_len
        │
        ▼
   payload_len < 126?
        │           │
       是          否
        │           │
        │      payload_len == 126?
        │           │           │
        │          是          否 (== 127)
        │           │           │
        │      读 2 字节扩展    读 8 字节扩展
        │      header_len=4    header_len=10
        │           │           │
        └─────┬─────┴─────┬─────┘
              │            │
              ▼            ▼
        header_len = 2 (或 4 或 10)
              │
              ▼
        MASK == 1? ─── 是 ──→ header_len += 4
              │
              ▼
    n < header_len + payload_len? ── 是 ──→ 返回 0
              │
              否
              ▼
        提取掩码键（如果有）
        解掩码 payload
              │
              ▼
        返回 header_len + payload_len
```

### 5.3 关键细节

**大端字节序**

WebSocket 帧中的多字节长度字段使用**网络字节序（大端）**：

```c
/* 16 位扩展长度：大端 */
payload_length = ((unsigned char)buf[2] << 8) |
                 (unsigned char)buf[3];

/* 64 位扩展长度：大端 */
payload_length = 0;
for (int i = 0; i < 8; i++) {
    payload_length = (payload_length << 8) |
                     (unsigned char)buf[2 + i];
}
```

**为什么用 `unsigned char` 强转？**

`char` 在某些平台是 `signed char`，`buf[2] << 8` 可能做符号扩展。
强转为 `unsigned char` 确保高位是 0。

**解掩码原地修改**

解掩码直接在 `buf` 上修改（原地解密），`payload` 指针指向 `buf` 内部。
调用者不需要额外释放 payload。

---

## 六、帧生成实现

### 6.1 服务器发送帧

服务器→客户端的帧**不掩码**（RFC 6455 §5.1 规定）。

```c
static int ws_send_frame(int fd, int opcode, const char *data, int len)
{
    /*
     * 把头部和 payload 拼到一个缓冲区，一次 write 发出。
     * 如果分两次 write，TCP 可能分成两个段发送，
     * 客户端第一次 recv 只拿到头部，解析出 length=N
     * 但 payload 为空，导致后续帧全部错位。
     */
    char frame[BUF_SIZE];
    int header_len = 0;

    /* 第 1 字节：FIN=1, opcode */
    frame[0] = 0x80 | (opcode & 0x0F);
    header_len = 1;

    /* 第 2 字节：MASK=0, payload length */
    if (len < 126) {
        frame[1] = len;
        header_len = 2;
    } else if (len < 65536) {
        frame[1] = 126;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        header_len = 4;
    } else {
        frame[1] = 127;
        for (int i = 0; i < 8; i++) {
            frame[2 + i] = (len >> (56 - 8 * i)) & 0xFF;
        }
        header_len = 10;
    }

    /* 拷贝 payload 到帧缓冲区 */
    if (len > 0 && data) {
        memcpy(frame + header_len, data, len);
    }

    /* 一次性发送整个帧 */
    int total = header_len + len;
    int written = 0;
    while (written < total) {
        int n = write(fd, frame + written, total - written);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        written += n;
    }

    return written;
}
```

### 6.2 为什么合并成一次 write

这是实现中一个**关键 bug** 的教训。

最初版本分两次发送：

```c
/* 错误做法：分两次 write */
write(fd, header, header_len);  // 先发头部
write(fd, data, len);           // 再发 payload
```

这会导致客户端收到**两个 TCP 段**：

```
第一次 recv: [0x81] [0x05]              ← 只有 2 字节头部
第二次 recv: [H] [e] [l] [l] [o]        ← 5 字节 payload
```

客户端第一次 `recv()` 只拿到 2 字节，解析出 `opcode=1, length=5`，
但 `payload = data[2:7]` 为空（因为只有 2 字节）。
第二次 `recv()` 拿到 `"Hello"`，但客户端把它当作**新帧**解析：
`'H'(0x48)` 的低 4 位是 `0x8`（close 帧的 opcode），导致协议错误。

**修复方法**：把头部和 payload 拼到一个缓冲区，一次 `write()` 发出：

```c
/* 正确做法：合并成一次 write */
char frame[BUF_SIZE];
memcpy(frame, header, header_len);
memcpy(frame + header_len, data, len);
write(fd, frame, header_len + len);
```

虽然 TCP 仍然可能分段，但 `write()` 一次写入的数据通常在同一个 TCP 段中。
即使分段，客户端的 `recv()` 也可能在一次调用中拿到完整帧。

### 6.3 长度字段编码

```c
if (len < 126) {
    frame[1] = len;           /* 直接放 7 位 */
    header_len = 2;
} else if (len < 65536) {
    frame[1] = 126;           /* 标记：用 16 位扩展 */
    frame[2] = (len >> 8) & 0xFF;  /* 高字节 */
    frame[3] = len & 0xFF;         /* 低字节 */
    header_len = 4;
} else {
    frame[1] = 127;           /* 标记：用 64 位扩展 */
    for (int i = 0; i < 8; i++) {
        frame[2 + i] = (len >> (56 - 8 * i)) & 0xFF;
    }
    header_len = 10;
}
```

---

## 七、消息处理

### 7.1 opcode 分发

收到帧后，根据 opcode 分发处理：

```c
static void handle_ws_message(int fd, int opcode,
                              char *payload, int payload_len)
{
    switch (opcode) {

    case WS_OPCODE_TEXT:
    case WS_OPCODE_BINARY:
        /* Echo：原样发回 */
        log_debug("收到消息 fd=%d: %.*s", fd, payload_len, payload);
        ws_send_frame(fd, opcode, payload, payload_len);
        break;

    case WS_OPCODE_PING:
        /* Ping → 回 Pong */
        log_debug("收到 Ping fd=%d", fd);
        ws_send_frame(fd, WS_OPCODE_PONG, payload, payload_len);
        break;

    case WS_OPCODE_PONG:
        /* Pong：心跳响应，忽略 */
        log_debug("收到 Pong fd=%d", fd);
        break;

    case WS_OPCODE_CLOSE:
        /* 客户端关闭：回 Close 帧后由 epoll 正常清理 */
        log_debug("收到 Close fd=%d", fd);
        ws_send_frame(fd, WS_OPCODE_CLOSE, NULL, 0);
        shutdown(fd, SHUT_WR);
        break;

    default:
        log_error("未知 opcode: %d", opcode);
    }
}
```

### 7.2 Echo 逻辑

本实现是一个 **Echo 服务器**：收到什么就原样发回。

```c
case WS_OPCODE_TEXT:
case WS_OPCODE_BINARY:
    ws_send_frame(fd, opcode, payload, payload_len);
    break;
```

注意：`opcode` 透传——收到文本帧回文本帧，收到二进制帧回二进制帧。

### 7.3 Ping / Pong 心跳

WebSocket 定义了 Ping/Pong 帧用于**心跳检测**：

- **Ping (0x9)**：一方发 Ping，可以带 payload
- **Pong (0xA)**：另一方必须尽快回 Pong，payload 原样返回

```c
case WS_OPCODE_PING:
    ws_send_frame(fd, WS_OPCODE_PONG, payload, payload_len);
    break;
```

Ping/Pong 的用途：
1. **检测连接是否存活**：发了 Ping 迟迟收不到 Pong，说明连接断了
2. **保持连接**：中间代理（NAT、负载均衡器）会超时清理空闲连接，
   定期 Ping 保持连接不过期
3. **测量延迟**：Ping 发出时间到 Pong 收到时间 = RTT

### 7.4 Close 帧

Close 帧用于**优雅关闭** WebSocket 连接：

```c
case WS_OPCODE_CLOSE:
    ws_send_frame(fd, WS_OPCODE_CLOSE, NULL, 0);
    shutdown(fd, SHUT_WR);
    break;
```

关闭流程（RFC 6455 §7.1.1）：

```
端点 A ──Close 帧──→ 端点 B
端点 A ←──Close 帧── 端点 B  (B 回一个 Close 作为确认)
端点 A ──TCP FIN──→ 端点 B   (A 关闭 TCP 连接)
```

Close 帧可以带 payload（状态码 + 原因短语）：

```
Payload: [状态码(2字节)] [原因短语(可选)]
```

常见状态码：

| 状态码 | 含义                     |
|--------|--------------------------|
| 1000   | 正常关闭                 |
| 1001   | 端点离开（如关闭页面）   |
| 1002   | 协议错误                 |
| 1003   | 不支持的数据类型         |
| 1006   | 异常关闭（没有发 Close） |
| 1011   | 服务器遇到意外情况       |

本实现简化处理，收到 Close 后回一个空 Close，然后 `shutdown(SHUT_WR)` 通知对端不再写数据。

---

## 八、连接状态管理

### 8.1 连接状态机

每个连接有两个状态：

```c
typedef enum {
    STATE_HTTP_HANDSHAKE,   /* 还在 HTTP 握手阶段 */
    STATE_WS_CONNECTED,     /* 已升级为 WebSocket */
} conn_state_t;

typedef struct {
    int          fd;
    conn_state_t state;
} conn_t;
```

状态转换：

```
新连接 ──→ STATE_HTTP_HANDSHAKE
                │
                │ 收到 HTTP Upgrade 请求
                │ do_handshake() 成功
                ▼
          STATE_WS_CONNECTED
                │
                │ 收到 WebSocket 帧
                │ 循环处理帧
                │
                │ 收到 Close 帧 或 对端关闭
                ▼
            [连接关闭]
```

### 8.2 handle_read 逻辑

```c
static void handle_read(int epfd, conn_t *conn)
{
    char buf[BUF_SIZE];
    int n = read(conn->fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        /* 对端关闭或出错 */
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close_conn(epfd, conn);
        }
        return;
    }
    buf[n] = '\0';

    if (conn->state == STATE_HTTP_HANDSHAKE) {
        /* HTTP Upgrade 握手 */
        if (do_handshake(conn->fd, buf, n) == 0) {
            conn->state = STATE_WS_CONNECTED;
        } else {
            close_conn(epfd, conn);
        }
    } else {
        /* WebSocket 帧解析 */
        int opcode, payload_len;
        char *payload;
        int frame_len = ws_parse_frame(buf, n, &opcode, &payload, &payload_len);

        if (frame_len > 0) {
            handle_ws_message(conn->fd, opcode, payload, payload_len);
        } else if (frame_len < 0) {
            log_error("帧解析错误 fd=%d", conn->fd);
            close_conn(epfd, conn);
        }
        /* frame_len == 0: 数据不够，等下次 EPOLLIN */
    }
}
```

### 8.3 连接清理

```c
static void close_conn(int epfd, conn_t *conn)
{
    if (!conn) return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);  /* 从 epoll 移除 */
    close(conn->fd);                                   /* 关闭 fd */
    if (conn->fd >= 0 && conn->fd < MAX_CLIENTS && clients[conn->fd]) {
        clients[conn->fd] = NULL;                      /* 清理引用 */
    }
    free(conn);                                        /* 释放内存 */
}
```

清理顺序很重要：
1. **先从 epoll 移除**：否则关闭 fd 后 epoll 可能报错
2. **再关 fd**：释放系统资源
3. **清理 clients 数组**：防止悬空指针
4. **释放 conn 结构体**：释放内存

---

## 九、epoll 事件循环

### 9.1 主循环

```c
int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* 创建监听 socket */
    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    set_nonblocking(listen_fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    Bind(listen_fd, (SA *)&addr, sizeof(addr));
    Listen(listen_fd, BACKLOG);

    int epfd = epoll_create(1);
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;  /* 边沿触发 */
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                if (!running) break;
                continue;
            }
            err_sys("epoll_wait error");
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                /* 新连接：ET 模式必须循环 accept */
                for (;;) {
                    int conn_fd = accept(listen_fd, NULL, NULL);
                    if (conn_fd < 0) break;
                    set_nonblocking(conn_fd);

                    conn_t *conn = calloc(1, sizeof(conn_t));
                    conn->fd    = conn_fd;
                    conn->state = STATE_HTTP_HANDSHAKE;
                    clients[conn_fd] = conn;

                    struct epoll_event cev;
                    cev.events  = EPOLLIN | EPOLLET;
                    cev.data.fd = conn_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &cev);
                }
            } else {
                /* 数据到达 */
                int fd = events[i].data.fd;
                if (clients[fd]) {
                    handle_read(epfd, clients[fd]);
                }
            }
        }
    }

    Close(listen_fd);
    Close(epfd);
    return 0;
}
```

### 9.2 边沿触发注意事项

使用 `EPOLLET`（边沿触发）模式：

1. **accept 必须循环**：一次 EPOLLIN 事件可能有多个待 accept 的连接
2. **read 要读到 EAGAIN**：一次 EPOLLIN 事件可能有大量数据
   （本实现用 64KB 缓冲区，教学简化，不循环读）
3. **write 要写到 EAGAIN**：非阻塞 write 可能部分写入
   （`ws_send_frame` 中有循环写入逻辑）

### 9.3 fd 区分

`epoll_event.data` 是 union，可以存 `fd` 或 `ptr`。

本实现用 `data.fd` 存 fd，通过 `fd == listen_fd` 区分监听 socket 和连接 socket：

```c
if (events[i].data.fd == listen_fd) {
    /* 新连接 */
} else {
    /* 数据到达 */
}
```

另一种方案是用 `data.ptr` 存 `conn_t*`，`listen_fd` 用特殊标记区分。
两种方案都可以，`data.fd` 更简单。

---

## 十、测试验证

### 10.1 编译运行

```bash
# 编译
cmake --build build --target ws_server

# 运行（默认端口 8080）
build/bin/ws_server 8080
```

### 10.2 curl 握手测试

用 curl 模拟 WebSocket 握手请求：

```bash
curl -v --include \
    -H 'Connection: Upgrade' \
    -H 'Upgrade: websocket' \
    -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
    -H 'Sec-WebSocket-Version: 13' \
    http://localhost:8080/
```

预期输出：

```
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

`dGhlIHNhbXBsZSBub25jZQ==` 是 RFC 6455 的示例 Key，
对应的 Accept 值 `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=` 也是 RFC 规定的。

### 10.3 Python 客户端测试

```python
import socket, base64, os, struct

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('localhost', 8080))

# 1. 握手
key = base64.b64encode(os.urandom(16)).decode()
request = (
    f"GET /chat HTTP/1.1\r\n"
    f"Host: localhost:8080\r\n"
    f"Upgrade: websocket\r\n"
    f"Connection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {key}\r\n"
    f"Sec-WebSocket-Version: 13\r\n"
    f"\r\n"
)
sock.send(request.encode())
response = sock.recv(4096).decode()
assert '101' in response  # 握手成功

# 2. 发送文本帧（客户端必须掩码）
def ws_send(sock, data, opcode=0x1):
    payload = data.encode()
    mask = os.urandom(4)
    masked = bytearray(len(payload))
    for i in range(len(payload)):
        masked[i] = payload[i] ^ mask[i % 4]
    header = bytearray()
    header.append(0x80 | opcode)  # FIN=1
    header.append(0x80 | len(payload))  # MASK=1
    header.extend(mask)
    header.extend(masked)
    sock.send(header)

# 3. 接收帧（服务器不掩码）
def ws_recv(sock):
    data = sock.recv(4096)
    opcode = data[0] & 0x0F
    length = data[1] & 0x7F
    payload = data[2:2+length]
    return opcode, payload.decode()

# 4. Echo 测试
ws_send(sock, 'Hello WebSocket!')
opcode, msg = ws_recv(sock)
assert msg == 'Hello WebSocket!'  # ✓

# 5. Ping/Pong 测试
ws_send(sock, 'pingdata', opcode=0x9)
opcode, msg = ws_recv(sock)
assert opcode == 0xA  # Pong ✓
```

### 10.4 测试结果

```
[PASS] 连接成功
[PASS] 握手成功
[PASS] Echo: Hello WebSocket!
[PASS] Echo: 第二次消息
[PASS] Ping/Pong 成功
=== 所有测试通过 ===
```

---

## 十一、WebSocket 帧实战分析

### 11.1 客户端发送 "Hello"

```
客户端发送（掩码）:
  0x81 0x85 0x37 0xfa 0x21 0x3d 0x7f 0x9f 0x4d 0x51 0x58
  │    │    │─────────────│    │─────────────────────────│
  │    │    │  掩码键      │    │  掩码后的 payload        │
  │    │    └──────────────┘    └──────────────────────────┘
  │    │
  │    └─ 0x85: MASK=1, len=5
  └─ 0x81: FIN=1, opcode=1 (text)

解掩码:
  0x7f ^ 0x37 = 0x48 = 'H'
  0x9f ^ 0xfa = 0x65 = 'e'
  0x4d ^ 0x21 = 0x6c = 'l'
  0x51 ^ 0x3d = 0x6c = 'l'
  0x58 ^ 0x37 = 0x6f = 'o'
  → "Hello"
```

### 11.2 服务器回显 "Hello"

```
服务器发送（不掩码）:
  0x81 0x05 0x48 0x65 0x6c 0x6c 0x6f
  │    │    │─────────────────────────│
  │    │    │  原始 payload           │
  │    │    └──────────────────────────┘
  │    │
  │    └─ 0x05: MASK=0, len=5
  └─ 0x81: FIN=1, opcode=1 (text)
```

服务器发送的帧比客户端少 4 字节（没有掩码键）。

### 11.3 大消息（> 125 字节）

发送 200 字节的数据：

```
客户端发送:
  0x82 0xFE 0x00 0xC8 [4字节掩码] [200字节掩码payload]
  │    │    │─────────│
  │    │    │ 16位长度 │
  │    │    └──────────┘
  │    └─ 0xFE: MASK=1, len=126 (用16位扩展)
  └─ 0x82: FIN=1, opcode=2 (binary)

  0x00C8 = 200
```

### 11.4 Close 帧

```
客户端发送 Close:
  0x88 0x82 [4字节掩码] [2字节掩码状态码]
  │    │
  │    └─ 0x82: MASK=1, len=2
  └─ 0x88: FIN=1, opcode=8 (close)

  状态码 1000 (正常关闭) 掩码后发送

服务器回 Close:
  0x88 0x00
  │    │
  │    └─ 0x00: MASK=0, len=0 (空 payload)
  └─ 0x88: FIN=1, opcode=8 (close)
```

---

## 十二、WebSocket 应用场景

### 12.1 实时聊天

```
浏览器 A ──┐                    ┌── 浏览器 B
           │    WebSocket 服务器  │
浏览器 C ──┤←──── 全双工 ────→├── 浏览器 D
           │                    │
浏览器 E ──┘                    ┘
```

用户 A 发消息 → 服务器收到 → 服务器推给 B、C、D、E

### 12.2 实时通知

```
事件源 ──→ 服务器 ──WebSocket──→ 浏览器
                              ↑
                         服务器主动推
```

如 GitHub 的通知、Slack 的消息提醒。

### 12.3 在线协作

Google Docs、Figma 等用 WebSocket 同步操作：

```
用户 A 编辑 ──→ 服务器 ──→ 用户 B 看到 A 的编辑
用户 B 编辑 ──→ 服务器 ──→ 用户 A 看到 B 的编辑
```

### 12.4 游戏服务器

实时游戏用 WebSocket 传输游戏状态：

```
客户端 A ──位置更新──→ 服务器 ──→ 广播给所有玩家
客户端 B ──位置更新──→ 服务器 ──→ 广播给所有玩家
```

### 12.5 股票行情

```
交易所 ──→ 服务器 ──WebSocket──→ 客户端
                        ↑
                   主动推送行情
```

---

## 十三、WebSocket vs 其他技术

### 13.1 vs Long Polling

| 特性         | Long Polling    | WebSocket        |
| ------------ | --------------- | ---------------- |
| 通信模式     | 半双工          | 全双工           |
| 连接复用     | 每次响应后重连  | 握手后永久复用   |
| 头部开销     | 每次请求带 HTTP 头 | 2-14 字节帧头  |
| 实时性       | 较好（但需重连） | 极好             |
| 服务器复杂度 | 简单            | 中等             |
| 浏览器兼容   | 所有浏览器      | IE10+            |

### 13.2 vs SSE (Server-Sent Events)

| 特性         | SSE             | WebSocket        |
| ------------ | --------------- | ---------------- |
| 方向         | 服务器→客户端   | 双向             |
| 数据格式     | UTF-8 文本      | 文本或二进制     |
| 自动重连     | 浏览器自动重连  | 需手动实现       |
| 协议         | HTTP            | WebSocket        |
| 适用场景     | 通知、行情      | 聊天、游戏、协作 |

### 13.3 vs HTTP/2 Stream

HTTP/2 的 Stream 也是双向的，但有区别：

| 特性         | HTTP/2 Stream   | WebSocket        |
| ------------ | --------------- | ---------------- |
| 生命周期     | 请求-响应后关闭 | 永久（直到 Close）|
| 服务器推送   | Server Push     | 原生全双工       |
| 头部压缩     | HPACK           | 无（帧头极小）   |
| 多路复用     | 是              | 否（一条连接）   |

---

## 十四、安全考虑

### 14.1 Origin 检查

浏览器会自动在握手请求中加 `Origin` 头：

```http
Origin: http://localhost:3000
```

服务器应该检查 Origin，只允许信任的来源：

```c
/* 生产环境应该检查 Origin */
char *origin = strstr(request, "Origin: ");
if (origin) {
    /* 提取并验证 Origin */
    /* if (!is_allowed_origin(origin)) return -1; */
}
```

不检查 Origin 的风险：恶意网站可以让用户的浏览器连接你的 WebSocket 服务器，
执行未授权操作（CSRF 的 WebSocket 版本）。

### 14.2 掩码的安全意义

客户端→服务器的帧**必须掩码**，这不是为了加密（掩码键在帧里明文传输），
而是为了**防止中间代理缓存污染**。

攻击场景（无掩码时）：
1. 客户端通过代理访问服务器
2. 客户端发送数据，内容看起来像 HTTP 响应：
   `HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n...`
3. 代理误以为这是服务器发的响应，缓存它
4. 后续请求返回这个恶意缓存

掩码让数据看起来是随机的，代理无法误解。

### 14.3 wss (WebSocket Secure)

和 HTTPS 一样，WebSocket 也有加密版本：

```
ws://   → 明文 WebSocket（端口 80）
wss://  → TLS 加密 WebSocket（端口 443）
```

wss 的实现：
1. 先建立 TLS 连接
2. 在 TLS 连接上做 WebSocket 握手
3. 在 TLS 连接上收发 WebSocket 帧

本实现是 ws://（明文），生产环境应该用 wss://。
可以结合 stage8 的 TLS 知识实现 wss://。

### 14.4 消息大小限制

恶意客户端可能发送超大帧，耗尽服务器内存。

```c
/* 应该限制最大 payload 大小 */
#define MAX_PAYLOAD_SIZE (1024 * 1024)  /* 1MB */

if (payload_length > MAX_PAYLOAD_SIZE) {
    log_error("payload 太大: %d", payload_length);
    return -1;
}
```

本实现没有做这个检查（教学简化），生产环境必须做。

---

## 十五、完整代码结构

### 15.1 文件清单

```
stage12_websocket/
├── ws_server.c          # WebSocket 服务器实现
├── CMakeLists.txt       # 构建配置
└── test_ws.sh           # 测试脚本
```

### 15.2 代码组织

```
ws_server.c
├── 常量定义 (MAX_EVENTS, BUF_SIZE, ...)
├── WebSocket opcode 枚举
├── 连接状态定义 (conn_state_t, conn_t)
├── Base64 编码 (base64_encode)
├── 握手 (compute_accept_key, do_handshake)
├── 帧解析 (ws_parse_frame)
├── 帧生成 (ws_send_frame)
├── 消息处理 (handle_ws_message)
├── 连接管理 (close_conn, handle_read)
└── main (epoll 事件循环)
```

### 15.3 构建配置

```cmake
# CMakeLists.txt
add_executable(ws_server ws_server.c)

target_link_libraries(ws_server
    common_v2       # phase1 的通用工具
    pthread
    ssl             # OpenSSL（SHA1）
    crypto
)
```

---

## 十六、踩坑记录

### 16.1 OpenSSL BIO 段错误

**问题**：用 OpenSSL 的 BIO API 做 Base64 编码，段错误。

```c
/* 段错误代码 */
BIO *b64 = BIO_new(BIO_f_base64());  // OpenSSL 3.0 兼容问题
BIO *mem = BIO_new(BIO_s_mem());
b64 = BIO_push(b64, mem);
BIO_write(b64, data, len);
BIO_flush(b64);
```

**原因**：OpenSSL 3.0 中 `BIO_f_base64()` 隐式声明返回 `int` 而非指针，
`BIO_new()` 收到截断的指针导致段错误。

**解决**：手动实现 Base64 编码，不依赖 OpenSSL BIO API。

### 16.2 分次 write 导致帧错位

**问题**：客户端收到 Echo 内容为空或错位。

**原因**：`ws_send_frame` 分两次 `write()`（先头部后 payload），
TCP 分成两个段，客户端第一次 `recv()` 只拿到头部。

**解决**：合并到同一个缓冲区，一次 `write()` 发出。

### 16.3 close fd 后 segfault

**问题**：客户端关闭连接后服务器 segfault。

**原因**：在 `handle_ws_message` 中收到 Close 帧后直接 `close(fd)`，
但 `conn` 结构体未被释放，后续 epoll 事件访问已关闭的 fd。

**解决**：
1. 收到 Close 帧后只 `shutdown(fd, SHUT_WR)`，不 `close(fd)`
2. 让 epoll 的正常流程（read 返回 0）触发 `close_conn`
3. `close_conn` 统一处理：epoll 移除 → close fd → free conn

### 16.4 边沿触发 accept 漏接

**问题**：高并发时部分连接未被 accept。

**原因**：`EPOLLET` 模式下，一次 EPOLLIN 事件可能有多个待 accept 的连接，
只 accept 一次会漏掉。

**解决**：循环 accept 直到返回 EAGAIN。

---

## 十七、扩展方向

### 17.1 分片消息

RFC 6455 允许把大消息分成多个帧：

```
帧1: FIN=0, opcode=1 (text),  payload="Hello "
帧2: FIN=0, opcode=0 (cont),  payload="World "
帧3: FIN=1, opcode=0 (cont),  payload="!"
```

接收方需要缓存分片，直到 FIN=1 才组装完整消息。
本实现简化处理，假设每个帧都是完整消息（FIN=1）。

### 17.2 压缩扩展

`permessage-deflate` 扩展（RFC 7672）用 zlib 压缩 payload：

```
握手时协商:
  Sec-WebSocket-Extensions: permessage-deflate

帧头 RSV1=1 表示压缩:
  0xC1 ... (FIN=1, RSV1=1, opcode=1)
```

### 17.3 子协议协商

握手时可以协商子协议：

```
客户端: Sec-WebSocket-Protocol: chat, superchat
服务器: Sec-WebSocket-Protocol: chat
```

服务器选一个支持的协议在响应中返回。

### 17.4 多路复用

一条 WebSocket 连接上跑多个逻辑通道（类似 HTTP/2 的 Stream）。
需要应用层实现，WebSocket 协议本身不支持。

### 17.5 心跳定时器

本实现被动响应 Ping/Pong，不主动发心跳。
生产环境应该定时发 Ping，检测连接是否存活：

```c
/* 每隔 30 秒对所有连接发 Ping */
void heartbeat_timer(int epfd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->state == STATE_WS_CONNECTED) {
            ws_send_frame(clients[i]->fd, WS_OPCODE_PING, "ping", 4);
        }
    }
}
```

---

## 十八、WebSocket 协议总结

### 18.1 协议要点

1. **握手用 HTTP**：客户端发 HTTP Upgrade 请求，服务器回 101
2. **握手后是 TCP**：不再用 HTTP 格式，用 WebSocket 帧格式
3. **帧是基本单元**：每个帧有头部（2-14 字节）+ payload
4. **客户端必须掩码**：防止代理缓存污染
5. **服务器不掩码**：减少开销
6. **全双工通信**：双方随时发帧
7. **Close 帧优雅关闭**：先发 Close，再关 TCP

### 18.2 帧格式速查

```
┌─────────┬─────────┬──────────────┬──────────┬─────────┐
│ FIN     │ opcode  │ MASK         │ len      │ payload  │
│ 1 bit   │ 3 bits  │ 1 bit        │ 7+ bits  │ 变长     │
└─────────┴─────────┴──────────────┴──────────┴─────────┘

len < 126:   实际长度
len = 126:   后跟 2 字节扩展长度
len = 127:   后跟 8 字节扩展长度
MASK = 1:   后跟 4 字节掩码键，payload 需 XOR 解密
```

### 18.3 opcode 速查

```
0x0: continuation  (分片后续帧)
0x1: text          (UTF-8 文本)
0x2: binary        (二进制数据)
0x8: close         (关闭连接)
0x9: ping          (心跳请求)
0xA: pong          (心跳响应)
```

### 18.4 握手速查

```
请求:
  GET /path HTTP/1.1
  Upgrade: websocket
  Connection: Upgrade
  Sec-WebSocket-Key: <随机Base64>
  Sec-WebSocket-Version: 13

响应:
  HTTP/1.1 101 Switching Protocols
  Upgrade: websocket
  Connection: Upgrade
  Sec-WebSocket-Accept: Base64(SHA1(Key + GUID))

  GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
```

---

## 十九、从 WebSocket 到下一步

### 19.1 回顾

本阶段实现了：
- HTTP Upgrade 握手
- Sec-WebSocket-Accept 计算（SHA1 + Base64）
- WebSocket 帧解析（处理掩码、扩展长度）
- WebSocket 帧生成（合并写入避免 TCP 分段）
- 消息处理（Echo、Ping/Pong、Close）
- epoll 事件循环（边沿触发）
- 连接状态管理（握手 → WebSocket → 关闭）

### 19.2 下一步

WebSocket 解决了"服务器主动推送"的问题，但有一个限制：
**回调风格编程**。收到消息后处理，处理完再等下一条——异步回调嵌套。

如果能像写同步代码一样写异步逻辑，代码会清晰很多。
这就是**协程**的价值：用同步的写法实现异步的效果。

下一阶段（stage13）我们将实现**协程**（coroutine），
用 `ucontext` 或汇编实现用户态上下文切换，
让异步代码看起来像同步代码。

```
回调风格（当前）:
  on_message(msg) {
      process(msg);
      send(response);
  }

协程风格（下一步）:
  coroutine() {
      msg = co_await recv();  // 看起来是同步的
      process(msg);
      send(response);
  }
```

---

## 二十、完整代码

```c
/*
 * ws_server.c —— 阶段 12：WebSocket 服务器
 *
 * WebSocket 协议流程：
 * 1. HTTP Upgrade 握手
 * 2. 握手后用帧通信
 *
 * 运行：
 *   build/bin/ws_server 8080
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#include <openssl/sha.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define MAX_EVENTS 256
#define BACKLOG    512
#define BUF_SIZE   65536
#define MAX_CLIENTS 1024

enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT         = 0x1,
    WS_OPCODE_BINARY       = 0x2,
    WS_OPCODE_CLOSE        = 0x8,
    WS_OPCODE_PING         = 0x9,
    WS_OPCODE_PONG         = 0xA,
};

typedef enum {
    STATE_HTTP_HANDSHAKE,
    STATE_WS_CONNECTED,
} conn_state_t;

typedef struct {
    int          fd;
    conn_state_t state;
} conn_t;

static conn_t *clients[MAX_CLIENTS];
static volatile sig_atomic_t running = 1;

static void sig_handler(int sig) { (void)sig; running = 0; }

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_conn(int epfd, conn_t *conn) {
    if (!conn) return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    if (conn->fd >= 0 && conn->fd < MAX_CLIENTS)
        clients[conn->fd] = NULL;
    free(conn);
}

/* Base64 编码 */
static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *result = malloc(out_len + 1);
    int j = 0;
    for (size_t i = 0; i < len;) {
        unsigned int a = i < len ? data[i++] : 0;
        unsigned int b = i < len ? data[i++] : 0;
        unsigned int c = i < len ? data[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        result[j++] = base64_table[(triple >> 18) & 0x3F];
        result[j++] = base64_table[(triple >> 12) & 0x3F];
        result[j++] = base64_table[(triple >> 6) & 0x3F];
        result[j++] = base64_table[triple & 0x3F];
    }
    for (size_t i = 0; i < (3 - len % 3) % 3; i++)
        result[out_len - 1 - i] = '=';
    result[out_len] = '\0';
    return result;
}

/* 计算 Sec-WebSocket-Accept */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static char *compute_accept_key(const char *client_key) {
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", client_key, WS_GUID);
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)combined, strlen(combined), hash);
    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

/* HTTP Upgrade 握手 */
static int do_handshake(int fd, char *request, int req_len) {
    (void)req_len;
    char *key_start = strstr(request, "Sec-WebSocket-Key: ");
    if (!key_start) return -1;
    key_start += strlen("Sec-WebSocket-Key: ");
    char *key_end = strstr(key_start, "\r\n");
    if (!key_end) return -1;

    char client_key[128];
    int key_len = key_end - key_start;
    memcpy(client_key, key_start, key_len);
    client_key[key_len] = '\0';

    char *accept_key = compute_accept_key(client_key);
    char response[512];
    int resp_len = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key);
    free(accept_key);
    write(fd, response, resp_len);
    return 0;
}

/* 帧解析 */
static int ws_parse_frame(const char *buf, int buf_len,
                          int *opcode, char **payload, int *payload_len) {
    if (buf_len < 2) return 0;
    int op = buf[0] & 0x0F;
    int masked = (buf[1] >> 7) & 0x1;
    int len = buf[1] & 0x7F;
    int header_len = 2;
    int payload_length;

    if (len < 126) {
        payload_length = len;
    } else if (len == 126) {
        if (buf_len < 4) return 0;
        payload_length = ((unsigned char)buf[2] << 8) | (unsigned char)buf[3];
        header_len = 4;
    } else {
        if (buf_len < 10) return 0;
        payload_length = 0;
        for (int i = 0; i < 8; i++)
            payload_length = (payload_length << 8) | (unsigned char)buf[2 + i];
        header_len = 10;
    }
    if (masked) header_len += 4;
    if (buf_len < header_len + payload_length) return 0;

    char *p;
    if (masked) {
        unsigned char mask[4];
        memcpy(mask, buf + header_len - 4, 4);
        p = (char *)buf + header_len;
        for (int i = 0; i < payload_length; i++)
            p[i] ^= mask[i % 4];
    } else {
        p = (char *)buf + header_len;
    }
    *opcode = op;
    *payload = p;
    *payload_len = payload_length;
    return header_len + payload_length;
}

/* 帧生成 */
static int ws_send_frame(int fd, int opcode, const char *data, int len) {
    char frame[BUF_SIZE];
    int header_len = 0;
    frame[0] = 0x80 | (opcode & 0x0F);
    if (len < 126) {
        frame[1] = len;
        header_len = 2;
    } else if (len < 65536) {
        frame[1] = 126;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        header_len = 4;
    } else {
        frame[1] = 127;
        for (int i = 0; i < 8; i++)
            frame[2 + i] = (len >> (56 - 8 * i)) & 0xFF;
        header_len = 10;
    }
    if (len > 0 && data)
        memcpy(frame + header_len, data, len);
    int total = header_len + len;
    int written = 0;
    while (written < total) {
        int n = write(fd, frame + written, total - written);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        written += n;
    }
    return written;
}

/* 消息处理 */
static void handle_ws_message(int fd, int opcode,
                              char *payload, int payload_len) {
    switch (opcode) {
    case WS_OPCODE_TEXT:
    case WS_OPCODE_BINARY:
        ws_send_frame(fd, opcode, payload, payload_len);
        break;
    case WS_OPCODE_PING:
        ws_send_frame(fd, WS_OPCODE_PONG, payload, payload_len);
        break;
    case WS_OPCODE_PONG:
        break;
    case WS_OPCODE_CLOSE:
        ws_send_frame(fd, WS_OPCODE_CLOSE, NULL, 0);
        shutdown(fd, SHUT_WR);
        break;
    }
}

/* 连接处理 */
static void handle_read(int epfd, conn_t *conn) {
    char buf[BUF_SIZE];
    int n = read(conn->fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            close_conn(epfd, conn);
        return;
    }
    buf[n] = '\0';
    if (conn->state == STATE_HTTP_HANDSHAKE) {
        if (do_handshake(conn->fd, buf, n) == 0)
            conn->state = STATE_WS_CONNECTED;
        else
            close_conn(epfd, conn);
    } else {
        int opcode, payload_len;
        char *payload;
        int frame_len = ws_parse_frame(buf, n, &opcode, &payload, &payload_len);
        if (frame_len > 0)
            handle_ws_message(conn->fd, opcode, payload, payload_len);
        else if (frame_len < 0)
            close_conn(epfd, conn);
    }
}

int main(int argc, char *argv[]) {
    int port = argc >= 2 ? atoi(argv[1]) : 8080;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    set_nonblocking(listen_fd);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    Bind(listen_fd, (SA *)&addr, sizeof(addr));
    Listen(listen_fd, BACKLOG);

    int epfd = epoll_create(1);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.fd = listen_fd};
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    struct epoll_event events[MAX_EVENTS];
    while (running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                for (;;) {
                    int conn_fd = accept(listen_fd, NULL, NULL);
                    if (conn_fd < 0) break;
                    set_nonblocking(conn_fd);
                    conn_t *conn = calloc(1, sizeof(conn_t));
                    conn->fd = conn_fd;
                    conn->state = STATE_HTTP_HANDSHAKE;
                    clients[conn_fd] = conn;
                    struct epoll_event cev = {
                        .events = EPOLLIN | EPOLLET, .data.fd = conn_fd
                    };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &cev);
                }
            } else {
                int fd = events[i].data.fd;
                if (clients[fd])
                    handle_read(epfd, clients[fd]);
            }
        }
    }
    close(listen_fd);
    close(epfd);
    return 0;
}
```

---

## 附录：RFC 6455 关键章节

| 章节  | 内容                        |
|-------|-----------------------------|
| §1    | 引言                        |
| §4    | 握手（Opening Handshake）   |
| §5    | 数据帧（Data Framing）      |
| §5.1  | 掩码要求                    |
| §5.2  | 帧格式                      |
| §5.3  | 掩码算法                    |
| §5.5  | 分片                        |
| §6    | 发送和接收数据              |
| §7    | 关闭连接（Closing Handshake）|
| §7.1.1| 关闭状态机                  |
| §10   | 安全考虑                    |
| §11   | IANA 考虑（opcode 注册）    |