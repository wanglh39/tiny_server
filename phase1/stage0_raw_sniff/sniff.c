/*
 * sniff.c —— 阶段 0：raw socket 抓包，手拆 IP/TCP 头部
 *
 * ============================================================
 * 这个程序做什么？
 * ============================================================
 *
 *   用 raw socket 收下所有经过本机的 TCP 包，
 *   然后自己解析 IP 头和 TCP 头的每一个字段，
 *   把三次握手、数据传输、四次挥手的过程打印出来。
 *
 *   运行后，另开一个终端执行 curl localhost:8080，
 *   你就能看到：
 *     [SYN]      client:port → server:8080  seq=xxx  ack=0
 *     [SYN,ACK]  server:8080 → client:port  seq=yyy  ack=xxx+1
 *     [ACK]      client:port → server:8080  seq=xxx+1  ack=yyy+1
 *     ...（数据包 PSH,ACK）...
 *     [FIN,ACK]  ...四次挥手...
 *
 * ============================================================
 * raw socket 是什么？
 * ============================================================
 *
 *   普通 socket(AF_INET, SOCK_STREAM, 0)：
 *     内核帮你完成 TCP/IP 头部的填充和解析，
 *     你只看到应用层数据（read/write 的内容）。
 *
 *   raw socket(AF_INET, SOCK_RAW, IPPROTO_TCP)：
 *     内核把 IP 头 + TCP 头 + 数据整个包都给你，
 *     你自己拆。这就是"看到底层"。
 *
 *   注意：
 *     1. raw socket 需要 root 权限（sudo）
 *     2. AF_INET + SOCK_RAW 收到的是"包含 IP 头的完整包"
 *     3. 收到的包是"以本机为目的地的"包，不收"本机发出的"包
 *        （要收发出的包需要 AF_PACKET，更底层，我们先用 AF_INET）
 *
 * ============================================================
 * 编译运行
 * ============================================================
 *
 *   cmake --build build
 *   sudo build/bin/raw_sniff           # 抓所有 TCP 包
 *   sudo build/bin/raw_sniff 8080      # 只抓端口 8080 的包
 *
 *   另一个终端：
 *   curl http://localhost:8080/        # 触发三次握手
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <endian.h>     /* __BYTE_ORDER */
#include <errno.h>

#include "error.h"
#include "log.h"
#include "byte_order.h"

/* ============================================================ */
/*  IP 头部结构体（RFC 791）                                     */
/* ============================================================ */
/*
 * 为什么自己定义而不是用 <netinet/ip.h> 的 struct iphdr？
 *   —— 教学目的：让你看到每个字段在头部的哪个 bit。
 *
 * 为什么必须 __attribute__((packed))？
 *   —— 网络包是紧凑排列的，字段间没有填充字节。
 *      如果不加 packed，编译器可能在 ihl 后插入填充，
 *      导致后续字段全部错位，解析出垃圾值。
 *
 * 位域的顺序：
 *   #if __BYTE_ORDER == __LITTLE_ENDIAN
 *     在小端机器上，先定义低字节字段
 *   #endif
 *   这是因为网络包是大端，而位域在内存里的排列方向
 *   依赖机器字节序。Linux 内核头文件就是这么处理的。
 */
struct ip_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t  ihl:4;        /* IP Header Length，单位 4 字节。值=5 表示 20 字节 */
    uint8_t  version:4;    /* IP 版本，IPv4=4 */
#else
    uint8_t  version:4;
    uint8_t  ihl:4;
#endif
    uint8_t  tos;          /* Type of Service，服务类型（DSCP/ECN） */
    uint16_t total_len;    /* 整个 IP 包长度（头+数据） */
    uint16_t id;           /* 标识，用于分片重组 */
    uint16_t frag_off;     /* 分片标志 + 偏移 */
    uint8_t  ttl;          /* Time to Live，每过一个路由器减 1，到 0 丢弃 */
    uint8_t  protocol;     /* 上层协议：6=TCP 17=UDP 1=ICMP */
    uint16_t checksum;     /* IP 头部校验和 */
    uint32_t src_ip;       /* 源 IP 地址（网络字节序） */
    uint32_t dst_ip;       /* 目的 IP 地址（网络字节序） */
} __attribute__((packed));

