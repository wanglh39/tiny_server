# 01 - 阶段 0：raw socket 抓包，手拆 IP/TCP 头部

> 这是教学项目的第一个阶段。我们用 raw socket 收下真实的 TCP 包，
> 自己解析 IP 头和 TCP 头的每一个字段，亲眼看到三次握手、数据传输、四次挥手。
>
> 这一篇会从 IP/TCP 协议的每个字段讲起，画出完整的状态机、时序图，
> 逐行讲解 `sniff.c` 的每个函数，最后给出一系列对照实验。
> 读完之后你不仅能看懂 tcpdump 的输出，还能在白板上画出 TCP 通信的全过程。

## 目录

1. [本阶段目标](#1-本阶段目标)
2. [raw socket 是什么](#2-raw-socket-是什么)
3. [IP 协议详解](#3-ip-协议详解)
4. [TCP 协议详解](#4-tcp-协议详解)
5. [三次握手详解](#5-三次握手详解)
6. [四次挥手详解](#6-四次挥手详解)
7. [TCP 状态机](#7-tcp-状态机)
8. [TCP 选项详解](#8-tcp-选项详解)
9. [代码逐段讲解](#9-代码逐段讲解)
10. [运行方法](#10-运行方法)
11. [对照实验](#11-对照实验)
12. [思考题](#12-思考题)
13. [安全注意事项](#13-安全注意事项)
14. [常见问题](#14-常见问题)

---

## 1. 本阶段目标

做完这个阶段，你应该能回答：

- [ ] 三次握手的三个包，每个包的 SYN/ACK 标志怎么设？seq 和 ack 怎么变化？
- [ ] IP 头的 `ihl` 字段为什么是 5 而不是 20？（提示：单位是 4 字节）
- [ ] TCP 头的 `data_off` 字段同理
- [ ] 为什么 SYN 和 FIN 各占一个序列号？（即使没有数据）
- [ ] `recvfrom` 从 raw socket 读到的数据，第一个字节是什么？（IP 头的 version/ihl）
- [ ] 为什么 raw socket 需要 root 权限？
- [ ] TCP 有哪 11 个状态？TIME_WAIT 为什么存在 2*MSL？
- [ ] MSS、Window Scale、SACK、Timestamp 各有什么作用？
- [ ] IP 分片在什么情况下发生？DF 标志有什么用？

---

## 2. raw socket 是什么

### 2.1 三种 socket 类型的对比

| socket 类型 | 收到的数据 | 能看到什么 | 需要权限 |
|-------------|-----------|-----------|---------|
| `SOCK_STREAM` | 应用层数据 | 只有 read/write 的内容 | 普通 |
| `SOCK_RAW` + `IPPROTO_TCP` | IP头 + TCP头 + 数据 | 完整的 IP 包 | root |
| `SOCK_RAW` + `IPPROTO_RAW` | 可发送任意 IP 包 | 能伪造源 IP 等 | root |
| `AF_PACKET` + `SOCK_RAW` | 以太网帧 | 连链路层都能看 | root |

我们用 `socket(AF_INET, SOCK_RAW, IPPROTO_TCP)`：
- 收到的是 **IP 头 + TCP 头 + 数据**（不含以太网帧头）
- 只收 TCP 包（`IPPROTO_TCP` 过滤）
- 收的是**以本机为目的地的包**（不收本机发出的包）

### 2.2 为什么需要 root？

raw socket 能看到所有经过本机的包的内容，包括别人的密码、会话 token 等。
出于安全考虑，Linux 内核要求 `CAP_NET_RAW` 权限（root 或 sudo）才能创建。

### 2.3 raw socket vs tcpdump

`tcpdump` 底层也用 raw socket（确切说是 `AF_PACKET`），但它帮你做了：
- 解析以太网帧
- 过滤规则（BPF）
- 时间戳
- 格式化输出

我们自己写，是为了**理解 tcpdump 输出的每一行是什么意思**。

### 2.4 创建 raw socket 的代码

```c
int raw_fd = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
if (raw_fd < 0) {
    err_sys("创建 raw socket 失败（需要 sudo 权限）");
}
```

- `AF_INET`：IPv4
- `SOCK_RAW`：原始套接字
- `IPPROTO_TCP`：只收 TCP 包（`<netinet/in.h>` 里定义为 6）

### 2.5 raw socket 能做什么不能做什么

**能做的**：
- 收下到达本机 IP 层的包（含 IP 头）
- 用 `IPPROTO_TCP`/`IPPROTO_UDP` 等过滤上层协议
- 发送自构造的 IP 包（`IPPROTO_RAW`，可伪造源 IP）

**不能做的**：
- 收本机发出的包（要 `AF_PACKET`）
- 收以太网帧（要 `AF_PACKET`）
- 修改流经的包（要 `iptables`/`nftables` 的 NFQUEUE）
- 收其他主机单播给别人的包（要混杂模式 + `AF_PACKET`）

### 2.6 raw socket 的内核路径

```
网卡收到包
    ↓
链路层（以太网驱动）
    ↓
IP 层（ip_rcv）
    ↓
├── 本机目的地？→ ip_local_deliver
│       ↓
│   TCP/UDP 上交
│       ↓
│   同时复制一份给 raw socket（IPPROTO_TCP 匹配）
│       ↓
│   recvfrom 返回 IP 头 + TCP 头 + 数据
│
└── 转发？→ ip_forward（不交给 raw socket）
```

所以 raw socket 看到的是**已经过 IP 层处理、即将上交 TCP 栈**的包。
这时 IP 头完整，TCP 头还没被 TCP 栈处理。

---

## 3. IP 协议详解

### 3.1 IP 头部格式（RFC 791）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Version|  IHL  |Type of Service|          Total Length         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Identification        |Flags|      Fragment Offset    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Time to Live |    Protocol   |         Header Checksum       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Source Address                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Destination Address                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

IP 头**固定部分**20 字节，后面可能有可选项（Options）。

### 3.2 逐字段解释

| 字段 | 大小 | 含义 | 我们怎么用 |
|------|------|------|-----------|
| Version | 4 bit | IP 版本，IPv4=4 | 检查是否为 4 |
| IHL | 4 bit | IP Header Length，**单位 4 字节** | `ip_hdr_len = ihl * 4` |
| TOS | 1 字节 | Type of Service，QoS 标记 | 通常为 0 |
| Total Length | 2 字节 | 整个 IP 包长度（头+数据） | `ntohs(ip->total_len)` |
| Identification | 2 字节 | 包标识，用于分片重组 | 分片相关 |
| Flags | 3 bit | 分片标志（Reserved/DF/MF） | 分片相关 |
| Fragment Offset | 13 bit | 分片偏移，单位 8 字节 | 分片相关 |
| TTL | 1 字节 | 最多经过的路由器数，每跳减 1 | 到 0 则丢弃 |
| Protocol | 1 字节 | 上层协议：6=TCP 17=UDP | 我们只看 6 |
| Header Checksum | 2 字节 | IP 头校验和 | 不验证（教学简化） |
| Source Address | 4 字节 | 源 IP | `inet_ntop` 打印 |
| Destination Address | 4 字节 | 目的 IP | `inet_ntop` 打印 |

### 3.3 Version 字段

IPv4 填 4，IPv6 填 6。我们只处理 IPv4，所以检查：

```c
if (ip->version != 4) {
    /* 跳过 IPv6 包 */
    return;
}
```

IPv6 的头格式完全不同（40 字节固定头，无 IHL 字段），
所以解析 IPv6 要用另一套结构体。

### 3.4 IHL 字段的陷阱

IHL = IP Header Length，但**单位是 4 字节**，不是字节！

```
IHL = 5 → IP 头长度 = 5 * 4 = 20 字节（最常见，无 Options）
IHL = 6 → IP 头长度 = 6 * 4 = 24 字节（有 4 字节 Options）
IHL = 15 → IP 头长度 = 15 * 4 = 60 字节（最大，40 字节 Options）
```

代码里：

```c
int ip_hdr_len = ip->ihl * 4;  // 一定要乘 4！
```

**为什么用 4 字节作单位？** 因为 IP 头长度总是 4 的倍数（Options 用 padding 凑齐）。
用 4 字节单位只要 4 bit 就能表示 0-60 字节，节省头部空间。

### 3.5 TOS / DSCP 字段

Type of Service，1 字节，用于 QoS（服务质量）：

```
 0   1   2   3   4   5   6   7
+---+---+---+---+---+---+---+---+
|  DSCP (6 bit) | ECN (2 bit) |
+---+---+---+---+---+---+---+---+
```

- DSCP（Differentiated Services Code Point）：6 bit，区分优先级
  - 0：默认（Best Effort）
  - 46：EF（Expedited Forwarding，最高优先级，VoIP）
  - 8：CS1（Low Priority）
- ECN（Explicit Congestion Notification）：2 bit，显式拥塞通知

大多数普通流量 TOS=0。路由器可以按 DSCP 值给不同优先级。

### 3.6 Total Length 字段

整个 IP 包的长度（头 + 数据），2 字节，最大 65535 字节。

```c
int total_len = ntohs(ip->total_len);
int data_len = total_len - ip_hdr_len;  /* IP 数据长度 */
```

注意以太网帧最小 64 字节，但 IP 包可以更小（以太网帧会 padding）。
所以 Total Length 才是真实长度，不能从以太网帧长度推算。

### 3.7 Identification 字段

2 字节，包标识。每个 IP 包递增（不是必须，但常见）。
用于**分片重组**：同一原始包的分片有相同的 Identification。

```c
printf("id = 0x%04X\n", ntohs(ip->id));
```

### 3.8 Flags 和 Fragment Offset

这两个字段合在一起 2 字节（16 bit）：

```
 0   1   2   3   4   5   6   7   8   9   10  ...  15
+---+---+---+---+---+---+---+---+---+---+---+---+---+
| R | DF| MF|    Fragment Offset (13 bit)          |
+---+---+---+---+---+---+---+---+---+---+---+---+---+
```

- R（Reserved）：保留位，必须为 0
- DF（Don't Fragment）：1 = 不允许分片
  - 路径 MTU 发现用这个标志：发 DF=1 的包，如果路由器要分片就返回 ICMP 错误
- MF（More Fragments）：1 = 后面还有分片，0 = 最后一个分片
- Fragment Offset：本分片在原包中的偏移，**单位 8 字节**

```c
uint16_t frag = ntohs(ip->frag_off);
int df = (frag >> 14) & 1;
int mf = (frag >> 13) & 1;
int offset = frag & 0x1FFF;
```

### 3.9 IP 分片详解

当 IP 包大于路径 MTU（通常 1500 字节）时，路由器会分片：

```
原始包 4000 字节（IP 头 20 + 数据 3980）
    ↓ MTU = 1500
分片 1: offset=0,    len=1480, MF=1  (IP 头 20 + 数据 1480)
分片 2: offset=1480, len=1480, MF=1  (IP 头 20 + 数据 1480)
分片 3: offset=2960, len=1020, MF=0  (IP 头 20 + 数据 1020)
```

注意 offset 单位是 8 字节，所以 1480/8 = 185。
所有分片有相同的 Identification，目的端按 offset 重组。

**DF=1 时不允许分片**：如果路由器要分片但 DF=1，丢弃包并返回 ICMP "Fragmentation Needed"。
路径 MTU 发现就靠这个：发 DF=1 的包，收到 ICMP 就减小包大小。

### 3.10 TTL 字段

Time to Live，1 字节。每经过一个路由器减 1，到 0 时丢弃并返回 ICMP "Time Exceeded"。

- 默认值通常是 64（Linux）或 128（Windows）或 255（某些网络设备）
- `traceroute` 利用 TTL：发 TTL=1 的包，第一个路由器返回 ICMP；发 TTL=2，第二个返回；...

```c
printf("ttl = %d\n", ip->ttl);
/* ttl=64 大概是 Linux，128 是 Windows，255 是某些路由器 */
```

### 3.11 Protocol 字段

1 字节，指示上层协议：

| 值 | 协议 | 说明 |
|----|------|------|
| 1 | ICMP | ping、错误报告 |
| 2 | IGMP | 组播管理 |
| 6 | **TCP** | 我们关心的 |
| 17 | UDP | 无连接 |
| 41 | IPv6 | IPv6 隧道 |
| 47 | GRE | VPN 隧道 |
| 50 | ESP | IPSec |
| 89 | OSPF | 路由协议 |
| 132 | SCTP | 流控制传输协议 |

我们用 `if (ip->protocol != 6) return;` 过滤掉非 TCP 包。

### 3.12 Header Checksum 字段

2 字节，IP 头的 16 位反码求和校验和（见 `00_byte_order.md` 第 12 节）。
只校验 IP 头，不校验数据。教学项目里我们不验证。

### 3.13 Source / Destination Address

各 4 字节，源 IP 和目的 IP。**网络字节序**存储。

```c
char src_str[INET_ADDRSTRLEN];
char dst_str[INET_ADDRSTRLEN];
inet_ntop(AF_INET, &ip->src_ip, src_str, sizeof(src_str));
inet_ntop(AF_INET, &ip->dst_ip, dst_str, sizeof(dst_str));
printf("%s → %s\n", src_str, dst_str);
```

`inet_ntop` 期望参数是网络字节序，所以不要 `ntohl`。

### 3.14 IP Options（可选项）

IHL > 5 时有 Options，最长 40 字节（IHL=15）。
常见 Options：
- Record Route：记录经过的路由器
- Timestamp：记录经过时间
- Loose/Strict Source Route：指定路径

现代网络几乎不用 IP Options，因为：
- 路由器处理慢（不是快速路径）
- 安全风险（泄露路径信息）
- 很多路由器直接丢弃带 Options 的包

### 3.15 IP 头结构体定义

```c
struct ip_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t  ihl:4;        /* 低 4 位 */
    uint8_t  version:4;    /* 高 4 位 */
#else
    uint8_t  version:4;
    uint8_t  ihl:4;
#endif
    uint8_t  tos;
    uint16_t total_len;    /* 网络字节序，读取时 ntohs */
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;     /* 单字节，不用转字节序 */
    uint16_t checksum;
    uint32_t src_ip;       /* 网络字节序，inet_ntop 直接用 */
    uint32_t dst_ip;
} __attribute__((packed));
```

注意：
1. `version` 和 `ihl` 的顺序依赖字节序（见 `00_byte_order.md` 第 6 节）
2. `packed` 确保没有填充
3. 多字节字段（`total_len` 等）存的是**网络字节序**，读取时要 `ntohs`

---

## 4. TCP 协议详解

### 4.1 TCP 头部格式（RFC 793）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Acknowledgment Number                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Data |           |U|A|P|R|S|F|                               |
| Offset| Reserved  |R|C|S|S|Y|I|            Window             |
|       |           |G|K|H|T|N|N|                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Checksum            |         Urgent Pointer        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

TCP 头**固定部分**也是 20 字节，后面可能有 Options（如 MSS、Timestamp、SACK）。

### 4.2 逐字段解释

| 字段 | 大小 | 含义 | 关键理解 |
|------|------|------|---------|
| Source Port | 2 字节 | 源端口 | 客户端通常是临时端口（ephemeral） |
| Dest Port | 2 字节 | 目的端口 | 服务器监听端口（如 8080） |
| Sequence Number | 4 字节 | 序列号 | **TCP 的核心**，见 4.3 |
| Acknowledgment Number | 4 字节 | 确认号 | 期望收到的下一个字节编号 |
| Data Offset | 4 bit | TCP 头长度，**单位 4 字节** | `tcp_hdr_len = data_off * 4` |
| Reserved | 6 bit | 保留（部分用于 NS/CWR/ECE） | 通常为 0 |
| URG | 1 bit | 紧急指针有效 | 很少用 |
| ACK | 1 bit | 确认号有效 | 握手第三包开始都为 1 |
| PSH | 1 bit | 推送，立即交给应用 | 通常和 ACK 一起 |
| RST | 1 bit | 重置连接 | 异常关闭 |
| SYN | 1 bit | 同步序列号 | 握手前两包为 1 |
| FIN | 1 bit | 发送方结束 | 挥手用 |
| Window | 2 字节 | 接收窗口大小 | **流量控制**，见 4.4 |
| Checksum | 2 字节 | 校验和（含伪首部） | 不验证（教学简化） |
| Urgent Pointer | 2 字节 | 紧急数据偏移 | URG=1 时有效 |

### 4.3 序列号（Sequence Number）—— TCP 的灵魂

TCP 是**可靠的字节流**。序列号给每个字节编号，让接收方知道：
- 这个字节在流的哪个位置
- 哪些字节已经收到（去重）
- 哪些字节缺失（触发重传）

**关键规则**：

1. SYN 包的 seq 是随机初始值（ISN，Initial Sequence Number）
   - 为什么随机？防止前一个连接的包被误收，也防止序列号预测攻击
2. SYN 和 FIN 各**消耗一个序列号**（即使没有数据）
   - 所以握手后，seq = ISN + 1
3. 数据包的 seq 是该包第一个字节的编号
4. ACK 包的 ack = 期望收到的下一个字节编号 = 已收到的最大连续字节 + 1

**为什么 SYN/FIN 要消耗序列号？**

因为 ACK 机制要求"每个发送的段都要被确认"。如果 FIN 不消耗序列号，
ACK 丢失时发送方无法判断"对端确认了我的 FIN"还是"对端确认了我 FIN 前的数据"。
消耗一个序列号后，FIN 的 ACK = FIN_seq + 1，明确表示"FIN 已收到"。

### 4.4 窗口（Window）—— 流量控制

Window 告诉对端："我的接收缓冲区还能收多少字节"。
对端发送的数据量不能超过这个窗口。

- 窗口 = 0：别发了，我缓冲区满了
- 窗口变大：我处理了一些数据，可以继续收

这就是 TCP 的**流量控制**（flow control），和**拥塞控制**不同：
- 流量控制：保护接收方（端到端）
- 拥塞控制：保护网络（全局）

### 4.5 拥塞控制简介

TCP 拥塞控制有四个阶段：

```
1. 慢启动（Slow Start）
   拥塞窗口 cwnd 从 1 开始，每收到一个 ACK 翻倍
   指数增长：1 → 2 → 4 → 8 → 16 → ...

2. 拥塞避免（Congestion Avoidance）
   到达 ssthresh（慢启动阈值）后，每 RTT 加 1
   线性增长：16 → 17 → 18 → ...

3. 拥塞发生（Congestion Detected）
   超时：ssthresh = cwnd/2, cwnd = 1，回到慢启动
   3 次重复 ACK：ssthresh = cwnd/2, cwnd = ssthresh，快速恢复

4. 快速恢复（Fast Recovery）
   拥塞窗口线性增长，直到收到新 ACK
```

实际发送窗口 = min(对端通告的 Window, 拥塞窗口 cwnd)。

### 4.6 TCP 标志位详解

| 标志 | 含义 | 何时出现 |
|------|------|---------|
| SYN | 同步序列号 | 握手前两包 |
| ACK | 确认号有效 | 握手第三包起几乎所有包 |
| FIN | 发送方结束 | 挥手时 |
| RST | 重置连接 | 异常关闭、拒绝连接、端口未监听 |
| PSH | 立即推送 | 有数据要立即交给应用 |
| URG | 紧急指针有效 | 极少用（telnet 的 Ctrl-C） |

**RST 出现的场景**：
1. 连接到一个没有监听的端口 → 服务器回 RST
2. 进程崩溃，内核发 RST 清理连接
3. 防火墙拒绝连接 → 发 RST
4. 接收到不属于任何连接的包 → 发 RST

### 4.7 TCP 头结构体定义

```c
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack_seq;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t res1:4;
    uint16_t data_off:4;
    uint16_t fin:1;
    uint16_t syn:1;
    uint16_t rst:1;
    uint16_t psh:1;
    uint16_t ack:1;
    uint16_t urg:1;
    uint16_t res2:2;
#else
    uint16_t data_off:4;
    uint16_t res1:4;
    uint16_t res2:2;
    uint16_t urg:1;
    uint16_t ack:1;
    uint16_t psh:1;
    uint16_t rst:1;
    uint16_t syn:1;
    uint16_t fin:1;
#endif
    uint16_t window;
    uint16_t checksum;
    uint16_t urg_ptr;
} __attribute__((packed));
```

位域的顺序在小端机器上是从低位到高位排列的，
所以 `fin` 在最前（最低位），对应 TCP 头里 flags 字节的最低位。

### 4.8 Data Offset 字段

和 IP 的 IHL 类似，TCP 头长度单位是 4 字节：

```
data_off = 5 → TCP 头 = 20 字节（无 Options）
data_off = 8 → TCP 头 = 32 字节（12 字节 Options，常见）
data_off = 15 → TCP 头 = 60 字节（最大，40 字节 Options）
```

```c
int tcp_hdr_len = tcp->data_off * 4;
int data_len = ip_data_len - tcp_hdr_len;  /* TCP 载荷长度 */
```

---

## 5. 三次握手详解

### 5.1 为什么是三次，不是两次？

**两次握手的问题**：

假设只有两次：
1. 客户端 → 服务器：SYN, seq=x
2. 服务器 → 客户端：SYN+ACK, seq=y, ack=x+1

现在服务器认为连接建立了。但如果包 1 是**延迟的旧包**（网络中滞留的旧 SYN），
服务器会错误地建立一个连接，浪费资源。

第三次握手（客户端确认）能防止这种情况：
客户端收到包 2 后，如果发现自己的 ISN 不是 x，就不会发 ACK，服务器收不到 ACK 就会超时重置。

### 5.2 三个包的详细分析

假设客户端 ISN = 1000，服务器 ISN = 2000。

#### 包 1：客户端 → 服务器（SYN）

```
[SYN]  client:54321 → server:8080  seq=1000 ack=0 win=64240 data=0
```

- `SYN=1, ACK=0`：请求建立连接，ack 字段无意义（填 0）
- `seq=1000`：客户端的 ISN
- `ack=0`：还没有要确认的东西
- `data=0`：SYN 不携带数据，但消耗 1 个序列号

#### 包 2：服务器 → 客户端（SYN+ACK）

```
[SYN,ACK]  server:8080 → client:54321  seq=2000 ack=1001 win=64240 data=0
```

- `SYN=1, ACK=1`：同意连接，同时同步自己的序列号
- `seq=2000`：服务器的 ISN
- `ack=1001`：确认收到你的 1000，期望下一个是 1001（ISN + 1）
- `data=0`：SYN 消耗 1 个序列号

#### 包 3：客户端 → 服务器（ACK）

```
[ACK]  client:54321 → server:8080  seq=1001 ack=2001 win=64240 data=0
```

- `SYN=0, ACK=1`：确认收到服务器的 SYN
- `seq=1001`：上一个 seq(1000) + SYN消耗的1 = 1001
- `ack=2001`：确认收到你的 2000，期望下一个是 2001
- `data=0`：纯 ACK 不消耗序列号

### 5.3 握手的时序图

```
    客户端                                            服务器
      |                                                |
      |  [SYN] seq=1000, ack=0                         |
      |----------------------------------------------->|  CLOSED → SYN_RCVD
      |                                                |
      |  [SYN,ACK] seq=2000, ack=1001                 |
      |<-----------------------------------------------|  SYN_RCVD
      |                                                |
      |  [ACK] seq=1001, ack=2001                     |
      |----------------------------------------------->|  SYN_RCVD → ESTABLISHED
      |                                                |
   ESTABLISHED                                     ESTABLISHED
      |                                                |
      |  [PSH,ACK] seq=1001, ack=2001, data=78        |
      |----------------------------------------------->|
      |                                                |
      |  [ACK] seq=2001, ack=1079                     |
      |<-----------------------------------------------|
      |                                                |
```

### 5.4 seq/ack 计算规则总结

| 包 | seq 变化 | ack 变化 |
|----|---------|---------|
| SYN | ISN（随机） | 0（无意义） |
| SYN+ACK | 服务器 ISN | 客户端 ISN + 1 |
| ACK | 客户端 ISN + 1 | 服务器 ISN + 1 |
| 数据（n 字节） | 上个 seq + 数据长度 | 不变（没新数据要确认） |
| 纯 ACK | 不变（ACK 不消耗 seq） | 上个对端 seq + 数据长度 |

**口诀**：
- 我发的 seq = 我上次 seq + 我上次发的数据量（SYN/FIN 算 1 字节）
- 我发的 ack = 对端上次 seq + 对端上次发的数据量（SYN/FIN 算 1 字节）

### 5.5 握手后的状态

握手完成后：
- 客户端：`ESTABLISHED`，seq 从 1001 开始
- 服务器：`ESTABLISHED`，seq 从 2001 开始

之后的数据包：
```
[PSH,ACK]  client:54321 → server:8080  seq=1001 ack=2001 win=64240 data=78
[ACK]      server:8080 → client:54321  seq=2001 ack=1079 win=64240 data=0
```

- 客户端发 78 字节数据：seq=1001, data=78
- 服务器确认：ack=1001+78=1079

### 5.6 握手可携带数据（TCP Fast Open）

RFC 7413 的 TCP Fast Open（TFO）允许 SYN 包携带数据：
- 第一次握手：客户端发 SYN + 数据 + Cookie 请求
- 服务器验证 Cookie，直接处理数据并响应
- 省去一个 RTT

但 TFO 部署率低，普通 TCP 握手不携带数据。

### 5.7 用我们的程序观察

运行 `sudo raw_sniff 8080`，另开终端 `curl localhost:8080`，你会看到：

```
[SYN]        127.0.0.1:54321 → 127.0.0.1:8080  seq=123456789 ack=0 win=64240 data=0
[SYN,ACK]    127.0.0.1:8080  → 127.0.0.1:54321 seq=987654321 ack=123456790 win=64240 data=0
[ACK]        127.0.0.1:54321 → 127.0.0.1:8080  seq=123456790 ack=987654322 win=64240 data=0
[PSH,ACK]    127.0.0.1:54321 → 127.0.0.1:8080  seq=123456790 ack=987654322 win=64240 data=78
[ACK]        127.0.0.1:8080  → 127.0.0.1:54321 seq=987654322 ack=123456868 win=64240 data=0
...
```

**重点观察**：
1. 包1 的 ack=0（SYN 不确认任何东西）
2. 包2 的 ack = 包1 的 seq + 1
3. 包3 的 ack = 包2 的 seq + 1
4. 数据包的 ack 不变（没有新数据要确认），seq 增加 data 大小

---

## 6. 四次挥手详解

### 6.1 为什么挥手是四次，握手是三次？

握手时，SYN+ACK 可以合并成一个包（服务器同时发起和确认）。

挥手时，FIN 表示"我没有数据要发了"，但**可能还要接收数据**。
所以两个方向的关闭是独立的：

1. 客户端发 FIN：我不发了
2. 服务器回 ACK：知道了
3. 服务器发 FIN：我也不发了（可能中间还有数据要发）
4. 客户端回 ACK：知道了

如果服务器收到 FIN 时已经没有数据要发，步骤 2 和 3 可以合并（变成三次挥手）。

### 6.2 四个包的详细分析

```
[FIN,ACK]  client:54321 → server:8080  seq=1079 ack=2100 win=64240 data=0
[ACK]      server:8080 → client:54321  seq=2100 ack=1080 win=64240 data=0
[FIN,ACK]  server:8080 → client:54321  seq=2100 ack=1080 win=64240 data=0
[ACK]      client:54321 → server:8080  seq=1080 ack=2101 win=64240 data=0
```

- 包1：客户端 FIN，seq=1079，消耗 1 个序列号
- 包2：服务器 ACK，ack=1080（1079+1）
- 包3：服务器 FIN，seq=2100
- 包4：客户端 ACK，ack=2101（2100+1）

### 6.3 挥手的时序图

```
    客户端                                            服务器
      |                                                |
   ESTABLISHED                                     ESTABLISHED
      |                                                |
      |  [FIN,ACK] seq=1079, ack=2100                 |
      |----------------------------------------------->|  ESTABLISHED → CLOSE_WAIT
      |                                                |
      |  [ACK] seq=2100, ack=1080                     |
      |<-----------------------------------------------|  CLOSE_WAIT
      |                                                |
   FIN_WAIT_2                                      CLOSE_WAIT
      |                                                |
      |  （服务器可能还有数据要发）                    |
      |  [PSH,ACK] seq=2100, ack=1080, data=...       |
      |<-----------------------------------------------|
      |  [ACK] seq=1080, ack=...                      |
      |----------------------------------------------->|
      |                                                |
      |  [FIN,ACK] seq=2100, ack=1080                 |
      |<-----------------------------------------------|  CLOSE_WAIT → LAST_ACK
      |                                                |
      |  [ACK] seq=1080, ack=2101                     |
      |----------------------------------------------->|  LAST_ACK → CLOSED
      |                                                |
   TIME_WAIT                                        CLOSED
      |                                                |
      |  等待 2*MSL                                    |
      |                                                |
   CLOSED
```

### 6.4 半关闭状态

TCP 允许"半关闭"（half-close）：一个方向关闭了，另一个方向还能传数据。

```
客户端发 FIN → 客户端→服务器方向关闭
但服务器→客户端方向还能传数据
服务器发完数据后发 FIN → 完全关闭
```

应用场景：HTTP/1.0 的客户端发完请求就 close write，
服务器还能返回响应，然后才关闭。

### 6.5 TIME_WAIT 状态

**主动关闭的一方**（这里是客户端）发完最后一个 ACK 后进入 `TIME_WAIT`，
持续 **2*MSL**（通常 60 秒）。

为什么？

1. **确保最后一个 ACK 能到达对端**
   如果 ACK 丢失，对端会重发 FIN，本端还能重发 ACK。
   如果本端已经 CLOSED，对端重发的 FIN 就得不到响应，永远卡在 LAST_ACK。

2. **让旧连接的包在网络中消亡**
   2*MSL 时间内，旧连接的延迟包都会被丢弃，防止被新连接误收。
   MSL（Maximum Segment Lifetime）是包在网络中最长存活时间，通常 30 秒。

**TIME_WAIT 的问题**：
- 主动关闭方在 TIME_WAIT 期间，端口被占用
- 服务器重启时会报 `Address already in use`
- 解决：设置 `SO_REUSEADDR`

**TIME_WAIT 的优化**：
- `tcp_tw_reuse=1`：允许复用 TIME_WAIT 的端口（客户端）
- `tcp_tw_recycle=1`：快速回收 TIME_WAIT（已废弃，有副作用）
- 减少主动关闭：让客户端主动关闭（服务器被动关闭不进 TIME_WAIT）

### 6.6 RST 关闭（异常关闭）

除了四次挥手，TCP 还可以用 RST 一次性关闭：

```
[RST,ACK]  client → server  seq=... ack=...
```

RST 关闭的特点：
- 不需要四次挥手，一个包搞定
- 不进入 TIME_WAIT
- 不保证数据都送达（可能丢数据）
- 接收方会收到 "Connection reset by peer" 错误

**何时发 RST**：
- 进程被 kill -9（来不及挥手）
- close 时接收缓冲区还有数据（设置 SO_LINGER 为 0）
- 连接到未监听的端口
- 防火墙拒绝

---

## 7. TCP 状态机

### 7.1 完整的 11 个状态

| 状态 | 说明 | 谁会进入 |
|------|------|---------|
| CLOSED | 初始状态，没有连接 | 双方 |
| LISTEN | 服务器等待连接 | 服务器 |
| SYN_SENT | 客户端发了 SYN，等 SYN+ACK | 客户端 |
| SYN_RCVD | 服务器收到 SYN，发了 SYN+ACK，等 ACK | 服务器 |
| ESTABLISHED | 连接建立，可以传数据 | 双方 |
| FIN_WAIT_1 | 主动关闭方发了 FIN，等 ACK | 主动关闭方 |
| FIN_WAIT_2 | 主动关闭方收到 ACK，等对端 FIN | 主动关闭方 |
| CLOSE_WAIT | 被动关闭方收到 FIN，回了 ACK，等自己 close | 被动关闭方 |
| LAST_ACK | 被动关闭方发了 FIN，等最后一个 ACK | 被动关闭方 |
| TIME_WAIT | 主动关闭方发了最后 ACK，等 2*MSL | 主动关闭方 |
| CLOSING | 双方同时发 FIN（少见） | 双方 |

### 7.2 完整状态转换图

```
                              +---------+
                  rcv SYN     |  LISTEN |
                  snd SYN,ACK +---------+
                     |                |
                     |                |
                    del              rcv SYN
                    |                snd SYN,ACK
                    v                |
                              +---------+
                              | SYN_RCVD|
                              +---------+
                     |                |
                 rcv ACK of SYN       |
                 snd nothing          |
                     |                |
                     v                |
                              +---------+
                              |ESTABLISHED|
                              +---------+
                     |                |
              close  |                |  rcv FIN
              snd FIN|                |  snd ACK
                     v                v
                              +---------+        +---------+
                              | FIN_WAIT1|      | CLOSE_WAIT|
                              +---------+        +---------+
                     |                |              |
              rcv FIN|                |rcv ACK of FIN|  close
              snd ACK|                |              |  snd FIN
                     v                v              v
                              +---------+        +---------+
                              |FIN_WAIT2 |      | LAST_ACK|
                              +---------+        +---------+
                     |                |              |
                 rcv FIN|                |rcv ACK of FIN|
                 snd ACK|                |              |
                     v                v              v
                              +---------+        +---------+
                              |TIME_WAIT |      |  CLOSED  |
                              +---------+        +---------+
                     |                |
              2*MSL timeout           |
                     v                v
                              +---------+
                              |  CLOSED |
                              +---------+
```

### 7.3 客户端的状态转换

```
CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT → CLOSED
   connect     握手完成        close          rcv ACK       rcv FIN      2*MSL
```

### 7.4 服务器的状态转换

```
CLOSED → LISTEN → SYN_RCVD → ESTABLISHED → CLOSE_WAIT → LAST_ACK → CLOSED
  listen   rcv SYN   握手完成     rcv FIN       close      rcv ACK
```

### 7.5 用 ss 命令观察状态

```bash
# 查看所有 TCP 连接的状态
ss -tan

# 输出示例
State   Recv-Q  Send-Q  Local Address:Port  Peer Address:Port
LISTEN  0       128     0.0.0.0:8080        0.0.0.0:*
ESTAB   0       0       127.0.0.1:8080     127.0.0.1:54321
TIME-WAIT 0     0       127.0.0.1:54321    127.0.0.1:8080
```

### 7.6 用 netstat 观察状态

```bash
netstat -tan

# 输出示例
Proto Recv-Q Send-Q Local Address     Foreign Address   State
tcp   0      0      0.0.0.0:8080      0.0.0.0:*         LISTEN
tcp   0      0      127.0.0.1:8080    127.0.0.1:54321   ESTABLISHED
tcp   0      0      127.0.0.1:54321   127.0.0.1:8080    TIME_WAIT
```

### 7.7 教学要点

- `LISTEN`：服务器等待连接
- `SYN_RCVD`：收到 SYN，发了 SYN+ACK，等 ACK（很短暂，难观察到）
- `ESTABLISHED`：连接建立
- `CLOSE_WAIT`：收到 FIN，还没 close（如果程序不 close，会一直停留在这里）
- `TIME_WAIT`：主动 close 后，等 2*MSL

### 7.8 CLOSE_WAIT 堆积问题

如果服务器收到客户端 FIN 后**不调用 close**，连接会一直停在 CLOSE_WAIT。
这是常见的 fd 泄漏原因：

```c
/* ❌ 忘了 close */
while (read(conn_fd, buf, sizeof(buf)) > 0) {
    /* 处理数据 */
}
/* read 返回 0 表示对端关闭，但本端没 close → CLOSE_WAIT 堆积 */
```

```c
/* ✅ read 返回 0 后要 close */
while (read(conn_fd, buf, sizeof(buf)) > 0) {
    /* 处理数据 */
}
close(conn_fd);  /* 必须 close */
```

用 `ss -tan | grep CLOSE-WAIT | wc -l` 检查 CLOSE_WAIT 数量。

---

## 8. TCP 选项详解

### 8.1 TCP Options 的位置

紧跟在 TCP 头固定 20 字节之后，最长 40 字节（data_off=15）。
每个 Option 格式：`[Kind][Length][Data...]`

### 8.2 MSS（Maximum Segment Size）

- Kind = 2，Length = 4
- 告诉对端"我能接收的 TCP 段最大数据量"
- 通常 = MTU - IP 头 - TCP 头 = 1500 - 20 - 20 = 1460
- 只在 SYN 包里出现

```
SYN 包里的 MSS 选项：
02 04 05 B4    (Kind=2, Len=4, MSS=0x05B4=1460)
```

### 8.3 Window Scale（窗口缩放）

- Kind = 3，Length = 3
- Window 字段只有 16 bit，最大 65535 字节（64KB）
- 现代网络带宽远超 64KB/RTT，需要更大的窗口
- Window Scale 让实际窗口 = Window 字段 << shift_count

```
SYN 包里的 Window Scale 选项：
03 03 07    (Kind=3, Len=3, shift=7)

实际窗口 = Window 字段 * 2^7 = Window * 128
最大 = 65535 * 128 = 8MB
```

只在 SYN 包里协商 shift，之后的数据包用协商好的 shift。

### 8.4 SACK（Selective Acknowledgment）

- Kind = 5，Length = 可变
- 标准 ACK 只能确认"连续收到的最大字节"
- 如果中间丢了一些，重传会多传已收到的
- SACK 允许"我收到了这些不连续的块"

```
SACK 选项示例：
05 0A 00 01 00 05 00 0A 00 10
Kind=5, Len=10
块1: [0x00010005, 0x000A0010)  (收到 65541-65552)
```

SACK 让发送方只重传缺失的块，提高性能。

### 8.5 Timestamp

- Kind = 8，Length = 10
- 两个 4 字节时间戳：发送方的时间戳 + 接收方上次的时间戳（echo）
- 用于：
  1. 更精确的 RTT 估算
  2. PAWS（Protect Against Wrapped Sequence numbers）防止旧包

```
Timestamp 选项：
08 0A 00 00 12 34 00 00 56 78
Kind=8, Len=10
TSval=0x00001234, TSecr=0x00005678
```

### 8.6 常见 Options 组合

一个 SYN 包通常有：

```
MSS(4) + Window Scale(3) + SACK Permitted(2) + Timestamp(10) + NOP(1) + NOP(1) = 21 字节
```

加上 TCP 头固定 20 字节，data_off = (20 + 21 + 3 padding) / 4 = 11（向上对齐到 4 字节）。

### 8.7 NOP 选项

- Kind = 1，Length = 1
- 用于对齐：把下一个选项对齐到 4 字节边界
- 也用于填充

### 8.8 SACK Permitted

- Kind = 4，Length = 2
- SYN 包里表示"我支持 SACK"
- 实际 SACK 数据在数据包里

---

## 9. 代码逐段讲解

### 9.1 头文件包含

```c
#include <netinet/ip.h>     // 系统的 IP 头结构体（我们没用，自己定义了）
#include <netinet/tcp.h>    // 系统的 TCP 头结构体（同上）
#include <arpa/inet.h>      // inet_ntop：IP 地址转字符串
#include <endian.h>         // __BYTE_ORDER 宏
#include <errno.h>          // errno
```

### 9.2 IP 头结构体

```c
struct ip_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t  ihl:4;        // 低 4 位
    uint8_t  version:4;    // 高 4 位
#else
    uint8_t  version:4;
    uint8_t  ihl:4;
#endif
    uint8_t  tos;
    uint16_t total_len;    // 网络字节序，读取时 ntohs
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;     // 单字节，不用转字节序
    uint16_t checksum;
    uint32_t src_ip;       // 网络字节序，inet_ntop 直接用
    uint32_t dst_ip;
} __attribute__((packed));
```

**为什么不用系统的 `struct iphdr`？**
教学目的：让你看到每个字段。系统的 `struct iphdr` 在 `<netinet/ip.h>` 里，
定义方式几乎一样，但字段名不同（`saddr` vs `src_ip`）。

### 9.3 TCP 头结构体

```c
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;          // 序列号
    uint32_t ack_seq;      // 确认号
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t res1:4;
    uint16_t data_off:4;   // TCP 头长度，单位 4 字节
    uint16_t fin:1;        // 标志位，从低位开始
    uint16_t syn:1;
    uint16_t rst:1;
    uint16_t psh:1;
    uint16_t ack:1;
    uint16_t urg:1;
    uint16_t res2:2;
#else
    // 大端顺序相反
    ...
#endif
    uint16_t window;       // 接收窗口
    uint16_t checksum;
    uint16_t urg_ptr;
} __attribute__((packed));
```

### 9.4 解析函数 parse_and_print

```c
static void parse_and_print(const uint8_t *pkt, int pkt_len, int filter_port)
{
    // 1. 解析 IP 头
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;

    if (ip->protocol != 6) return;  // 只看 TCP

    int ip_hdr_len = ip->ihl * 4;   // IHL 单位是 4 字节

    // 2. 解析 TCP 头（紧跟在 IP 头后面）
    const struct tcp_hdr *tcp = (const struct tcp_hdr *)(pkt + ip_hdr_len);

    // 3. 端口过滤
    if (filter_port > 0) {
        int src = ntohs(tcp->src_port);  // 网络序转主机序
        int dst = ntohs(tcp->dst_port);
        if (src != filter_port && dst != filter_port) return;
    }

    // 4. 格式化 IP 地址
    char src_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip->src_ip, src_ip_str, sizeof(src_ip_str));
    // inet_ntop 期望参数是网络字节序，所以不转

    // 5. 构建 flags 字符串
    char flags[64] = "";
    if (tcp->fin) strcat(flags, "FIN,");
    if (tcp->syn) strcat(flags, "SYN,");
    // ...

    // 6. 打印
    printf("[%-12s] %s:%d → %s:%d  seq=%u ack=%u win=%u data=%d\n",
           flags, src_ip_str, ntohs(tcp->src_port), ...,
           ntohl(tcp->seq), ntohl(tcp->ack_seq),
           ntohs(tcp->window), data_len);
}
```

**关键点**：
1. `pkt` 是 `recvfrom` 收到的原始字节，直接强转成结构体指针
2. TCP 头的地址 = `pkt + ip_hdr_len`（IP 头后面紧跟 TCP 头）
3. 多字节字段读取时要 `ntohs`/`ntohl`
4. `inet_ntop` 期望网络字节序，不转

### 9.5 主循环

```c
while (g_running) {
    ssize_t n = recvfrom(raw_fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&peer, &peer_len);
    if (n < 0) {
        if (errno == EINTR) continue;  // 被 Ctrl-C 打断
        err_sys("recvfrom error");
    }
    parse_and_print(buf, (int)n, filter_port);
}
```

`recvfrom` 每次返回一个完整的 IP 包。阻塞直到有包到达。

### 9.6 信号处理

```c
static volatile int g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;  /* 让主循环退出 */
}

int main(...) {
    signal(SIGINT,  on_signal);  /* Ctrl-C */
    signal(SIGTERM, on_signal);  /* kill */
    ...
}
```

`volatile` 告诉编译器不要把 `g_running` 优化到寄存器，
每次都从内存读，确保信号处理函数的修改能被主循环看到。

### 9.7 字节序演示函数

```c
void byte_order_demo(void)
{
    printf("==========================================\n");
    printf("  字节序演示\n");
    printf("==========================================\n\n");

    /* 1. 检测字节序 */
    uint32_t i = 1;
    printf("1. 当前主机字节序: %s\n\n",
           (*(uint8_t *)&i == 1) ? "小端 (Little-Endian)"
                                  : "大端 (Big-Endian)");

    /* 2. htons 演示 */
    uint16_t port = 8080;
    uint16_t port_net = htons(port);
    printf("2. 端口 8080 (0x1F90):\n");
    printf("   主机序: "); print_hex(&port, 2);     /* 90 1f */
    printf("   网络序: "); print_hex(&port_net, 2); /* 1f 90 */

    /* 3. htonl 演示 */
    uint32_t ip = 0x7F000001;
    uint32_t ip_net = htonl(ip);
    printf("3. IP 127.0.0.1 (0x7F000001):\n");
    printf("   主机序: "); print_hex(&ip, 4);       /* 01 00 00 7f */
    printf("   网络序: "); print_hex(&ip_net, 4);   /* 7f 00 00 01 */

    /* 4. 结构体对齐 */
    printf("4. 结构体对齐:\n");
    printf("   packed:   %zu\n", sizeof(struct packed_struct));    /* 5 */
    printf("   unpacked: %zu\n", sizeof(struct unpacked_struct));  /* 8 */
}
```

### 9.8 print_hex 辅助函数

```c
static void print_hex(const char *label, const void *p, size_t n)
{
    printf("  %-20s: ", label);
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        printf("%02x ", b[i]);
    }
    printf("\n");
}
```

逐字节打印内存，用于观察字节序。

---

## 10. 运行方法

### 10.1 编译

```bash
# 在 WSL2 项目根目录
cmake -B build -S .
cmake --build build
```

### 10.2 运行

```bash
# 抓所有 TCP 包（需要 sudo）
sudo build/bin/raw_sniff

# 只抓端口 8080 的包
sudo build/bin/raw_sniff 8080
```

### 10.3 触发三次握手

另开一个终端：

```bash
# 方法1：curl
curl http://localhost:8080/

# 方法2：nc 手动连接
nc localhost 8080
# 输入 GET / HTTP/1.1 然后按两次回车

# 方法3：python
python3 -c "import socket; s=socket.socket(); s.connect(('localhost',8080))"
```

### 10.4 预期输出

```
==========================================
  字节序演示
==========================================
...（字节序演示输出）

--- 开始抓包，请另开终端执行 curl localhost:8080 ---

[SYN]        127.0.0.1:54321 → 127.0.0.1:8080  seq=123456789 ack=0 win=64240 data=0
[SYN,ACK]    127.0.0.1:8080  → 127.0.0.1:54321 seq=987654321 ack=123456790 win=64240 data=0
[ACK]        127.0.0.1:54321 → 127.0.0.1:8080  seq=123456790 ack=987654322 win=64240 data=0
[PSH,ACK]    127.0.0.1:54321 → 127.0.0.1:8080  seq=123456790 ack=987654322 win=64240 data=78
[ACK]        127.0.0.1:8080  → 127.0.0.1:54321 seq=987654322 ack=123456868 win=64240 data=0
...
[FIN,ACK]    127.0.0.1:54321 → 127.0.0.1:8080  seq=123456868 ack=987654400 win=64240 data=0
[ACK]        127.0.0.1:8080  → 127.0.0.1:54321 seq=987654400 ack=123456869 win=64240 data=0
[FIN,ACK]    127.0.0.1:8080  → 127.0.0.1:54321 seq=987654400 ack=123456869 win=64240 data=0
[ACK]        127.0.0.1:54321 → 127.0.0.1:8080  seq=123456869 ack=987654401 win=64240 data=0
```

### 10.5 命令行参数

```bash
Usage: raw_sniff [port]

  port  只显示涉及该端口的包（可选）
  不传  显示所有 TCP 包
```

---

## 11. 对照实验

### 11.1 用 tcpdump 对比

```bash
# 同时运行我们的程序和 tcpdump
# 终端1：我们的程序
sudo build/bin/raw_sniff 8080

# 终端2：tcpdump
sudo tcpdump -i lo port 8080 -nn -S

# 终端3：触发
curl localhost:8080
```

tcpdump 的 `-S` 选项打印绝对序列号（默认是相对的），
对比我们的输出，应该完全一致。

### 11.2 观察 TIME_WAIT

```bash
# 发起一个连接然后关闭
nc localhost 8080
# Ctrl-C 关闭

# 立即查看连接状态
ss -tan | grep 8080
# 会看到 TIME-WAIT 状态，持续约 60 秒
```

### 11.3 观察 RST（异常关闭）

```bash
# 连接后不正常关闭（用 kill -9）
nc localhost 8080 &
# 找到 nc 的 PID
kill -9 $!
# 在 raw_sniff 输出里会看到 RST 包
```

### 11.4 不同窗口大小

```bash
# 修改接收缓冲区大小
sysctl -w net.ipv4.tcp_rmem="4096 87380 6291456"

# 然后连接，观察 Window 字段的变化
```

### 11.5 观察分片

```bash
# 发一个大于 MTU 的包（ping 大包）
ping -s 4000 localhost
# 在 raw_sniff 里能看到分片（但 raw_sniff 只看 TCP，ping 是 ICMP）
# 改用 TCP：用 nc 传大文件
nc -l 8080 > /dev/null &
nc localhost 8080 < big_file
# 如果 big_file > 1460 字节，会看到多个数据包
```

### 11.6 观察 TTL

```bash
# 连接到远程主机，看 TTL
curl http://example.com
# TTL 通常是 64 - 经过的路由器数
# 如果 TTL=50，说明经过了 14 个路由器
```

### 11.7 观察不同操作系统的默认 TTL

```bash
# Linux 默认 TTL=64
ping localhost  # ttl=64

# Windows 默认 TTL=128
# 某些网络设备 TTL=255
```

### 11.8 用 wireshark 对比

```bash
# 同时用 wireshark 抓包
sudo wireshark
# 过滤条件：tcp.port == 8080
# 对比 wireshark 的图形界面和我们的文本输出
```

wireshark 的好处：
- 图形界面，更直观
- 能看到 TCP 选项的详细解析
- 能追踪 TCP 流（Follow TCP Stream）
- 能统计 TCP 性能（Round Trip Time、Window Size）

---

## 12. 思考题

1. **为什么 SYN 和 FIN 各消耗一个序列号？**
   提示：如果 FIN 不消耗序列号，ACK 丢失时对端怎么知道要重传 FIN？

2. **为什么 ISN（初始序列号）是随机的？**
   提示：搜索 "TCP sequence prediction attack"。

3. **如果握手第二个包（SYN+ACK）丢失了，会发生什么？**
   提示：客户端会重传 SYN，服务器重传 SYN+ACK。

4. **为什么 raw socket 收不到本机发出的包？**
   提示：AF_INET + SOCK_RAW 只收"入站"包。要收"出站"包需要 AF_PACKET。

5. **IP 头的 IHL=5 意味着头部长度是多少？**
   5 * 4 = 20 字节。

6. **TCP 的 Window=0 意味着什么？对端应该怎么做？**
   停止发送数据，等待窗口更新。

7. **四次挥手中，如果最后一个 ACK 丢失了，会发生什么？**
   服务器（LAST_ACK 状态）会重传 FIN，客户端（TIME_WAIT）还能重发 ACK。
   这就是 TIME_WAIT 存在 2*MSL 的原因之一。

8. **我们的程序为什么收不到 SYN_RCVD 状态的包？**
   提示：AF_INET raw socket 收到的是已交付给 IP 层的包，
   三次握手由内核 TCP 栈处理，SYN_RCVD 是内核内部状态。

9. **为什么 TCP 头有 Data Offset 字段，而 UDP 头没有？**
   提示：TCP 有可变长 Options，UDP 头固定 8 字节。

10. **MSS 和 MTU 的关系？**
    MSS = MTU - IP 头 - TCP 头 = 1500 - 20 - 20 = 1460（以太网）

11. **如果客户端发 SYN 后立刻 RST，服务器会怎样？**
    服务器在 SYN_RCVD 状态收到 RST，回到 LISTEN。

12. **为什么 TIME_WAIT 是 2*MSL 而不是 1*MSL？**
    1*MSL 等最后 ACK 到达对端，1*MSL 等对端重传的 FIN 到达本端，共 2*MSL。

13. **TCP 为什么是面向字节流而不是面向消息？**
    没有消息边界，应用层自己定义边界（如 HTTP 用 \r\n\r\n）。

14. **如果同时发 FIN（同时关闭），状态怎么走？**
    双方都从 ESTABLISHED → FIN_WAIT_1 → CLOSING → TIME_WAIT → CLOSED。

15. **Window Scale=7 意味着什么？**
    实际窗口 = Window 字段 * 128，最大 65535 * 128 = 8MB。

---

## 13. 安全注意事项

### 13.1 raw socket 的风险

raw socket 能看到所有本机入站包，包括：
- 其他用户的密码、token
- SSL/TLS 加密前的明文（如果在本机发起）
- 内部网络通信

**所以**：
- 不要在生产环境长期开 raw socket
- 抓包文件（pcap）可能含敏感信息，不要外泄
- 用最小权限原则：能不用 root 就不用

### 13.2 给 raw socket 加权限

不用 sudo，给程序设置 capability：

```bash
# 给程序设置 CAP_NET_RAW
sudo setcap cap_net_raw+ep build/bin/raw_sniff

# 现在普通用户也能运行
./build/bin/raw_sniff
```

这样程序本身有权限，不需要整个进程是 root。

### 13.3 抓包文件的敏感信息

```bash
# tcpdump 写文件
sudo tcpdump -w capture.pcap
# capture.pcap 含所有包的原始数据，包括 HTTP 明文密码

# 清理敏感信息（wireshark 的 editcap）
editcap --inject-secrets tls,tls-keys.txt capture.pcap cleaned.pcap
```

### 13.4 raw socket 和防火墙

raw socket 读取的是**防火墙之前**的包（确切说是 IP 层之后）。
所以即使防火墙 drop 了包，raw socket 可能还是能看到。

### 13.5 伪造源 IP 的风险

```c
/* IPPROTO_RAW 可以伪造源 IP */
int raw_fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);

struct ip_hdr ip = {0};
ip.src_ip = inet_addr("1.2.3.4");  /* 伪造的源 IP */
/* ... 构造完整 IP 包 ... */
sendto(raw_fd, &ip, sizeof(ip), 0, ...);
```

这是 IP 欺骗攻击的基础。现代网络有反向路径过滤（rp_filter）防御。
**仅用于学习和测试，不要用于攻击**。

### 13.6 教学建议

- 在虚拟机或容器里做实验，不影响宿主机
- 实验完立即关掉 raw socket
- 不要把抓包数据上传到公共仓库

---

## 14. 常见问题

### Q1: 运行报 "Operation not permitted"

raw socket 需要 root 权限。用 `sudo` 运行：

```bash
sudo build/bin/raw_sniff
```

或者设置 capability：

```bash
sudo setcap cap_net_raw+ep build/bin/raw_sniff
./build/bin/raw_sniff
```

### Q2: 收不到包

可能原因：
1. 没有用 `sudo`
2. 没有流量经过本机（用 `curl localhost:PORT` 触发）
3. 端口过滤太严（去掉端口参数试试）
4. 防火墙挡了

### Q3: 序列号很大很乱

TCP 的 ISN 是随机的，不是从 0 开始。
用 `tcpdump -S` 对比，或者只看**增量**（后一个 seq - 前一个 seq）。

### Q4: 为什么看不到以太网帧头？

`AF_INET + SOCK_RAW` 从 IP 层开始收，不含链路层。
要看以太网帧，用 `AF_PACKET + SOCK_RAW + ETH_P_IP`。

### Q5: 为什么有些包的 data=0 但 seq 还是增加了？

SYN 和 FIN 各消耗一个序列号，即使 data=0。
纯 ACK 不消耗序列号。

### Q6: 为什么只看到入站包，看不到出站包？

`AF_INET + SOCK_RAW` 只收以本机为目的地的包。
要看出站包，用 `AF_PACKET + SOCK_RAW` 并设置 `ETH_P_ALL`。

### Q7: 为什么 SYN_RCVD 状态观察不到？

SYN_RCVD 是内核 TCP 栈的内部状态，非常短暂。
用 `ss -tan` 在握手瞬间可能看到，但很难捕捉。

### Q8: 为什么有些包的 win=0？

对端接收缓冲区满了，告诉本端别发数据。
本端应该停止发送，等窗口更新。

### Q9: 怎么看 TCP 选项？

我们的程序没解析 TCP 选项。用 wireshark 或 tcpdump -v：

```bash
sudo tcpdump -i lo port 8080 -nn -v
# 输出会包含 <mss 1460,nop,wscale 7,nop,nop,sackOK>
```

### Q10: 为什么 localhost 的 TTL=64？

localhost 不经过任何路由器，TTL 不减。
Linux 默认 TTL=64，所以 localhost 包 TTL=64。

---

## 小结

这个阶段你学到了：

| 知识点 | 掌握了什么 |
|--------|-----------|
| raw socket | 能看到 IP+TCP 完整包头 |
| IP 头部 | 每个字段的含义和位置 |
| TCP 头部 | seq/ack/window/flags |
| 三次握手 | 三个包的 seq/ack 变化规律 |
| 四次挥手 | FIN/ACK 的独立关闭 |
| TCP 状态机 | 11 个状态的转换 |
| TCP 选项 | MSS/Window Scale/SACK/Timestamp |
| 字节序 | ntohs/ntohl 什么时候用 |
| 结构体对齐 | packed 的必要性 |
| 位域 | 字节序对位域排列的影响 |

**下一步**：`stage1_echo_blocking` —— 用普通 socket 写一个 echo server，
理解 fd、listen、accept 在做什么。那时你会理解为什么 raw socket 看到的包
是内核帮你处理完握手后"递交"给应用的。

---

## 附录 A：TCP 常见默认值

| 参数 | Linux 默认 | 说明 |
|------|-----------|------|
| TTL | 64 | `/proc/sys/net/ipv4/ip_default_ttl` |
| MSS | 1460 | MTU(1500) - IP(20) - TCP(20) |
| Window Scale | 7 | `/proc/sys/net/ipv4/tcp_window_scaling` |
| SACK | 开启 | `/proc/sys/net/ipv4/tcp_sack` |
| Timestamp | 开启 | `/proc/sys/net/ipv4/tcp_timestamps` |
| SYN 重传次数 | 6 | `/proc/sys/net/ipv4/tcp_syn_retries` |
| FIN 重传次数 | 3 | `/proc/sys/net/ipv4/tcp_orphan_retries` |
| TIME_WAIT 时长 | 60s | 2 * MSL, MSL=30s |
| keepalive 时长 | 7200s | `/proc/sys/net/ipv4/tcp_keepalive_time` |

## 附录 B：进一步阅读

- RFC 791 - Internet Protocol
- RFC 793 - Transmission Control Protocol
- RFC 1122 - Requirements for Internet Hosts (TCP 部分修正)
- RFC 1323 - TCP Extensions for High Performance (Window Scale, Timestamp)
- RFC 2018 - TCP Selective Acknowledgment Options (SACK)
- RFC 7413 - TCP Fast Open
- 《TCP/IP Illustrated, Volume 1》- W. Richard Stevens
- 《UNIX Network Programming》- W. Richard Stevens
