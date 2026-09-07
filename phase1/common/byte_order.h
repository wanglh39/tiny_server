#ifndef TINY_BYTE_ORDER_H
#define TINY_BYTE_ORDER_H

/*
 * byte_order.h —— 网络字节序模块
 *
 * ============================================================
 * 为什么要单独讲字节序？（这是网络编程的第一个坑）
 * ============================================================
 *
 *   计算机内存里，一个 4 字节整数 0x01020304 有两种放法：
 *
 *     大端（Big-Endian，网络字节序）：
 *       地址:  0    1    2    3
 *       内容:  01   02   03   04    ← 高位在前，像人读数字的顺序
 *
 *     小端（Little-Endian，x86/ARM 主机字节序）：
 *       地址:  0    1    2    3
 *       内容:  04   03   02   01    ← 低位在前
 *
 *   TCP/IP 协议规定：网络上传输的多字节字段一律用大端（网络字节序）。
 *   但你的 x86 CPU 是小端的！
 *   所以每次在"主机内存"和"网络包"之间转换，都要调 htons/ntohs。
 *
 *   h = host（主机）  n = network（网络）  s = short(16位)  l = long(32位)
 *     htons：host → network short   把主机的 16 位数转成网络字节序
 *     ntohs：network → host short   反过来
 *     htonl / ntohl：32 位版本
 *
 * 教学要点：
 *   读完这个模块，你应该能回答：
 *   - 为什么 IP 地址 127.0.0.1 在包里是 7f 00 00 01 而不是 01 00 00 7f？
 *   - 为什么端口 8080（0x1F90）在包里是 1F 90 而不是 90 1F？
 *   - 为什么 struct in_addr 里的 s_addr 要用 htonl 转换？
 */

#include <stdint.h>

/*
 * byte_order_detect —— 运行时检测当前主机是大端还是小端
 * 返回 1 表示小端，0 表示大端
 *
 * 原理：把整数 1 的地址强转成 char*，看第一个字节是 0 还是 1
 *   小端：01 00 00 00（低位字节在低地址）→ 第一个字节是 01 → 返回 1
 *   大端：00 00 00 01（高位字节在低地址）→ 第一个字节是 00 → 返回 0
 */
int byte_order_detect(void);

/*
 * byte_order_print_hex —— 以十六进制打印一段内存
 * 用于直观看到字节在内存里的排列顺序
 * 用法： byte_order_print_hex("port 8080", &port, sizeof(port));
 */
void byte_order_print_hex(const char *label, const void *data, int len);

/*
 * byte_order_demo —— 运行一个完整的字节序演示
 * 打印主机字节序、演示 htons/ntohs 的效果
 * 在 stage0 开始前调用，建立直觉
 */
void byte_order_demo(void);

#endif /* TINY_BYTE_ORDER_H */