/* ============================================================ */
/*  TCP 头部结构体（RFC 793）                                    */
/* ============================================================ */
struct tcp_hdr {
    uint16_t src_port;     /* 源端口 */
    uint16_t dst_port;     /* 目的端口 */
    uint32_t seq;          /* 序列号：本包数据第一个字节的编号 */
    uint32_t ack_seq;      /* 确认号：期望收到的下一个字节编号 */
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t res1:4;       /* 保留 */
    uint16_t data_off:4;   /* TCP Header Length，单位 4 字节。值=5 表示 20 字节 */
    uint16_t fin:1;        /* FIN：我要关闭了 */
    uint16_t syn:1;        /* SYN：请求建立连接 / 同步序列号 */
    uint16_t rst:1;        /* RST：重置连接（异常关闭） */
    uint16_t psh:1;        /* PSH：请立即推给应用层 */
    uint16_t ack:1;        /* ACK：确认号字段有效 */
    uint16_t urg:1;        /* URG：紧急指针有效 */
    uint16_t res2:2;       /* 保留 */
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
    uint16_t window;       /* 接收窗口大小（流量控制） */
    uint16_t checksum;     /* TCP 校验和（含伪首部） */
    uint16_t urg_ptr;      /* 紧急指针 */
} __attribute__((packed));

/* ============================================================ */
/*  全局状态                                                     */
/* ============================================================ */
static volatile int g_running = 1;  /* Ctrl-C 时置 0，优雅退出 */

/* 信号处理函数 */
static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ============================================================ */
/*  解析并打印一个 IP+TCP 包                                      */
/* ============================================================ */
static void parse_and_print(const uint8_t *pkt, int pkt_len, int filter_port)
{
    if (pkt_len < (int)sizeof(struct ip_hdr)) {
        return;  /* 太短，不是完整的 IP 包 */
    }

    /* ---------- 1. 解析 IP 头 ---------- */
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;

    /* 只看 TCP（protocol=6），跳过 UDP/ICMP 等 */
    if (ip->protocol != 6) {
        return;
    }

    /* IHL 字段单位是 4 字节，值=5 表示头部长度 20 字节 */
    int ip_hdr_len = ip->ihl * 4;
    if (ip_hdr_len < 20 || pkt_len < ip_hdr_len + (int)sizeof(struct tcp_hdr)) {
        return;  /* 包不完整 */
    }

    /* ---------- 2. 解析 TCP 头 ---------- */
    /*
     * TCP 头紧跟在 IP 头后面
     * pkt + ip_hdr_len 就是 TCP 头的起始地址
     */
    const struct tcp_hdr *tcp = (const struct tcp_hdr *)(pkt + ip_hdr_len);

    /* 端口过滤：如果指定了 filter_port，只显示该端口的包 */
    if (filter_port > 0) {
        int src = ntohs(tcp->src_port);
        int dst = ntohs(tcp->dst_port);
        if (src != filter_port && dst != filter_port) {
            return;
        }
    }

    /* ---------- 3. 格式化 IP 地址 ---------- */
    char src_ip_str[INET_ADDRSTRLEN];
    char dst_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip->src_ip, src_ip_str, sizeof(src_ip_str));
    inet_ntop(AF_INET, &ip->dst_ip, dst_ip_str, sizeof(dst_ip_str));

    int src_port = ntohs(tcp->src_port);
    int dst_port = ntohs(tcp->dst_port);

    /* ---------- 4. 构建 flags 字符串 ---------- */
    /*
     * TCP 标志位是理解连接状态的关键：
     *   SYN：同步序列号，三次握手的前两包都带 SYN
     *   ACK：确认号有效，握手第三包开始都带 ACK
     *   FIN：发送方没有更多数据了，四次挥手用
     *   RST：异常重置连接
     *   PSH：数据要立即推给应用层（通常和 ACK 一起）
     */
    char flags[64] = "";
    if (tcp->fin) strcat(flags, "FIN,");
    if (tcp->syn) strcat(flags, "SYN,");
    if (tcp->rst) strcat(flags, "RST,");
    if (tcp->psh) strcat(flags, "PSH,");
    if (tcp->ack) strcat(flags, "ACK,");
    if (tcp->urg) strcat(flags, "URG,");
    /* 去掉末尾逗号 */
    int flen = strlen(flags);
    if (flen > 0 && flags[flen - 1] == ',') {
        flags[flen - 1] = '\0';
    }

    /* ---------- 5. 计算数据长度 ---------- */
    int tcp_hdr_len = tcp->data_off * 4;
    int data_len = ntohs(ip->total_len) - ip_hdr_len - tcp_hdr_len;

    /* ---------- 6. 打印 ---------- */
    /*
     * 格式：
     *   [SYN]  127.0.0.1:54321 → 127.0.0.1:8080  seq=12345 ack=0 win=64240 data=0
     *
     * 重点观察三次握手：
     *   包1 [SYN]      seq=x     ack=0      ← 客户端发起，ack 无意义所以是 0
     *   包2 [SYN,ACK]  seq=y     ack=x+1    ← 服务器回应，确认收到 x，发自己的 y
     *   包3 [ACK]      seq=x+1   ack=y+1    ← 客户端确认收到 y
     *
     * 关键规律：ack 总是 = 对方上一个 seq + 1（SYN/FIN 各占一个序号）
     */
    printf("[%-12s] %s:%d → %s:%d  seq=%u ack=%u win=%u data=%d\n",
           flags,
           src_ip_str, src_port,
           dst_ip_str, dst_port,
           ntohl(tcp->seq),
           ntohl(tcp->ack_seq),
           ntohs(tcp->window),
           data_len);
}

