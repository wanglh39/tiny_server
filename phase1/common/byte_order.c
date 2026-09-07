/*
 * byte_order.c —— 网络字节序模块实现
 *
 * 这个文件用"看得见"的方式演示字节序：
 *   把同一个数字在内存里的原始字节打印出来，
 *   让你亲眼看到 0x01020304 在小端机器上是 04 03 02 01。
 */

#include "byte_order.h"
#include "log.h"

#include <stdio.h>
#include <arpa/inet.h>   /* htons / ntohs / htonl / ntohl */
#include <string.h>

int byte_order_detect(void)
{
    /*
     * 经典检测手法：
     *   int i = 1;
     *   内存里要么是 [01 00 00 00]（大端）要么是 [00 00 00 01]（小端）
     *   取第一个字节：大端是 1，小端是 0
     *
     * 为什么不直接编译期判断？
     *   因为教学目的是让你"看到"内存布局，而不是优化性能。
     */
    uint32_t i = 1;
    return *(uint8_t *)&i;  /* 取 i 的第一个字节 */
}

void byte_order_print_hex(const char *label, const void *data, int len)
{
    /*
     * 把 data 指向的 len 个字节，按地址顺序打印成十六进制
     *
     * 例如端口 8080 = 0x1F90，在小端机器上：
     *   &port 指向的内存是 [90 1F]（低位先存）
     *   打印出来就是 "90 1f"
     *
     * 这就是为什么网络包里不能直接放主机的字节序——
     * 对端收到 "90 1f" 会解释成 0x901F = 36943，而不是 8080！
     */
    const uint8_t *p = (const uint8_t *)data;
    printf("  %-20s : ", label);
    for (int i = 0; i < len; i++) {
        printf("%02x ", p[i]);
    }
    printf("\n");
}

void byte_order_demo(void)
{
    printf("\n");
    printf("==========================================\n");
    printf("  字节序演示\n");
    printf("==========================================\n\n");

    /* ---------- 1. 检测主机字节序 ---------- */
    int little_endian = byte_order_detect();
    printf("1. 当前主机字节序: %s\n\n",
           little_endian ? "小端 (Little-Endian)" : "大端 (Big-Endian)");

    /* ---------- 2. 演示 16 位端口转换 ---------- */
    /*
     * 端口 8080 = 0x1F90
     * 小端主机内存：[90 1F]
     * 网络字节序  ：[1F 90]
     */
    uint16_t port_host = 8080;          /* 主机字节序的端口 */
    uint16_t port_net  = htons(port_host); /* 网络字节序的端口 */

    printf("2. 16 位端口转换 (htons):\n");
    printf("   端口值 = %d (0x%04X)\n", port_host, port_host);
    byte_order_print_hex("主机字节序内存", &port_host, 2);
    byte_order_print_hex("网络字节序内存", &port_net, 2);
    printf("   ntohs 转回来 = %d\n\n", ntohs(port_net));

    /* ---------- 3. 演示 32 位 IP 地址转换 ---------- */
    /*
     * IP 127.0.0.1 = 0x7F000001
     * 小端主机内存：[01 00 00 7F]
     * 网络字节序  ：[7F 00 00 01]
     *
     * 注意：inet_addr / inet_pton 返回的已经是网络字节序！
     * 所以直接放进 struct in_addr.s_addr，不需要再 htonl。
     * 这是初学者常犯的错：对 inet_addr 的结果再 htonl，就转两次了。
     */
    uint32_t ip_host = 0x7F000001;        /* 127.0.0.1 主机字节序 */
    uint32_t ip_net  = htonl(ip_host);    /* 网络字节序 */

    printf("3. 32 位 IP 地址转换 (htonl):\n");
    printf("   IP = 127.0.0.1 (0x%08X)\n", ip_host);
    byte_order_print_hex("主机字节序内存", &ip_host, 4);
    byte_order_print_hex("网络字节序内存", &ip_net, 4);
    printf("   ntohl 转回来 = 0x%08X\n\n", ntohl(ip_net));

    /* ---------- 4. 结构体对齐的坑 ---------- */
    /*
     * 定义 IP 头部结构体时，必须加 __attribute__((packed))，
     * 否则编译器会在字段间插入填充字节，导致包解析错位。
     *
     * 这里演示 packed 和非 packed 的差别。
     */
    struct __attribute__((packed)) packed_struct {
        uint8_t  a;    /* 1 字节 */
        uint32_t b;    /* 4 字节 */
    };

    struct unpacked_struct {
        uint8_t  a;    /* 1 字节 */
        uint32_t b;    /* 4 字节 */
    };

    printf("4. 结构体对齐 (packed vs 默认):\n");
    printf("   packed   struct 大小 = %zu (1 + 4 = 5，无填充)\n",
           sizeof(struct packed_struct));
    printf("   unpacked struct 大小 = %zu (a 后填充 3 字节对齐到 4)\n",
           sizeof(struct unpacked_struct));
    printf("   → 解析网络包必须用 packed，否则字段错位！\n\n");

    printf("==========================================\n");
    printf("  演示结束。接下来看 raw socket 抓包。\n");
    printf("==========================================\n\n");
}