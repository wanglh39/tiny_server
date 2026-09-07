/*
 * echo_server.c —— 阶段 1：阻塞式 echo server
 *
 * ============================================================
 * 这是最简单的 TCP 服务器：收到什么就回什么（echo）
 * ============================================================
 *
 * 核心流程：
 *   socket → bind → listen → accept → (read → write 循环) → close
 *
 * 特点：
 *   - 阻塞 IO：read 没数据时会卡住等待
 *   - 单连接：同时只能服务一个客户端，第二个客户端连上来会卡在 accept
 *
 * 教学要点：
 *   1. fd 是什么：socket 返回的 3，accept 返回的 4，都是"文件描述符"
 *   2. listen_fd vs conn_fd：3 是"门口"，4 才是"通道"
 *   3. 为什么只能服务一个客户端：accept 之后的 read 会阻塞，没法回去 accept 下一个
 *
 * 运行：
 *   build/bin/echo_server 8080
 *   另一个终端： nc localhost 8080
 *   再开一个：   nc localhost 8080  ← 会卡住！
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define MAX_LINE 4096
#define BACKLOG  128

int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    signal(SIGPIPE, SIG_IGN);  /* 对端关闭时 write 会触发 SIGPIPE，忽略它 */

    log_info("=== 阶段 1：阻塞式 echo server ===");
    log_info("端口: %d", port);

    /* ---------- 1. 创建监听 socket ---------- */
    /*
     * socket(AF_INET, SOCK_STREAM, 0)
     *   AF_INET：IPv4
     *   SOCK_STREAM：TCP（流式套接字）
     *   0：协议自动选择（TCP 对应 IPPROTO_TCP）
     *
     * 返回 fd=3（0/1/2 被 stdin/stdout/stderr 占了）
     * 这个 fd 是"监听 socket"，只负责接客，不负责数据传输
     */
    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);

    /* ---------- 2. 设置 SO_REUSEADDR ---------- */
    /*
     * 为什么需要 SO_REUSEADDR？
     *   服务器关闭后，端口会进入 TIME_WAIT 状态，持续 2*MSL（约 60 秒）
     *   这期间 bind 会报 "Address already in use"
     *   SO_REUSEADDR 允许绑定 TIME_WAIT 状态的端口，服务器能立即重启
     */
    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* ---------- 3. bind：绑定地址和端口 ---------- */
    /*
     * struct sockaddr_in 是 IPv4 的地址结构
     *   sin_family：地址族，必须和 socket 的 family 一致
     *   sin_port：端口，网络字节序，必须 htons
     *   sin_addr.s_addr：IP 地址，INADDR_ANY = 0.0.0.0 = 监听所有网卡
     *
     * 为什么要 INADDR_ANY 而不是 127.0.0.1？
     *   服务器可能有多个网卡（多个 IP），INADDR_ANY 表示都监听
     *   教学时用 127.0.0.1 也行，但生产环境通常用 INADDR_ANY
     */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);  /* 0.0.0.0 */

    Bind(listen_fd, (SA *)&server_addr, sizeof(server_addr));

    /* ---------- 4. listen：开始监听 ---------- */
    /*
     * listen 把 socket 从"主动连接"变成"被动监听"
     * 内核开始维护两个队列：
     *   - 未完成队列：收到 SYN，还没完成三次握手（SYN_RCVD 状态）
     *   - 已完成队列：三次握手完成，等待 accept（ESTABLISHED 状态）
     * backlog=128 限制已完成队列长度
     */
    Listen(listen_fd, BACKLOG);

    log_info("服务器开始监听 0.0.0.0:%d，等待连接...", port);
    printf("\n>>> 用 nc localhost %d 连接测试 <<<\n", port);
    printf(">>> 输入什么就回什么，Ctrl-C 退出 <<<\n\n");

    /* ---------- 5. accept 循环 ---------- */
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /*
         * accept 从已完成队列取出一个连接
         * 返回一个新的 fd（conn_fd），专门用于和这个客户端通信
         *
         * 阻塞行为：
         *   如果已完成队列为空，accept 会阻塞（卡住等待）
         *   直到有新连接完成三次握手才返回
         *
         * fd 分配规律：
         *   listen_fd=3 不变
         *   第一个客户端：conn_fd=4
         *   客户端关闭后，fd=4 被回收
         *   下一个客户端：conn_fd=4（复用）
         */
        int conn_fd = Accept(listen_fd, (SA *)&client_addr, &client_len);

        log_info("新连接建立，开始 echo 循环");

        /* ---------- 6. echo 循环 ---------- */
        /*
         * read 阻塞等待客户端发数据
         *   n > 0：读到 n 字节
         *   n = 0：客户端关闭连接（收到 FIN）
         *   n < 0：出错（被 wrap_posix 处理）
         *
         * 为什么只能服务一个客户端？
         *   这个 while 循环不退出，就没法回到上面的 accept
         *   第二个客户端连上来后，三次握手能完成（内核帮你做）
         *   但 accept 不会被调用，客户端的数据卡在内核缓冲区
         */
        char buf[MAX_LINE];
        ssize_t n;
        while ((n = Read(conn_fd, buf, sizeof(buf))) > 0) {
            /*
             * 把收到的数据原样写回去
             * Writen 确保写满 n 字节（循环 write）
             */
            Writen(conn_fd, buf, n);
        }

        /*
         * 走到这里说明 n=0（客户端关闭）或 n<0（出错）
         * 关闭 conn_fd，触发四次挥手
         */
        Close(conn_fd);
        log_info("连接关闭，等待下一个客户端...");
    }

    /* 理论上走不到这里 */
    Close(listen_fd);
    return 0;
}