/* ============================================================ */
/*  main                                                         */
/* ============================================================ */
int main(int argc, char *argv[])
{
    int filter_port = 0;  /* 0 表示不过滤 */

    if (argc >= 2) {
        filter_port = atoi(argv[1]);
    }

    /* 禁用 stdout 缓冲：通过管道运行时也能立即看到输出 */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 打印启动信息 */
    printf("\n");
    printf("==========================================\n");
    printf("  raw socket TCP 抓包器 (stage0)\n");
    printf("==========================================\n");
    printf("  用法： sudo %s [port]\n", argv[0]);
    printf("  当前过滤端口： %s\n", filter_port > 0 ? argv[1] : "无（显示所有 TCP）");
    printf("  按 Ctrl-C 退出\n");
    printf("==========================================\n\n");

    /* 先跑字节序演示，建立直觉 */
    byte_order_demo();

    /* 注册 Ctrl-C 信号处理 */
    signal(SIGINT, on_sigint);

    /* ---------- 创建 raw socket ---------- */
    /*
     * AF_INET：IPv4
     * SOCK_RAW：原始套接字，收到的包包含 IP 头
     * IPPROTO_TCP：只收 TCP 包
     *
     * 为什么需要 root？
     *   raw socket 能看到所有包的内容，有安全风险，
     *   所以内核要求 CAP_NET_RAW 权限（root 或 sudo）。
     */
    int raw_fd = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (raw_fd < 0) {
        err_sys("创建 raw socket 失败（需要 sudo 权限）");
    }
    log_info("raw socket 创建成功，fd=%d", raw_fd);

    /* ---------- 设置：IP 头部包含在收到的数据里 ---------- */
    /*
     * IP_HDRINCL=1 表示：发送时我们自己填 IP 头
     * 对接收来说，AF_INET + SOCK_RAW 默认就包含 IP 头
     * 这里我们只接收，所以不需要设置这个选项
     * （但发送 raw 包时需要，这里提一下让你知道）
     */

    /* ---------- 接收循环 ---------- */
    uint8_t buf[65536];   /* 最大 IP 包 65535 字节 */
    struct sockaddr_in peer;
    socklen_t peer_len;

    printf("--- 开始抓包，请另开终端执行 curl localhost:8080 ---\n\n");

    while (g_running) {
        peer_len = sizeof(peer);

        /*
         * recvfrom：从 raw socket 读一个包
         * 返回值是整个包的长度（IP头 + TCP头 + 数据）
         *
         * 对 raw socket：
         *   - 每次 recvfrom 返回一个完整的 IP 包
         *   - buf 里从第 0 字节开始就是 IP 头
         *   - peer 里是发送方的地址
         */
        ssize_t n = recvfrom(raw_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &peer_len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  /* 被 Ctrl-C 信号打断 */
            }
            err_sys("recvfrom error");
        }

        /* 解析并打印这个包 */
        parse_and_print(buf, (int)n, filter_port);
    }

    /* ---------- 清理 ---------- */
    close(raw_fd);
    printf("\n\n--- 抓包结束 ---\n");
    return 0;
}