# 00 - 网络字节序与结构体对齐

> 本篇是整个教学项目的前置知识。如果你不理解字节序和对齐，
> 后面解析 IP/TCP 头部时会觉得"明明按结构体取了字段，值却是乱的"。
>
> 这一篇会从历史讲起，把字节序、对齐、位域、校验和这些"看似枯燥但其实非常实用"
> 的底层知识一次讲透。读完之后你不仅看得懂 `sniff.c` 里那些奇怪的写法，
> 还能在面试里把面试官问懵。

## 目录

1. [什么是字节序](#1-什么是字节序)
2. [大端和小端的直观演示](#2-大端和小端的直观演示)
3. [为什么网络要用大端](#3-为什么网络要用大端)
4. [htons / ntohs / htonl / ntohl](#4-htons--ntohs--htonl--ntohl)
5. [结构体对齐：packed 的必要性](#5-结构体对齐packed-的必要性)
6. [位域与字节序的交互](#6-位域与字节序的交互)
7. [常见陷阱](#7-常见陷阱)
8. [动手实验](#8-动手实验)
9. [字节序的历史背景](#9-字节序的历史背景)
10. [网络字节序的由来](#10-网络字节序的由来)
11. [抓包中看到的字节序](#11-抓包中看到的字节序)
12. [校验和计算详解](#12-校验和计算详解)
13. [跨平台字节序处理](#13-跨平台字节序处理)
14. [union 检测字节序](#14-union-检测字节序)
15. [位域在不同编译器上的行为](#15-位域在不同编译器上的行为)
16. [结构体对齐的详细规则](#16-结构体对齐的详细规则)
17. [内存对齐对性能的影响](#17-内存对齐对性能的影响)
18. [常见面试题精选](#18-常见面试题精选)
19. [完整代码示例](#19-完整代码示例)

---

## 1. 什么是字节序

### 1.1 问题的起源

计算机内存按**字节**编址，每个地址存 1 个字节（8 bit）。
但很多数据类型超过 1 字节：

- `uint16_t`：2 字节
- `uint32_t`：4 字节
- `uint64_t`：8 字节

一个 4 字节整数 `0x01020304`，在内存里从低地址到高地址怎么放？
有两种放法，这就是**字节序**（byte order）：

### 1.2 大端（Big-Endian）

> 高位字节放在低地址，低位字节放在高地址。
> 像人读数字的顺序。

```
地址:  0x1000  0x1001  0x1002  0x1003
内容:    01      02      03      04
```

记忆：**大端 → 大端在前（低地址）**

### 1.3 小端（Little-Endian）

> 低位字节放在低地址，高位字节放在高地址。

```
地址:  0x1000  0x1001  0x1002  0x1003
内容:    04      03      02      01
```

记忆：**小端 → 小端在前（低地址）**

### 1.4 谁用大端，谁用小端？

| 架构 | 字节序 | 说明 |
|------|--------|------|
| x86 / x86-64 | 小端 | 你用的 PC、服务器基本都是 |
| ARM | 可配置 | 默认小端，但支持大端模式 |
| PowerPC | 可配置 | 历史上大端（如早期 Mac） |
| SPARC | 大端 | Sun 的工作站 |
| MIPS | 可配置 | 嵌入式常见 |
| RISC-V | 小端 | 开源指令集 |
| **网络协议** | **大端** | TCP/IP 规定 |

**关键矛盾**：你的 x86 机器是小端，但网络传输要用大端。
所以每次在"主机内存"和"网络包"之间转换，都要做字节翻转。

### 1.5 一个生活中的类比

想象你要把"1234"这个数字写在一张张卡片上，每张卡片只能写一位。

- **大端写法**：从左到右写 `1 2 3 4`，第一张卡片是最高位 1。
  这是我们人类自然的阅读顺序。
- **小端写法**：从右到左写 `4 3 2 1`，第一张卡片是最低位 4。
  这是计算机算术运算更友好的顺序。

为什么计算机喜欢小端？因为加减法从低位开始算，
小端布局下"取第一个字节"就能拿到最低位字节，进位处理更自然。

---

## 2. 大端和小端的直观演示

### 2.1 用 C 代码检测当前机器的字节序

```c
int byte_order_detect(void) {
    uint32_t i = 1;
    return *(uint8_t *)&i;
}
```

**原理**：

```
大端机器：i=1 的内存是 [01 00 00 00]，取第一个字节 = 1
小端机器：i=1 的内存是 [00 00 00 01]，取第一个字节 = 0
```

返回 1 是大端，0 是小端。

### 2.2 看一个 4 字节整数的内存布局

```c
uint32_t val = 0x01020304;
byte_order_print_hex("val", &val, 4);
```

在小端机器上输出：

```
  val                  : 04 03 02 01
```

在大端机器上输出：

```
  val                  : 01 02 03 04
```

### 2.3 端口 8080 的例子

端口 `8080 = 0x1F90`。

```c
uint16_t port = 8080;          // 0x1F90
byte_order_print_hex("host", &port, 2);

uint16_t port_net = htons(port);
byte_order_print_hex("network", &port_net, 2);
```

小端机器输出：

```
  host                 : 90 1f        ← 小端：低位 90 在前
  network              : 1f 90        ← 大端：高位 1F 在前
```

**如果直接把主机的 `90 1f` 发到网络上**，对端按大端解释：
`0x901F = 36943`，完全不是 8080！

这就是为什么**每个多字节字段在发往网络前都要 `htons`**。

### 2.4 一个完整的字节序演示程序

```c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

static void print_hex(const char *name, const void *p, size_t n)
{
    printf("  %-20s: ", name);
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        printf("%02x ", b[i]);
    }
    printf("\n");
}

int main(void)
{
    /* 1. 检测字节序 */
    uint32_t one = 1;
    int is_little = (*(uint8_t *)&one) == 1;
    printf("本机字节序: %s\n\n", is_little ? "小端" : "大端");

    /* 2. 32位整数 */
    uint32_t val = 0x01020304;
    print_hex("0x01020304 (host)", &val, 4);
    uint32_t val_net = htonl(val);
    print_hex("0x01020304 (network)", &val_net, 4);
    printf("  ntohl 转回 = 0x%08X\n\n", ntohl(val_net));

    /* 3. 端口 */
    uint16_t port = 8080;
    print_hex("8080 (host)", &port, 2);
    uint16_t port_net = htons(port);
    print_hex("8080 (network)", &port_net, 2);
    printf("  ntohs 转回 = %u\n\n", ntohs(port_net));

    /* 4. IP 地址 */
    uint32_t ip = 0xC0A80001;  /* 192.168.0.1 */
    print_hex("192.168.0.1 (host)", &ip, 4);
    uint32_t ip_net = htonl(ip);
    print_hex("192.168.0.1 (network)", &ip_net, 4);

    return 0;
}
```

在小端 x86 机器上运行：

```
本机字节序: 小端

  0x01020304 (host)    : 04 03 02 01
  0x01020304 (network) : 01 02 03 04
  ntohl 转回 = 0x01020304

  8080 (host)          : 90 1f
  8080 (network)       : 1f 90
  ntohs 转回 = 8080

  192.168.0.1 (host)   : 01 00 a8 c0
  192.168.0.1 (network): c0 a8 00 01
```

注意 IP `192.168.0.1 = 0xC0A80001`，小端内存是 `01 00 a8 c0`，
htonl 后变成 `c0 a8 00 01`，正好是我们写 IP 地址时从左到右的顺序。

---

## 3. 为什么网络要用大端

### 3.1 历史原因

TCP/IP 协议在 1970 年代由 Vint Cerf 和 Bob Kahn 设计。
当时的主流计算机（如 IBM 大型机、Motorola 68000）是大端。
协议设计者选了大端作为"网络字节序"（Network Byte Order）。

### 3.2 术语

- **Network Byte Order** = 大端（Big-Endian）
- **Host Byte Order** = 当前机器的字节序（可能是大端或小端）

### 3.3 转换函数的命名

```
h = host（主机）
n = network（网络）
s = short（16 位）
l = long（32 位）
```

| 函数 | 方向 | 用途 |
|------|------|------|
| `htons` | host → network (16位) | 发送前转换端口 |
| `ntohs` | network → host (16位) | 接收后转换端口 |
| `htonl` | host → network (32位) | 发送前转换 IP 地址 |
| `ntohl` | network → host (32位) | 接收后转换 IP 地址 |

### 3.4 在大端机器上这些函数做什么？

**什么都不做**。大端机器的 host byte order 就是 network byte order，
`htons` 是个空操作（可能就是 `#define htons(x) (x)`）。

在小端机器上，它们会翻转字节顺序。

这就是为什么代码里**总是写 `htons`/`ntohs`** 而不是直接判断字节序——
这些函数会自动适配当前机器，代码可移植。

### 3.5 一个有趣的细节：htons 的实现

在 glibc 里，`htons` 通常是用宏或内联函数实现的，没有函数调用开销：

```c
/* glibc 的 bits/byteswap.h 大致是这样 */
#define htons(x) (__builtin_constant_p(x) ? __constant_htons(x) : __htons(x))

/* 小端机器上的实际操作 */
uint16_t __htons(uint16_t x) {
    return ((x & 0x00FF) << 8) | ((x & 0xFF00) >> 8);
}
```

`__builtin_constant_p` 是 GCC 内建函数，判断参数是否编译期常量。
如果是常量，编译期就转换好；否则生成运行时翻转代码。
所以 `htons(8080)` 在编译期就完成了，运行时零开销。

---

## 4. htons / ntohs / htonl / ntohl

### 4.1 什么时候需要转换？

| 场景 | 需要转换？ | 用什么 |
|------|-----------|--------|
| `struct sockaddr_in.sin_port` | 是 | `htons(port)` 赋值，`ntohs()` 读取 |
| `struct sockaddr_in.sin_addr.s_addr` | 看情况 | `inet_addr()`/`inet_pton()` 返回的已经是网络序，**不要再 htonl** |
| 自己解析 IP 头部的字段 | 是 | 从包里读出来后 `ntohs()`/`ntohl()` |
| 纯应用层数据（如 HTTP 文本） | 否 | 文本是单字节的，没有字节序问题 |
| 自己定义的二进制协议 | 是 | 发送前 htons/htonl，接收后 ntohs/ntohl |
| 文件存储多字节整数 | 看情况 | 取决于文件格式规定（如 BMP 是小端，PNG 是大端） |

### 4.2 最常见的错误：对 inet_addr 的结果再 htonl

```c
// ❌ 错误！inet_addr 返回的已经是网络字节序
addr.sin_addr.s_addr = htonl(inet_addr("127.0.0.1"));

// ✅ 正确
addr.sin_addr.s_addr = inet_addr("127.0.0.1");
// 或者
inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
```

`inet_addr` 和 `inet_pton` 的文档明确说返回值是 network byte order。
如果你再 `htonl` 一次，就转了两次，在小端机器上等于没转，值就错了。

### 4.3 自己解析网络包时的规则

从网络包里读出来的每个多字节字段，都是**网络字节序（大端）**。
要转成主机字节序才能正确解释：

```c
struct ip_hdr *ip = (struct ip_hdr *)packet;

uint16_t total_len = ntohs(ip->total_len);  // ✅ 转了才能用
uint32_t src_ip   = ip->src_ip;             // 不转也能用，因为 inet_ntop 期望网络序
```

注意 `src_ip` 不转的原因：`inet_ntop` 函数**期望参数是网络字节序的**，
它内部会自己处理。所以"要不要转"取决于你下一步用什么函数读它。

### 4.4 一个完整的端口绑定示例

```c
int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

struct sockaddr_in addr;
addr.sin_family      = AF_INET;
addr.sin_port        = htons(8080);               /* ✅ 端口要 htons */
addr.sin_addr.s_addr = htonl(INADDR_ANY);         /* INADDR_ANY = 0，htonl(0)=0，写不写都行 */

bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
listen(listen_fd, 128);

/* 读取对端端口时 */
struct sockaddr_in peer;
socklen_t peer_len = sizeof(peer);
getpeername(conn_fd, (struct sockaddr *)&peer, &peer_len);
int peer_port = ntohs(peer.sin_port);             /* ✅ 读取要 ntohs */
```

注意 `htonl(INADDR_ANY)` 是个习惯写法，因为 `INADDR_ANY = 0`，
而 `htonl(0) = 0`，转不转都一样。但写上 `htonl` 更明确表达意图。

---

## 5. 结构体对齐：packed 的必要性

### 5.1 编译器为什么要对齐

默认情况下，编译器会在结构体字段间插入**填充字节**，
让每个字段对齐到自然边界（通常是其大小的整数倍）。
这样 CPU 读取更高效（不对齐的访问在某些架构上会触发异常）。

```c
struct example {
    uint8_t  a;    // 1 字节，偏移 0
    // 编译器插入 3 字节填充，让 b 对齐到 4 字节边界
    uint32_t b;    // 4 字节，偏移 4
};
// 总大小 = 8 字节（1 + 3填充 + 4）
```

### 5.2 对网络包的影响

网络包是**紧凑排列**的，字段间没有填充。
如果用默认对齐的结构体去解析网络包：

```c
struct ip_hdr_unpacked {
    uint8_t  version_ihl;  // 偏移 0
    uint8_t  tos;          // 偏移 1
    uint16_t total_len;    // 偏移 2（恰好对齐，没问题）
    uint16_t id;           // 偏移 4
    uint16_t frag_off;     // 偏移 6
    uint8_t  ttl;          // 偏移 8
    uint8_t  protocol;     // 偏移 9
    uint16_t checksum;     // 偏移 10
    uint32_t src_ip;       // 偏移 12（恰好对齐）
    uint32_t dst_ip;       // 偏移 16
};
// 总大小 = 20 字节，恰好和 IP 头一样
```

IP 头恰好 20 字节且字段天然对齐，所以不 packed 也能用。
**但 TCP 头不是**：

```c
struct tcp_hdr_unpacked {
    uint16_t src_port;     // 偏移 0
    uint16_t dst_port;     // 偏移 2
    uint32_t seq;          // 偏移 4
    uint32_t ack_seq;      // 偏移 8
    uint16_t flags_off;    // 偏移 12（data_off + flags 合并）
    uint16_t window;       // 偏移 14
    uint16_t checksum;     // 偏移 16
    uint16_t urg_ptr;      // 偏移 18
};
// 总大小 = 20 字节，也恰好对齐
```

**但如果你用位域拆 flags**，就可能出问题：

```c
struct bad_tcp_flags {
    uint8_t fin:1;
    uint8_t syn:1;
    uint8_t rst:1;
    uint8_t psh:1;
    uint8_t ack:1;
    uint8_t urg:1;
    uint8_t res1:2;
    // 这个结构体占 1 字节
    // 但 TCP 头里 flags 紧跟在 data_off 后面，只有 8 bit
    // 如果前面有填充，就错位了
};
```

### 5.3 解决方案：__attribute__((packed))

```c
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t flags_off;
    uint16_t window;
    uint16_t checksum;
    uint16_t urg_ptr;
} __attribute__((packed));
```

`packed` 告诉编译器：**不要插入任何填充，按实际大小紧凑排列**。

**代价**：访问未对齐的字段在某些架构上会慢（x86 硬件支持非对齐访问，
但 ARM 可能触发异常或变慢）。对网络包解析来说，正确性优先于性能。

### 5.4 验证大小

```c
printf("packed:   %zu\n", sizeof(struct packed_struct));   // 5
printf("unpacked: %zu\n", sizeof(struct unpacked_struct)); // 8
```

在 `byte_order_demo()` 里有这个验证，运行时能看到。

### 5.5 一个会出错的例子

```c
/* 假设有个自定义协议，1 字节 type 后跟 4 字节 length */
struct my_proto_bad {
    uint8_t  type;       // 偏移 0
                        // 3 字节填充
    uint32_t length;     // 偏移 4
};  // sizeof = 8

struct my_proto_good {
    uint8_t  type;
    uint32_t length;
} __attribute__((packed));  // sizeof = 5

/* 网络包是 5 字节：[type][length 4 字节] */
uint8_t packet[5] = {0x01, 0x00, 0x00, 0x10, 0x00};  /* length = 16 大端 */

/* 用 bad 结构体解析 */
struct my_proto_bad *bad = (struct my_proto_bad *)packet;
/* bad->type = 0x01 ✓ */
/* bad->length 在偏移 4，但 packet 只有 5 字节，读到的是 packet[4..7] */
/* 越界！而且即使不越界，偏移 4 也不是 length 字段的位置 */

/* 用 good 结构体解析 */
struct my_proto_good *good = (struct my_proto_good *)packet;
/* good->type = 0x01 ✓ */
/* good->length 在偏移 1，是 packet[1..4] = 00 00 10 00 */
/* ntohl(good->length) = 0x00001000 = 4096 */
```

这个例子说明：**结构体大小和字段偏移必须和网络包的布局完全一致**，
否则解析出来全是错的。

---

## 6. 位域与字节序的交互

### 6.1 什么是位域

C 允许在结构体里按**位**分配字段：

```c
struct ip_hdr {
    uint8_t ihl:4;      // 4 bit
    uint8_t version:4;  // 4 bit
    // 合起来占 1 字节
};
```

### 6.2 位域的排列方向依赖字节序

这是最容易出 bug 的地方。

IP 头第一个字节是：

```
 0   1   2   3   4   5   6   7
+---+---+---+---+---+---+---+---+
|  Version  |    IHL    |
|  (4 bit)  |  (4 bit)  |
+---+---+---+---+---+---+---+---+
```

在大端机器上，位域从高位开始排：

```c
// 大端
struct ip_hdr {
    uint8_t version:4;  // 先定义 → 高 4 位
    uint8_t ihl:4;      // 后定义 → 低 4 位
};
```

在小端机器上，位域从低位开始排：

```c
// 小端
struct ip_hdr {
    uint8_t ihl:4;      // 先定义 → 低 4 位
    uint8_t version:4;  // 后定义 → 高 4 位
};
```

### 6.3 解决方案：用条件编译

```c
struct ip_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t ihl:4;
    uint8_t version:4;
#else
    uint8_t version:4;
    uint8_t ihl:4;
#endif
    ...
} __attribute__((packed));
```

这就是 `sniff.c` 里 IP 和 TCP 头结构体的写法。
`__BYTE_ORDER` 在 `<endian.h>` 里定义。

### 6.4 替代方案：不用位域

位域的排列方向是**实现定义**的（虽然 GCC 行为一致）。
更可移植的做法是用位运算：

```c
uint8_t version_ihl = packet[0];
int version = (version_ihl >> 4) & 0x0F;  // 高 4 位
int ihl     = version_ihl & 0x0F;          // 低 4 位
```

这种方式不依赖字节序，完全可移植。
教学项目里我们用位域（更直观），但你要知道位运算的写法。

### 6.5 提取 TCP flags 的位运算写法

TCP 头第 13 字节（从 0 开始）是 flags：

```c
uint8_t flags = tcp_byte_13;
int fin = (flags >> 0) & 0x01;  // 第 0 位
int syn = (flags >> 1) & 0x01;  // 第 1 位
int rst = (flags >> 2) & 0x01;  // 第 2 位
int psh = (flags >> 3) & 0x01;  // 第 3 位
int ack = (flags >> 4) & 0x01;  // 第 4 位
int urg = (flags >> 5) & 0x01;  // 第 5 位
```

或者更简洁：

```c
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

if (flags & TCP_SYN) printf("SYN ");
if (flags & TCP_ACK) printf("ACK ");
```

---

## 7. 常见陷阱

### 7.1 陷阱一：忘了 htons

```c
// ❌ 端口直接赋值
addr.sin_port = 8080;

// ✅ 要 htons
addr.sin_port = htons(8080);
```

在小端机器上，`addr.sin_port = 8080` 会在内存里存 `90 1f`，
内核按网络序解释成 `0x901F = 36943`，绑定到错误的端口。

### 7.2 陷阱二：对 inet_addr 的结果再 htonl

```c
// ❌ 双重转换
addr.sin_addr.s_addr = htonl(inet_addr("127.0.0.1"));

// ✅ inet_addr 已经返回网络序
addr.sin_addr.s_addr = inet_addr("127.0.0.1");
```

### 7.3 陷阱三：结构体没 packed

```c
// ❌ 可能在字段间出现填充
struct ip_hdr {
    uint8_t  version_ihl;
    uint8_t  tos;
    ...
};

// ✅ packed
struct ip_hdr {
    ...
} __attribute__((packed));
```

### 7.4 陷阱四：用 memcpy 跨机器传输结构体

```c
// ❌ 不同机器的字节序/对齐可能不同
struct data d = {...};
send(fd, &d, sizeof(d), 0);

// ✅ 逐字段序列化，每个字段用 htons/htonl
uint16_t port_net = htons(d.port);
uint32_t ip_net   = htonl(d.ip);
send(fd, &port_net, 2, 0);
send(fd, &ip_net,   4, 0);
```

### 7.5 陷阱五：位域顺序写反

```c
// 在小端机器上，这样写 version 会跑到低 4 位
struct ip_hdr {
    uint8_t version:4;  // ❌ 小端上这是低 4 位
    uint8_t ihl:4;
};

// 要用条件编译，见 6.3
```

### 7.6 陷阱六：打印 IP 地址用 %d

```c
uint32_t ip = ip_hdr->src_ip;  /* 网络字节序 */

// ❌ 直接用 %u 打印，得到的是个奇怪的数字
printf("%u\n", ip);

// ✅ 用 inet_ntop 转成字符串
char buf[INET_ADDRSTRLEN];
inet_ntop(AF_INET, &ip, buf, sizeof(buf));
printf("%s\n", buf);  /* 192.168.0.1 */

// ✅ 或者先 ntohl 再拆分（不推荐，太啰嗦）
uint32_t host_ip = ntohl(ip);
printf("%d.%d.%d.%d\n",
    (host_ip >> 24) & 0xFF,
    (host_ip >> 16) & 0xFF,
    (host_ip >> 8)  & 0xFF,
    host_ip         & 0xFF);
```

### 7.7 陷阱七：htonl 当函数调用而非宏

```c
// ❌ 想取地址
uint32_t (*func)(uint32_t) = htonl;  /* htonl 是宏，不能取地址 */

// ✅ 直接用
uint32_t net_val = htonl(host_val);
```

### 7.8 陷阱八：64 位整数没有 htonll

标准库只提供 16/32 位的转换函数，64 位要自己写：

```c
uint64_t htonll(uint64_t x) {
    /* 检测字节序 */
    uint32_t test = 1;
    if (*(uint8_t *)&test == 1) {
        /* 小端，需要翻转 */
        return ((uint64_t)htonl(x & 0xFFFFFFFF) << 32) | htonl(x >> 32);
    }
    return x;  /* 大端，什么都不做 */
}

uint64_t ntohll(uint64_t x) {
    return htonll(x);  /* 转换是对称的 */
}
```

---

## 8. 动手实验

### 8.1 运行字节序演示

编译后运行 `raw_sniff`，它会先执行 `byte_order_demo()`：

```
==========================================
  字节序演示
==========================================

1. 当前主机字节序: 小端 (Little-Endian)

2. 16 位端口转换 (htons):
   端口值 = 8080 (0x1F90)
     主机字节序内存      : 90 1f
     网络字节序内存      : 1f 90
   ntohs 转回来 = 8080

3. 32 位 IP 地址转换 (htonl):
   IP = 127.0.0.1 (0x7F000001)
     主机字节序内存      : 01 00 00 7f
     网络字节序内存      : 7f 00 00 01
   ntohl 转回来 = 0x7F000001

4. 结构体对齐 (packed vs 默认):
   packed   struct 大小 = 5 (1 + 4 = 5，无填充)
   unpacked struct 大小 = 8 (a 后填充 3 字节对齐到 4)
   → 解析网络包必须用 packed，否则字段错位！
```

### 8.2 思考题

1. 为什么 `inet_addr("127.0.0.1")` 返回的值在内存里是 `01 00 00 7f` 而不是 `7f 00 00 01`？
   （提示：它返回的是网络字节序，在小端机器上内存布局是怎样的？）

2. 如果你在代码里写 `addr.sin_port = 8080`（忘了 htons），在小端机器上实际绑定的是哪个端口？
   （提示：8080 = 0x1F90，小端内存是 `90 1F`，内核按大端解释成多少？）

3. 为什么 `sizeof(struct packed_struct)` 是 5 而不是 8？
   （提示：packed 去掉了填充）

4. 如果不用位域，怎么从 IP 头第一个字节提取 version 和 ihl？
   （提示：位运算 `>> 4` 和 `& 0x0F`）

---

## 9. 字节序的历史背景

### 9.1 Endian 这个词的来历

"Endian" 这个词来自乔纳森·斯威夫特（Jonathan Swift）1726 年的讽刺小说
《格列佛游记》。小说里小人国两派人因为打鸡蛋应该从大端敲还是小端敲而爆发战争。

- **Big-Endians**：坚持从大端打破鸡蛋
- **Little-Endians**：坚持从小端打破鸡蛋

Danny Cohen 在 1980 年的论文《On Holy Wars and a Plea for Peace》里
借用这个典故来描述计算机里字节序的争论。从此 "endian" 就成了正式术语。

### 9.2 为什么会有两种字节序？

这不是某个人拍脑袋决定的，而是不同硬件设计师做了不同的合理选择。

**小端派的理由**（Intel x86 选择小端）：

1. **算术运算友好**：加减法从低位开始，小端布局下最低位字节在地址 0，
   取第一个字节就能开始算，进位自然往高地址传。
2. **指针强转方便**：一个 32 位整数 `0x00000041`，小端内存是 `41 00 00 00`，
   如果你想当字符用，`(char *)&i` 直接取到 `0x41 = 'A'`。
3. **扩展位数自然**：8 位 `0x41` 扩展成 16 位，小端就是 `41 00`，
   原字节不动，后面补 0。大端要变成 `00 41`，原字节要移动。

**大端派的理由**（IBM、Motorola 选择大端）：

1. **人类阅读顺序**：`0x01020304` 内存是 `01 02 03 04`，
   和我们写数字的顺序一致，hexdump 看起来直观。
2. **网络传输顺序**：先发高位字节，接收方可以"边收边比较"，
   比如比较两个数的大小，收到第一个字节就能判断个大概。
3. **历史惯性**：IBM 大型机从 1960 年代就是大端，大量遗留代码依赖大端。

### 9.3 字节序之争的本质

Cohen 论文的核心观点：**两种选择都合理，但必须统一**。
网络协议选了大端，x86 选了小端，所以需要转换函数。
这不是技术问题，是"先有鸡还是先有蛋"的历史问题。

### 9.4 现代处理器的字节序

| 处理器 | 默认字节序 | 可切换？ | 说明 |
|--------|-----------|---------|------|
| x86 / x86-64 | 小端 | 否 | Intel 坚定的小端派 |
| ARM | 小端 | 是（罕见） | ARMv4 之前可配置，现在几乎都是小端 |
| ARM64 | 小端 | 否 | ARMv8 固定小端 |
| PowerPC | 大端 | 是 | IBM Power 可切换 |
| SPARC | 大端 | 是（v9+） | Sun 的招牌 |
| MIPS | 可配置 | 是 | 嵌入式常用，开机时设置 |
| RISC-V | 小端 | 否 | 新兴指令集，选了小端 |
| Itanium | 可配置 | 是 | 已被市场淘汰 |

**趋势**：小端正在统一天下。x86 主导 PC/服务器，ARM 主导移动/嵌入式，
都是小端。大端机器在新开发里越来越少见了。

### 9.5 Bi-Endian 处理器

有些处理器（ARM、PowerPC、MIPS）支持两种字节序，启动时选择。
这听起来很美好，实际上很痛苦：

- 操作系统必须针对特定字节序编译
- 驱动程序要处理外设的字节序（外设寄存器可能是大端或小端）
- 跨字节序的二进制不兼容

所以实际中几乎没人切换字节序，处理器固定在一个模式。

---

## 10. 网络字节序的由来

### 10.1 TCP/IP 的诞生

1973 年，Vint Cerf 和 Bob Kahn 开始设计 TCP/IP。
1974 年发表 RFC 675，1978 年 TCP 和 IP 分离，1981 年 RFC 791 (IP) 和 RFC 793 (TCP) 定稿。

当时的目标：把各种不同的网络（ARPANET、PRNET、SATNET 等）和各种不同的计算机连起来。

### 10.2 为什么选大端？

Cerf 在后来回忆说，选择大端主要是：

1. **当时主流是大端**：IBM 360/370、DEC PDP-10、Motorola 68000 都是大端。
   这些是 1970 年代的主流计算机。
2. **IBM 的位编号习惯**：IBM 把最高位编号为 bit 0，最低位编号为 bit 31。
   这种编号下大端布局更自然。
3. **没有强烈理由选小端**：Intel 8080（1974 年）虽然是小端，但当时还没主导市场。

### 10.3 RFC 791 的原文

RFC 791 (IP) 里明确写：

> The convention in the Internet documentation is to express numbers in decimal
> and to picture data in binary. ...
> When a high order bit is ...
> The most important convention is that ...
> **The order of transmission of the header and data is "big endian."**

"big endian" 这个词在 RFC 里没直接出现（Cohen 论文 1980 年才提出），
但描述的就是大端：高位字节先传输。

### 10.4 网络字节序的精确定义

RFC 1700 (Assigned Numbers) 里定义：

> **Byte Order**
> The convention in the Internet is that packets are sent in "big-endian" order.
> ...
> The terms "network byte order" and "host byte order" are used to distinguish
> the byte order used in the network protocols from that used in the host computer.

简单说：**网络字节序 = 大端**，这是 TCP/IP 协议栈的硬性规定。

### 10.5 为什么不强制所有主机也用大端？

理论上可以让所有计算机都用大端，就不需要转换函数了。
但实际上：

1. **硬件设计自由**：让硬件设计师根据 CPU 的需求选择字节序，不强制。
2. **转换开销极小**：`htons` 就是一条 `bswap` 指令，纳秒级。
3. **历史无法统一**：Intel 已经卖了上亿片小端 x86，不可能让它改。

所以方案是：**网络层统一大端，主机层各自决定，转换函数桥接**。

### 10.6 一个真实的例子：为什么这很重要

假设你写一个网络协议，要发送一个 32 位整数 `count = 1000`。

```c
/* ❌ 直接发送 */
uint32_t count = 1000;
send(fd, &count, 4, 0);
```

在小端机器上，发出去的字节是 `E8 03 00 00`。
大端机器收到后，按大端解释成 `0xE8030000 = 3892314112`，完全错误！

```c
/* ✅ 转成网络序再发 */
uint32_t count = 1000;
uint32_t count_net = htonl(count);
send(fd, &count_net, 4, 0);
```

现在发出去的是 `00 00 03 E8`，任何机器收到后 `ntohl()` 都得到 1000。

---

## 11. 抓包中看到的字节序

### 11.1 用 tcpdump 看端口

```bash
sudo tcpdump -i lo port 8080 -nn -x
```

输出：

```
12:00:00.123456 IP 127.0.0.1.54321 > 127.0.0.1.8080: Flags [S], seq 123456789
    0x0000:  4500 003c 0001 4000 4006 0000 7f00 0001
    0x0010:  7f00 0001 d431 1f90 075b cd15 0000 0000
    ...
```

看 `0x0010` 行的开头 `7f00 0001`，这是源 IP `127.0.0.1`（大端存储）。
接着 `d431 1f90`：`d431` 是源端口 54321（大端），`1f90` 是目的端口 8080（大端）。

验证：`0x1f90 = 8080` ✓，`0xd431 = 54321` ✓。

### 11.2 用 xxd 看字节

```bash
echo -n "GET / HTTP/1.1" | xxd
```

```
00000000: 4745 5420 2f20 4854 5450 2f31 2e31       GET / HTTP/1.1
```

ASCII 文本是单字节的，没有字节序问题。`47 45 54` 就是 `G E T`。

### 11.3 抓一个 DHCP 包看字节序

DHCP 包里有多种多字节字段，是观察字节序的好例子：

```
0x0000:  0201 06xx ...           /* op=2, htype=1, hlen=6 */
0x0004:  xxxx xxxx               /* xid (4 字节事务 ID) */
0x0008:  0000 0000               /* secs, flags */
0x000c:  0000 0000               /* ciaddr (client IP) */
0x0010:  c0a8 0101               /* yiaddr (your IP) = 192.168.1.1 */
```

`c0a8 0101` 是 `192.168.1.1` 的大端表示。
如果在小端机器上存这个 IP，内存里是 `01 01 a8 c0`。

### 11.4 一个抓包解析的完整例子

假设我们抓到一个 IP 包的前 20 字节：

```
45 00 00 3c 00 01 40 00 40 06 00 00 7f 00 00 01 7f 00 00 01
```

逐字节解析：

```
字节 0:  0x45
  version = 0x45 >> 4 = 4        (IPv4 ✓)
  ihl     = 0x45 & 0x0F = 5      (头长 5*4=20 字节)

字节 1:  0x00  (TOS = 0)

字节 2-3: 0x00 0x3c  (大端)
  total_len = 0x003c = 60 字节

字节 4-5: 0x00 0x01  (id = 1)

字节 6-7: 0x40 0x00
  flags = 0x40 >> 5 = 0b010  (DF=1, MF=0)
  frag_off = 0x4000 & 0x1FFF = 0

字节 8: 0x40  (TTL = 64)
字节 9: 0x06  (protocol = 6 = TCP)

字节 10-11: 0x00 0x00  (checksum，这里没算)

字节 12-15: 0x7f 0x00 0x00 0x01  (src = 127.0.0.1)
字节 16-19: 0x7f 0x00 0x00 0x01  (dst = 127.0.0.1)
```

注意所有多字节字段都是大端存储，和我们写数字的顺序一致。
这就是网络字节序。

### 11.5 在代码里验证

```c
const uint8_t packet[] = {
    0x45, 0x00, 0x00, 0x3c, 0x00, 0x01, 0x40, 0x00,
    0x40, 0x06, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x01,
    0x7f, 0x00, 0x00, 0x01
};

struct ip_hdr *ip = (struct ip_hdr *)packet;
printf("version    = %d\n", ip->version);              /* 4 */
printf("ihl        = %d\n", ip->ihl);                  /* 5 */
printf("total_len  = %d\n", ntohs(ip->total_len));     /* 60 */
printf("ttl        = %d\n", ip->ttl);                  /* 64 */
printf("protocol   = %d\n", ip->protocol);             /* 6 */

char src_str[INET_ADDRSTRLEN];
inet_ntop(AF_INET, &ip->src_ip, src_str, sizeof(src_str));
printf("src_ip     = %s\n", src_str);                  /* 127.0.0.1 */
```

---

## 12. 校验和计算详解

### 12.1 校验和的作用

网络传输会出错（电磁干扰、路由器 bug 等）。
校验和让接收方能检测到错误，丢弃坏包。

IP 头校验和：只校验 IP 头（20 字节），不校验数据。
TCP 校验和：校验 TCP 头 + TCP 数据 + 伪首部（伪首部含 IP 信息）。

### 12.2 校验和算法（RFC 1071）

算法叫"16 位反码求和"（one's complement sum）：

1. 把数据按 16 位（2 字节）一组划分
2. 所有 16 位数相加，溢出回卷（加到最低位）
3. 取反码作为校验和

```c
uint16_t checksum_compute(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;

    /* 按 16 位一组累加 */
    while (len > 1) {
        uint16_t word;
        memcpy(&word, data, 2);
        sum += word;
        data += 2;
        len  -= 2;
    }

    /* 如果长度是奇数，最后一个字节补 0 凑成 16 位 */
    if (len == 1) {
        uint16_t word = 0;
        ((uint8_t *)&word)[0] = *data;
        sum += word;
    }

    /* 溢出回卷 */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    /* 取反 */
    return (uint16_t)~sum;
}
```

### 12.3 为什么用反码求和？

普通加法有进位，16 位加法溢出的进位会丢失。
反码求和把进位"回卷"加到最低位，这样：

- 加法的顺序无关（交换律、结合律都成立）
- 字节序无关（大端小端算出来一样，这是个巧妙的设计）

字节序无关的原因：大端 `0x1234` 和小端 `0x3412`，反码求和后结果互为字节翻转，
最后取反再翻转回来，校验和位置上的值一致。

### 12.4 IP 头校验和的计算步骤

发送方：

1. 把 checksum 字段置 0
2. 对整个 IP 头（20 字节）算 16 位反码和
3. 取反填回 checksum 字段

接收方：

1. 对整个 IP 头（含 checksum）算 16 位反码和
2. 结果应该是 0xFFFF
3. 取反应该是 0x0000，不是就说明出错

```c
/* 发送方：计算并填充 IP 校验和 */
void ip_checksum_fill(struct ip_hdr *ip)
{
    ip->checksum = 0;  /* 先清零 */
    int hdr_len = ip->ihl * 4;
    ip->checksum = checksum_compute((uint8_t *)ip, hdr_len);
}

/* 接收方：验证 IP 校验和 */
int ip_checksum_verify(const struct ip_hdr *ip)
{
    int hdr_len = ip->ihl * 4;
    uint16_t sum = checksum_compute((const uint8_t *)ip, hdr_len);
    return sum == 0;  /* 正确时应该是 0 */
}
```

### 12.5 一个 IP 校验和的计算实例

IP 头（不含 checksum，先填 0）：

```
45 00 00 3c 00 01 40 00 40 06 00 00 7f 00 00 01 7f 00 00 01
```

按 16 位分组（大端）：

```
4500 + 003c + 0001 + 4000 + 4006 + 0000 + 7f00 + 0001 + 7f00 + 0001
```

计算：

```
4500 + 003c = 453c
453c + 0001 = 453d
453d + 4000 = 853d
853d + 4006 = c543
c543 + 0000 = c543
c543 + 7f00 = 14443  → 回卷: 4443 + 1 = 4444
4444 + 0001 = 4445
4445 + 7f00 = c345
c345 + 0001 = c346
```

取反：`~c346 = 3cb9`

所以 checksum 字段填 `3cb9`，在包里就是 `3c b9`。

### 12.6 TCP 伪首部校验和

TCP 校验和比 IP 复杂，要加上一个"伪首部"（pseudo header）：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Source Address                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Destination Address                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  zero  |    Protocol   |          TCP Length                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

伪首部 12 字节：源 IP(4) + 目的 IP(4) + 0(1) + 协议(1) + TCP 长度(2)。

**为什么要伪首部？**

如果只校验 TCP 内容，接收方无法确认这个包是不是给自己的。
伪首部包含目的 IP，校验和能验证"这个包确实到了正确的 IP"。
这能防止某些路由错误把包送到错的地方。

```c
uint16_t tcp_checksum_compute(const struct ip_hdr *ip,
                              const struct tcp_hdr *tcp,
                              const uint8_t *tcp_data,
                              size_t tcp_data_len)
{
    /* 1. 构造伪首部 */
    struct {
        uint32_t src_ip;
        uint32_t dst_ip;
        uint8_t  zero;
        uint8_t  protocol;
        uint16_t tcp_len;
    } __attribute__((packed)) pseudo;

    pseudo.src_ip    = ip->src_ip;
    pseudo.dst_ip    = ip->dst_ip;
    pseudo.zero      = 0;
    pseudo.protocol  = 6;  /* TCP */
    uint16_t total_tcp_len = ntohs(tcp->data_off) ... /* 头+数据 */
    pseudo.tcp_len   = htons(total_tcp_len);

    /* 2. 分段累加：伪首部 + TCP 头 + TCP 数据 */
    uint32_t sum = 0;

    /* 累加伪首部 */
    sum += checksum_raw(&pseudo, sizeof(pseudo));

    /* 累加 TCP 头（checksum 字段先清零） */
    struct tcp_hdr tcp_copy = *tcp;
    tcp_copy.checksum = 0;
    sum += checksum_raw(&tcp_copy, sizeof(tcp_copy));

    /* 累加 TCP 数据 */
    sum += checksum_raw(tcp_data, tcp_data_len);

    /* 3. 回卷 + 取反 */
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
```

### 12.7 UDP 也有伪首部校验和

UDP 校验和是可选的（可以填 0 表示不校验），但 TCP 是强制的。
算法和 TCP 一样，只是 protocol 字段填 17 (UDP)。

### 12.8 校验和的局限性

校验和只是 16 位反码和，能检测单比特错误和一些多比特错误，
但**不能检测顺序交换**（比如两个 16 位字互换，校验和不变）。
更可靠的错误检测要靠 CRC（以太网帧用 CRC32）。

---

## 13. 跨平台字节序处理

### 13.1 用预处理器宏检测

```c
#include <endian.h>  /* Linux */

#if __BYTE_ORDER == __LITTLE_ENDIAN
    /* 小端代码 */
#elif __BYTE_ORDER == __BIG_ENDIAN
    /* 大端代码 */
#else
    #error "未知字节序"
#endif
```

不同平台的宏定义不同：

```c
/* Linux glibc */
#include <endian.h>
/* __BYTE_ORDER, __LITTLE_ENDIAN, __BIG_ENDIAN */

/* FreeBSD / macOS */
#include <machine/endian.h>
/* _BYTE_ORDER, _LITTLE_ENDIAN, _BIG_ENDIAN */

/* Windows */
/* 没有标准宏，但 Windows 只跑在 x86/x64 上，一定是小端 */
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __BYTE_ORDER    __LITTLE_ENDIAN
```

### 13.2 跨平台兼容写法

```c
#if defined(__linux__)
    #include <endian.h>
    #define IS_LITTLE_ENDIAN (__BYTE_ORDER == __LITTLE_ENDIAN)
#elif defined(__APPLE__)
    #include <machine/endian.h>
    #define IS_LITTLE_ENDIAN (_BYTE_ORDER == _LITTLE_ENDIAN)
#elif defined(_WIN32)
    #define IS_LITTLE_ENDIAN 1  /* Windows 永远小端 */
#else
    /* 运行时检测 */
    #define IS_LITTLE_ENDIAN ({ uint32_t t=1; (*(uint8_t*)&t)==1; })
#endif
```

### 13.3 用 CMake 在编译期检测

```cmake
include(TestBigEndian)
test_big_endian(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
    add_definitions(-DBYTE_ORDER_BIG=1)
else()
    add_definitions(-DBYTE_ORDER_LITTLE=1)
endif()
```

### 13.4 不依赖字节序的代码

最可移植的写法是**完全不依赖字节序**：

```c
/* 读取一个 4 字节大端整数 */
uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           ((uint32_t)p[3]);
}

/* 写一个 4 字节大端整数 */
void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8)  & 0xFF;
    p[3] = v         & 0xFF;
}
```

这种写法不管机器是什么字节序，结果都正确。
代价是比直接强转慢一点（多几次移位），但完全可移植。

很多跨平台库（如 Google Protobuf）就用这种方式序列化。

### 13.5 字节序转换的标准化：C23

C23 标准引入了 `<stdbit.h>`，提供字节序检测和转换函数：

```c
#include <stdbit.h>

/* 检测 */
if (stdc_endian_little() == 1) { ... }

/* 转换 */
uint16_t be_val = stdc_memreverse16(host_val);
```

但 C23 还没普及，目前还是用 `htons`/`htonl` 最实际。

### 13.6 文件格式的字节序

不同文件格式规定不同字节序：

| 格式 | 字节序 | 说明 |
|------|--------|------|
| BMP | 小端 | Windows 格式 |
| WAV | 小端 | Windows 格式 |
| GIF | 小端 | CompuServe |
| PNG | 大端 | 网络友好 |
| JPEG | 大端 | 大端标记 |
| TIFF | 可配置 | 头部指明字节序 |
| ELF | 可配置 | 头部指明字节序 |
| Java class | 大端 | Java 虚拟机 |

读这些文件时要注意按对应字节序解析。
ELF 和 TIFF 比较聪明，文件头里会说明自己是哪种字节序。

---

## 14. union 检测字节序

### 14.1 用 union 代替强转

```c
union byte_order_union {
    uint32_t i;
    uint8_t  b[4];
};

int is_little_endian(void)
{
    union byte_order_union u;
    u.i = 1;
    return u.b[0] == 1;
}
```

这和 `*(uint8_t *)&i` 的效果一样，但用 union 更清晰、更安全（不违反严格别名规则）。

### 14.2 union 的内存布局

```c
union {
    uint32_t i;
    uint8_t  b[4];
} u;

u.i = 0x01020304;
```

小端机器：

```
u.b[0] = 0x04  (低地址)
u.b[1] = 0x03
u.b[2] = 0x02
u.b[3] = 0x01  (高地址)
```

大端机器：

```
u.b[0] = 0x01  (低地址)
u.b[1] = 0x02
u.b[2] = 0x03
u.b[3] = 0x04  (高地址)
```

### 14.3 用 union 做字节序转换

```c
union conv16 {
    uint16_t v;
    uint8_t  b[2];
};

uint16_t my_htons(uint16_t x)
{
    if (is_little_endian()) {
        union conv16 u;
        u.v = x;
        uint8_t tmp = u.b[0];  /* 交换两个字节 */
        u.b[0] = u.b[1];
        u.b[1] = tmp;
        return u.v;
    }
    return x;  /* 大端什么都不做 */
}
```

### 14.4 union 的其他妙用

union 常用于"同一块内存多种解释"：

```c
/* IEEE 754 浮点数的内部结构 */
union float_bits {
    float    f;
    uint32_t u;
};

void inspect_float(float x)
{
    union float_bits fb;
    fb.f = x;
    printf("float %f = 0x%08X\n", x, fb.u);
    printf("  sign     = %d\n", (fb.u >> 31) & 1);
    printf("  exponent = %d\n", (fb.u >> 23) & 0xFF);
    printf("  mantissa = 0x%06X\n", fb.u & 0x7FFFFF);
}

inspect_float(1.0f);   /* 0x3F800000 */
inspect_float(-1.0f);  /* 0xBF800000 */
inspect_float(0.0f);   /* 0x00000000 */
```

注意：用 union 跨类型访问是 C 标准允许的（C11 明确允许），
但 C++ 严格说不是。所以 C 里放心用，C++ 里要小心。

### 14.5 严格别名规则（Strict Aliasing）

```c
/* ❌ 违反严格别名，C 里可能错，C++ 里未定义 */
uint32_t i = 0x01020304;
uint8_t b = *(uint8_t *)&i;  /* 不同类型指针互转 */

/* ✅ 用 union 是合法的 */
union { uint32_t i; uint8_t b[4]; } u;
u.i = 0x01020304;
uint8_t b = u.b[0];
```

GCC 用 `-fstrict-aliasing`（默认开启 -O2）时，
违反严格别名可能导致优化出错误结果。union 是安全的替代方案。

---

## 15. 位域在不同编译器上的行为

### 15.1 位域的三个实现定义行为

C 标准把以下三点留给编译器决定（implementation-defined）：

1. **位域分配方向**：从高位还是低位开始
2. **跨存储单元**：一个位域能不能跨两个 int
3. **位域顺序**：先定义的在高位还是低位

这意味着**不同编译器的位域布局可能不同**！

### 15.2 GCC 和 Clang 的行为

GCC 和 Clang 行为一致（Clang 故意兼容 GCC）：

- 位域分配方向**依赖字节序**
- 小端机器：先定义的位域在低位
- 大端机器：先定义的位域在高位
- 不跨存储单元（如果剩余 bit 不够，跳到下一个单元）

```c
struct bits {
    uint8_t a: 3;
    uint8_t b: 5;
};

/* 小端机器：a 在低 3 位，b 在高 5 位 */
/* 大端机器：a 在高 3 位，b 在低 5 位 */
```

### 15.3 MSVC 的行为

MSVC（Visual Studio）的行为：

- **不管字节序**，位域总是从低位开始分配
- 但 x86 永远是小端，所以实际上和 GCC 小端一致

所以 x86 上 MSVC 和 GCC 的位域布局一致。
但跨平台（比如 ARM 大端）就可能不同。

### 15.4 一个测试程序

```c
#include <stdio.h>
#include <stdint.h>

struct bitfield_test {
    uint8_t a: 4;
    uint8_t b: 4;
};

int main(void)
{
    struct bitfield_test t;
    t.a = 1;  /* 0b0001 */
    t.b = 2;  /* 0b0010 */

    uint8_t *p = (uint8_t *)&t;
    printf("byte = 0x%02X\n", *p);

    /* 小端 GCC/Clang: a 在低 4 位，b 在高 4 位 → 0x21 */
    /* 大端 GCC/Clang: a 在高 4 位，b 在低 4 位 → 0x12 */
    /* MSVC (x86):     和小端 GCC 一致 → 0x21 */

    return 0;
}
```

### 15.5 跨编译器的安全做法

如果代码要在不同编译器上跑，**不要用位域**，用位运算：

```c
/* 不依赖编译器的 flags 提取 */
uint8_t byte = packet[13];  /* TCP flags 字节 */
int fin = byte & 0x01;
int syn = byte & 0x02;
int rst = byte & 0x04;
int psh = byte & 0x08;
int ack = byte & 0x10;
int urg = byte & 0x20;
```

位运算的结果完全由你写的表达式决定，和编译器无关。

### 15.6 什么时候用位域是安全的？

1. **只在同一个编译器上跑**：比如 Linux 内核只用 GCC，位域布局一致
2. **不跨网络传输**：位域结构体只在本机内存里用，不 send/recv
3. **不跨字节序**：确定代码只跑在一种字节序的机器上

网络协议解析属于"跨网络传输"，所以要么用条件编译，要么用位运算。

### 15.7 位域的大小

```c
struct s1 { uint8_t a: 1; };              /* sizeof = 1 */
struct s2 { uint8_t a: 1; uint8_t b: 1; }; /* sizeof = 1 */
struct s3 { uint8_t a: 8; };              /* sizeof = 1 */
struct s4 { uint8_t a: 9; };              /* 编译错误！uint8_t 装不下 9 bit */
struct s5 { uint16_t a: 9; };             /* sizeof = 2 */
struct s6 { uint32_t a: 1; };             /* sizeof = 4，浪费 31 bit */
```

位域的存储单元是它声明的类型。`uint8_t a:1` 占 1 字节，
`uint32_t a:1` 占 4 字节。所以位域不省内存，只是省字段名。

---

## 16. 结构体对齐的详细规则

### 16.1 自然对齐（Natural Alignment）

每个类型有个对齐要求（alignment requirement）：

| 类型 | 大小 | 对齐 |
|------|------|------|
| char | 1 | 1 |
| short | 2 | 2 |
| int | 4 | 4 |
| long (32位) | 4 | 4 |
| long (64位) | 8 | 8 |
| float | 4 | 4 |
| double | 8 | 8 |
| 指针 (32位) | 4 | 4 |
| 指针 (64位) | 8 | 8 |

**规则**：N 字节类型的变量必须放在 N 的整数倍地址上。

### 16.2 结构体对齐的三条规则

1. **字段对齐**：每个字段的偏移必须是其对齐要求的整数倍
2. **填充**：不满足时在前面插入填充字节
3. **整体对齐**：结构体大小必须是其最大字段对齐的整数倍（尾部可能填充）

### 16.3 一个详细的对齐分析

```c
struct demo {
    char    a;    /* 1 字节，对齐 1 */
    int     b;    /* 4 字节，对齐 4 */
    short   c;    /* 2 字节，对齐 2 */
    char    d;    /* 1 字节，对齐 1 */
};
```

布局分析（x86-64）：

```
偏移 0:  a (char, 1 字节)
偏移 1-3: 填充 3 字节（让 b 对齐到 4）
偏移 4-7: b (int, 4 字节)
偏移 8-9: c (short, 2 字节)
偏移 10:  d (char, 1 字节)
偏移 11:  填充 1 字节（让结构体大小是 4 的倍数）

总大小: 12 字节
```

验证：

```c
printf("sizeof = %zu\n", sizeof(struct demo));        /* 12 */
printf("offsetof a = %zu\n", offsetof(struct demo, a)); /* 0 */
printf("offsetof b = %zu\n", offsetof(struct demo, b)); /* 4 */
printf("offsetof c = %zu\n", offsetof(struct demo, c)); /* 8 */
printf("offsetof d = %zu\n", offsetof(struct demo, d)); /* 10 */
```

### 16.4 重新排列字段节省空间

```c
struct bad_order {
    char  a;    /* 1 + 3 填充 */
    int   b;    /* 4 */
    char  c;    /* 1 + 3 填充 */
    int   d;    /* 4 */
};  /* sizeof = 16 */

struct good_order {
    int   b;    /* 4 */
    int   d;    /* 4 */
    char  a;    /* 1 */
    char  c;    /* 1 + 2 填充 */
};  /* sizeof = 12 */
```

**经验法则**：按字段大小从大到小排列，能减少填充。

### 16.5 指定对齐：#pragma pack

```c
#pragma pack(push, 1)  /* 1 字节对齐（即不对齐） */
struct packed_demo {
    char  a;
    int   b;
    short c;
    char  d;
};
#pragma pack(pop)  /* 恢复默认 */

/* sizeof(struct packed_demo) = 1 + 4 + 2 + 1 = 8 */
```

`#pragma pack(1)` 等价于 `__attribute__((packed))`，不同编译器语法不同：

```c
/* GCC/Clang */
struct s { ... } __attribute__((packed));

/* MSVC */
#pragma pack(push, 1)
struct s { ... };
#pragma pack(pop)
```

### 16.6 嵌套结构体的对齐

```c
struct inner {
    char  x;
    int   y;
};  /* sizeof = 8, 对齐 = 4 */

struct outer {
    char       a;
    struct inner b;  /* inner 的对齐是 4 */
    char       c;
};
```

布局：

```
偏移 0:    a (char)
偏移 1-3:  填充（让 b 对齐到 4）
偏移 4-11: b (struct inner, 8 字节)
           b.x 在偏移 4, b.y 在偏移 8
偏移 12:   c (char)
偏移 13-15: 填充（让结构体大小是 4 的倍数）

sizeof = 16
```

嵌套结构体的对齐要求 = 其最大字段的对齐要求。

### 16.7 数组的对齐

```c
struct with_array {
    char  a;
    int   b[3];  /* 12 字节，对齐 4 */
    char  c;
};
/* sizeof = 1 + 3填充 + 12 + 1 + 3填充 = 20 */
```

数组的对齐 = 元素的对齐。数组元素之间没有填充（C 保证）。

### 16.8 用 alignas 指定对齐（C11）

```c
#include <stdalign.h>

struct over_aligned {
    alignas(16) int a;  /* a 对齐到 16 字节边界 */
    int b;
};
/* sizeof = 32（a 占 16, b 占 4 + 12 填充到 16 的倍数） */
```

用途：SSE/AVX 指令要求数据 16/32 字节对齐，否则崩溃。

### 16.9 用 offsetof 检查布局

```c
#include <stddef.h>

struct my_struct {
    char  a;
    int   b;
    short c;
};

printf("a at offset %zu\n", offsetof(struct my_struct, a));  /* 0 */
printf("b at offset %zu\n", offsetof(struct my_struct, b));  /* 4 */
printf("c at offset %zu\n", offsetof(struct my_struct, c));  /* 8 */
printf("total size %zu\n", sizeof(struct my_struct));        /* 12 */
```

`offsetof` 是个宏，编译期求出字段偏移。调试对齐问题时非常有用。

---

## 17. 内存对齐对性能的影响

### 17.1 为什么不对齐会慢？

x86 CPU 读取一个 4 字节 int 时，如果地址是 4 的倍数（对齐），
一次内存访问就能读到。如果不对齐，可能要两次访问再拼接。

```
对齐的 int (地址 4)：
  一次访问 [4, 8) → 拿到完整 int

不对齐的 int (地址 2)：
  访问 [0, 4) 拿到高 2 字节
  访问 [4, 8) 拿到低 2 字节
  拼接 → 2 次访问
```

### 17.2 x86 的非对齐访问

x86 硬件支持非对齐访问，不会崩溃，但：

- 缓存行跨界时明显变慢（一次访问跨两个缓存行）
- AVX/SSE 指令要求对齐，不对齐会崩溃

```c
#include <immintrin.h>

/* AVX 要求 32 字节对齐 */
alignas(32) float data[8];

__m256 v = _mm256_load_ps(data);  /* data 必须 32 字节对齐，否则 segfault */
```

### 17.3 ARM 的非对齐访问

ARMv5 及以前：非对齐访问直接崩溃（SIGBUS）。
ARMv6+：硬件支持非对齐，但慢。
ARMv7+：基本和 x86 一样，硬件处理。

所以跨平台代码要保证对齐，不能依赖 x86 的宽容。

### 17.4 一个性能测试

```c
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define N 10000000

struct aligned {
    int a;
    int b;
    int c;
    int d;
};

struct __attribute__((packed)) unaligned {
    int a;
    char x;  /* 破坏对齐 */
    int b;
    char y;
    int c;
    char z;
    int d;
};

double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void)
{
    struct aligned    *pa = malloc(N * sizeof(*pa));
    struct unaligned  *pu = malloc(N * sizeof(*pu));

    double t1 = now();
    long sum1 = 0;
    for (int i = 0; i < N; i++) {
        sum1 += pa[i].a + pa[i].b + pa[i].c + pa[i].d;
    }
    double t2 = now();

    long sum2 = 0;
    for (int i = 0; i < N; i++) {
        sum2 += pu[i].a + pu[i].b + pu[i].c + pu[i].d;
    }
    double t3 = now();

    printf("对齐:   %.3f s (sum=%ld)\n", t2 - t1, sum1);
    printf("不对齐: %.3f s (sum=%ld)\n", t3 - t2, sum2);

    free(pa); free(pu);
    return 0;
}
```

典型结果（x86-64）：

```
对齐:   0.025 s
不对齐: 0.038 s  (慢 50%)
```

x86 上不对齐慢 50% 左右。ARM 上可能慢几倍甚至崩溃。

### 17.5 缓存行对齐

现代 CPU 缓存行通常 64 字节。如果数据跨缓存行边界，性能下降。

```c
/* 让结构体正好占一个缓存行 */
struct cache_aligned {
    int   data[14];  /* 56 字节 */
    char  pad[8];    /* 凑满 64 字节 */
} __attribute__((aligned(64)));
```

或者用 `alignas(64)` 强制 64 字节对齐。

### 17.6 False Sharing（伪共享）

多线程时，如果两个线程频繁修改的结构体字段在同一个缓存行里，
会触发缓存行频繁同步，性能急剧下降。

```c
struct counters {
    long a;  /* 线程 1 修改 */
    long b;  /* 线程 2 修改 */
};  /* a 和 b 可能在同一缓存行 → false sharing */

struct counters_fixed {
    alignas(64) long a;  /* 独占一个缓存行 */
    alignas(64) long b;  /* 独占一个缓存行 */
};  /* 无 false sharing */
```

这是高性能编程的重要技巧。

---

## 18. 常见面试题精选

### 18.1 题目一：判断字节序

**问**：写一个函数判断当前机器是大端还是小端。

**答**：

```c
int is_little_endian(void) {
    uint32_t x = 1;
    return *(uint8_t *)&x == 1;
}
```

**追问**：能不能不用指针强转？

```c
int is_little_endian_union(void) {
    union { uint32_t i; uint8_t b[4]; } u;
    u.i = 1;
    return u.b[0] == 1;
}
```

### 18.2 题目二：实现 htonl

**问**：手写一个 htonl 函数。

**答**：

```c
uint32_t my_htonl(uint32_t x) {
    if (is_little_endian()) {
        return ((x & 0x000000FF) << 24) |
               ((x & 0x0000FF00) << 8)  |
               ((x & 0x00FF0000) >> 8)  |
               ((x & 0xFF000000) >> 24);
    }
    return x;
}
```

**优化版**（用 GCC 内建函数）：

```c
uint32_t my_htonl(uint32_t x) {
    return is_little_endian() ? __builtin_bswap32(x) : x;
}
```

`__builtin_bswap32` 编译成单条 `bswap` 指令，最快。

### 18.3 题目三：结构体大小

**问**：以下结构体在 64 位系统上 sizeof 是多少？

```c
struct s {
    char  a;
    int   b;
    char  c;
    void *d;
};
```

**答**：

```
偏移 0:    a (1)
偏移 1-3:  填充
偏移 4-7:  b (4)
偏移 8:    c (1)
偏移 9-15: 填充（让 d 对齐到 8）
偏移 16-23: d (8, 指针 8 字节)

sizeof = 24
```

### 18.4 题目四：packed 结构体的访问

**问**：packed 结构体有什么优缺点？

**答**：
- 优点：内存紧凑，适合解析网络包
- 缺点：
  - 访问未对齐字段可能慢（x86 上慢 50%）
  - 某些架构（ARMv5）会崩溃
  - 不能用原子操作（atomic 要求对齐）
  - 不能传给某些系统调用（如 sendmsg 的 iov）

### 18.5 题目五：位域的可移植性

**问**：以下代码在不同编译器上结果一样吗？

```c
struct flags {
    uint8_t a: 4;
    uint8_t b: 4;
};
```

**答**：不一定。位域的分配方向是 implementation-defined：
- GCC 小端：a 在低 4 位
- GCC 大端：a 在高 4 位
- MSVC：a 在低 4 位（不管字节序）

跨编译器/跨平台不要用位域，用位运算。

### 18.6 题目六：为什么网络用大端

**问**：为什么 TCP/IP 协议选大端作为网络字节序？

**答**：
1. 历史原因：1970 年代主流计算机（IBM、Motorola）是大端
2. 大端是人类阅读顺序，调试方便
3. 实际上选哪个都行，关键是统一，选了大端就固定下来
4. Cohen 1980 论文讨论了这个问题，结论是"两种都合理，但必须统一"

### 18.7 题目七：IP 校验和验证

**问**：为什么 IP 校验和只校验头部，不校验数据？

**答**：
1. IP 层只负责把包送到下一跳，数据校验是上层（TCP/UDP）的事
2. 校验整个包开销大（IP 包可达 64KB）
3. 每层只校验自己的头部，职责清晰
4. TCP/UDP 有自己的校验和（含伪首部，覆盖数据）

### 18.8 题目八：union 和 struct 的区别

**问**：union 和 struct 有什么区别？

**答**：
- struct：所有字段各自占用独立内存，总大小 ≈ 各字段大小之和（加填充）
- union：所有字段共享同一块内存，总大小 = 最大字段大小

```c
struct  s { int a; int b; };  /* sizeof = 8, a 和 b 独立 */
union   u { int a; int b; };  /* sizeof = 4, a 和 b 共享内存 */
```

union 常用于：字节序检测、浮点数位操作、节省内存（互斥字段）。

### 18.9 题目九：跨平台传输结构体

'**问**：怎么把一个结构体从 x86 机器发到 ARM 机器？

**答**：不能直接 `send(&struct, sizeof)`，因为：
1. 字节序可能不同（x86 小端，ARM 可能大端）
2. 对齐可能不同（字段间填充不同）
3. 类型大小可能不同（long 在 32/64 位不同）

正确做法：
1. 定义明确的线上格式（如大端、紧凑）
2. 逐字段序列化，每个字段用 htons/htonl
3. 或者用 Protobuf/FlatBuffers 等序列化库

```c
/* 发送 */
uint16_t age_net = htons(p->age);
uint32_t salary_net = htonl(p->salary);
send(fd, &age_net, 2, 0);
send(fd, &salary_net, 4, 0);

/* 接收 */
uint16_t age_net;
uint32_t salary_net;
recv(fd, &age_net, 2, MSG_WAITALL);
recv(fd, &salary_net, 4, MSG_WAITALL);
p->age = ntohs(age_net);
p->salary = ntohl(salary_net);
```

### 18.10 题目十：修改结构体布局

**问**：以下两个结构体哪个更省内存？

```c
struct A { char a; int b; char c; };   /* 12 字节 */
struct B { int b; char a; char c; };   /* 8 字节 */
```

**答**：B 更省。

A 布局：a(1) + 填充(3) + b(4) + c(1) + 填充(1) = 12
B 布局：b(4) + a(1) + c(1) + 填充(2) = 8

**经验**：按大小降序排列字段。

---

## 19. 完整代码示例

### 19.1 字节序检测和演示

```c
/* byte_order_demo.c */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <arpa/inet.h>

/* 打印内存的十六进制 */
static void print_hex(const char *name, const void *p, size_t n)
{
    printf("  %-20s: ", name);
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        printf("%02x ", b[i]);
    }
    printf("\n");
}

/* 检测字节序 */
static const char *byte_order_name(void)
{
    uint32_t i = 1;
    return (*(uint8_t *)&i == 1) ? "小端 (Little-Endian)"
                                  : "大端 (Big-Endian)";
}

/* 演示 htons */
static void demo_htons(void)
{
    uint16_t port = 8080;
    printf("端口值 = %u (0x%04X)\n", port, port);
    print_hex("  主机字节序", &port, 2);

    uint16_t net = htons(port);
    print_hex("  网络字节序", &net, 2);
    printf("  ntohs 转回 = %u\n", ntohs(net));
}

/* 演示 htonl */
static void demo_htonl(void)
{
    uint32_t ip = 0x7F000001;  /* 127.0.0.1 */
    printf("IP = 127.0.0.1 (0x%08X)\n", ip);
    print_hex("  主机字节序", &ip, 4);

    uint32_t net = htonl(ip);
    print_hex("  网络字节序", &net, 4);
    printf("  ntohl 转回 = 0x%08X\n", ntohl(net));
}

/* 演示结构体对齐 */
static void demo_alignment(void)
{
    struct packed_struct {
        uint8_t  a;
        uint32_t b;
    } __attribute__((packed));

    struct unpacked_struct {
        uint8_t  a;
        uint32_t b;
    };

    printf("packed:   %zu\n", sizeof(struct packed_struct));    /* 5 */
    printf("unpacked: %zu\n", sizeof(struct unpacked_struct));  /* 8 */
}

/* 演示位域 */
static void demo_bitfield(void)
{
    struct ip_hdr_byte {
#if __BYTE_ORDER == __LITTLE_ENDIAN
        uint8_t ihl:4;
        uint8_t version:4;
#else
        uint8_t version:4;
        uint8_t ihl:4;
#endif
    } __attribute__((packed));

    struct ip_hdr_byte b;
    b.version = 4;
    b.ihl = 5;

    uint8_t *p = (uint8_t *)&b;
    printf("IP 头第一字节 = 0x%02X (version=4, ihl=5)\n", *p);
    /* 输出 0x45，因为 version=4 在高 4 位，ihl=5 在低 4 位 */
}

int main(void)
{
    printf("==========================================\n");
    printf("  字节序演示\n");
    printf("==========================================\n\n");

    printf("1. 当前主机字节序: %s\n\n", byte_order_name());

    printf("2. 16 位端口转换 (htons):\n");
    demo_htons();
    printf("\n");

    printf("3. 32 位 IP 地址转换 (htonl):\n");
    demo_htonl();
    printf("\n");

    printf("4. 结构体对齐 (packed vs 默认):\n");
    demo_alignment();
    printf("\n");

    printf("5. 位域:\n");
    demo_bitfield();

    return 0;
}
```

### 19.2 校验和计算

```c
/* checksum.c */
#include <stdint.h>
#include <string.h>

/* 16 位反码求和的核心 */
static uint32_t checksum_partial(const uint8_t *data, size_t len, uint32_t init)
{
    uint32_t sum = init;

    while (len > 1) {
        uint16_t word;
        memcpy(&word, data, 2);
        sum += word;
        data += 2;
        len  -= 2;
    }

    if (len == 1) {
        uint16_t word = 0;
        ((uint8_t *)&word)[0] = *data;
        sum += word;
    }

    return sum;
}

/* 计算校验和 */
uint16_t checksum_compute(const uint8_t *data, size_t len)
{
    uint32_t sum = checksum_partial(data, len, 0);

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

/* 验证校验和（结果应该是 0） */
int checksum_verify(const uint8_t *data, size_t len)
{
    return checksum_compute(data, len) == 0;
}
```

### 19.3 完整的 IP 头解析

```c
/* ip_parse.c */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

struct ip_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t  ihl:4;
    uint8_t  version:4;
#else
    uint8_t  version:4;
    uint8_t  ihl:4;
#endif
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed));

void ip_dump(const uint8_t *pkt)
{
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;

    char src_str[INET_ADDRSTRLEN];
    char dst_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip->src_ip, src_str, sizeof(src_str));
    inet_ntop(AF_INET, &ip->dst_ip, dst_str, sizeof(dst_str));

    printf("IP Header:\n");
    printf("  version    = %d\n", ip->version);
    printf("  ihl        = %d (header len = %d bytes)\n", ip->ihl, ip->ihl * 4);
    printf("  tos        = 0x%02X\n", ip->tos);
    printf("  total_len  = %d\n", ntohs(ip->total_len));
    printf("  id         = 0x%04X\n", ntohs(ip->id));

    uint16_t frag = ntohs(ip->frag_off);
    printf("  flags      = DF=%d MF=%d\n", (frag >> 14) & 1, (frag >> 13) & 1);
    printf("  frag_off   = %d\n", frag & 0x1FFF);

    printf("  ttl        = %d\n", ip->ttl);
    printf("  protocol   = %d (%s)\n", ip->protocol,
           ip->protocol == 6 ? "TCP" :
           ip->protocol == 17 ? "UDP" :
           ip->protocol == 1 ? "ICMP" : "other");
    printf("  checksum   = 0x%04X\n", ntohs(ip->checksum));
    printf("  src_ip     = %s\n", src_str);
    printf("  dst_ip     = %s\n", dst_str);
}

int main(void)
{
    /* 一个真实的 IP 包前 20 字节 */
    uint8_t packet[] = {
        0x45, 0x00, 0x00, 0x3c, 0x00, 0x01, 0x40, 0x00,
        0x40, 0x06, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x01,
        0x7f, 0x00, 0x00, 0x01
    };

    ip_dump(packet);
    return 0;
}
```

输出：

```
IP Header:
  version    = 4
  ihl        = 5 (header len = 20 bytes)
  tos        = 0x00
  total_len  = 60
  id         = 0x0001
  flags      = DF=1 MF=0
  frag_off   = 0
  ttl        = 64
  protocol   = 6 (TCP)
  checksum   = 0x0000
  src_ip     = 127.0.0.1
  dst_ip     = 127.0.0.1
```

### 19.4 union 检测字节序

```c
/* union_detect.c */
#include <stdio.h>
#include <stdint.h>

union byte_probe {
    uint32_t i;
    uint8_t  b[4];
};

int main(void)
{
    union byte_probe p;
    p.i = 0x01020304;

    printf("union 字节布局:\n");
    for (int i = 0; i < 4; i++) {
        printf("  b[%d] = 0x%02X\n", i, p.b[i]);
    }

    if (p.b[0] == 0x04) {
        printf("→ 小端 (低位字节在低地址)\n");
    } else if (p.b[0] == 0x01) {
        printf("→ 大端 (高位字节在低地址)\n");
    }

    return 0;
}
```

### 19.5 跨平台字节序转换

```c
/* portable_endian.h */
#ifndef PORTABLE_ENDIAN_H
#define PORTABLE_ENDIAN_H

#include <stdint.h>

/* 平台检测 */
#if defined(__linux__) || defined(__CYGWIN__)
    #include <endian.h>
#elif defined(__APPLE__)
    #include <libkern/OSByteOrder.h>
    #define htobe16(x) OSSwapHostToBigInt16(x)
    #define htobe32(x) OSSwapHostToBigInt32(x)
    #define be16toh(x) OSSwapBigToHostInt16(x)
    #define be32toh(x) OSSwapBigToHostInt32(x)
#elif defined(_WIN32)
    #include <winsock2.h>
    #define htobe16(x) htons(x)
    #define htobe32(x) htonl(x)
    #define be16toh(x) ntohs(x)
    #define be32toh(x) ntohl(x)
#endif

/* 64 位转换（标准库没有） */
static inline uint64_t htobe64(uint64_t x) {
    #if defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN
    return ((uint64_t)htobe32(x & 0xFFFFFFFF) << 32) | htobe32(x >> 32);
    #else
    return x;
    #endif
}

static inline uint64_t be64toh(uint64_t x) {
    return htobe64(x);  /* 转换是对称的 */
}

#endif /* PORTABLE_ENDIAN_H */
```

### 19.6 结构体对齐的完整演示

```c
/* alignment_demo.c */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#define DUMP(s) do { \
    printf("struct %s:\n", #s); \
    printf("  sizeof = %zu\n", sizeof(struct s)); \
    printf("  alignof = %zu\n", _Alignof(struct s)); \
} while (0)

struct natural {
    char  a;
    int   b;
    short c;
    char  d;
};

struct __attribute__((packed)) packed {
    char  a;
    int   b;
    short c;
    char  d;
};

#pragma pack(push, 1)
struct pragma_packed {
    char  a;
    int   b;
    short c;
    char  d;
};
#pragma pack(pop)

struct reordered {
    int   b;
    short c;
    char  a;
    char  d;
};

struct nested {
    char  tag;
    struct natural inner;
    char  pad;
};

int main(void)
{
    printf("=== 结构体对齐演示 ===\n\n");

    DUMP(natural);       /* 12 字节 */
    DUMP(packed);        /* 8 字节 */
    DUMP(pragma_packed); /* 8 字节 */
    DUMP(reordered);     /* 8 字节 */
    DUMP(nested);        /* 16 字节 */

    printf("\n=== 字段偏移 ===\n");
    printf("natural.a = %zu\n", offsetof(struct natural, a));  /* 0 */
    printf("natural.b = %zu\n", offsetof(struct natural, b));  /* 4 */
    printf("natural.c = %zu\n", offsetof(struct natural, c));  /* 8 */
    printf("natural.d = %zu\n", offsetof(struct natural, d));  /* 10 */

    return 0;
}
```

输出：

```
=== 结构体对齐演示 ===

struct natural:
  sizeof = 12
  alignof = 4
struct packed:
  sizeof = 8
  alignof = 1
struct pragma_packed:
  sizeof = 8
  alignof = 1
struct reordered:
  sizeof = 8
  alignof = 4
struct nested:
  sizeof = 16
  alignof = 4

=== 字段偏移 ===
natural.a = 0
1natural.b = 4
natural.c = 8
natural.d = 10
```

---

## 小结

| 知识点 | 要记住的 |
|--------|---------|
| 网络字节序 = 大端 | TCP/IP 规定 |
| x86 是小端 | 你的机器和网络的字节序相反 |
| 发送前 htons/htonl | 主机序 → 网络序 |
| 接收后 ntohs/ntohl | 网络序 → 主机序 |
| inet_addr 已是网络序 | 不要再 htonl |
| 网络包结构体要 packed | 去掉填充，字段不错位 |
| 位域顺序依赖字节序 | 用 `__BYTE_ORDER` 条件编译 |
| 校验和用反码求和 | 字节序无关，验证时结果为 0 |
| union 检测字节序 | 比 `*(uint8_t*)&i` 更安全 |
| 字段按大小降序排列 | 减少填充，节省内存 |
| 跨平台不要用位域 | 用位运算更可移植 |

理解了这些，你就能看懂 `sniff.c` 里 IP/TCP 头部结构体的每一行为什么那么写。
下一篇 `01_raw_sniff.md` 我们开始拆真实的网络包。

---

## 附录 A：常用字节序宏速查

```c
/* 检测字节序 */
uint32_t t = 1;
bool is_le = (*(uint8_t *)&t) == 1;

/* 转换函数 */
htons(x);  /* host → network, 16 bit */
ntohs(x);  /* network → host, 16 bit */
htonl(x);  /* host → network, 32 bit */
ntohl(x);  /* network → host, 32 bit */

/* GCC 内建（不经过 libc，最快） */
__builtin_bswap16(x);
__builtin_bswap32(x);
__builtin_bswap64(x);

/* C++20 标准 */
std::endian::native;       /* 当前字节序 */
std::byteswap(x);          /* 字节翻转 */
```

## 附录 B：常见类型大小和对齐

| 类型 | 32 位系统 | 64 位系统 |
|------|----------|----------|
| char | 1 / align 1 | 1 / align 1 |
| short | 2 / align 2 | 2 / align 2 |
| int | 4 / align 4 | 4 / align 4 |
| long | 4 / align 4 | 8 / align 8 |
| long long | 8 / align 8 | 8 / align 8 |
| float | 4 / align 4 | 4 / align 4 |
| double | 8 / align 8 | 8 / align 8 |
| void* | 4 / align 4 | 8 / align 8 |

注意 `long` 在 32 位和 64 位系统大小不同！
跨平台代码用 `int32_t`/`int64_t` 而不是 `long`。

## 附录 C：进一步阅读

- RFC 791 - Internet Protocol (IP 头格式)
- RFC 793 - Transmission Control Protocol (TCP 头格式)
- RFC 1071 - Computing the Internet Checksum (校验和算法)
- RFC 1700 - Assigned Numbers (网络字节序定义)
- Cohen 1980 - On Holy Wars and a Plea for Peace (字节序之争)
- 《Computer Systems: A Programmer's Perspective》第 2 章（数据表示）
- 《Unix Network Programming》第 3 章（字节序转换）
