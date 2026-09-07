/*
 * echo_server_fork.c —— 阶段 2：多进程并发 echo server
 *
 * ============================================================
 * 阶段 1 的问题：只能服务一个客户端
 * 解决方案：accept 后 fork 一个子进程处理连接
 * ============================================================
 *
 * 核心流程：
 *   socket → bind → listen
 *   loop:
 *     accept → fork
 *       子进程: close(listen_fd) → echo 循环 → close(conn_fd) → exit
 *       父进程: close(conn_fd) → 继续 accept
 *
 * 教学要点：
 *   1. fork 后父子进程各有一份 fd 表，fd 引用计数 +1
 *   2. 父进程必须 close(conn_fd)，否则子进程退出后 conn_fd 不回收（fd 泄漏）
 *   3. 子进程必须 close(listen_fd)，否则子进程也能 accept（虽然不会调用）
 *   4. SIGCHLD 回收僵尸进程，否则子进程退出后变僵尸占用 PID
 *   5. SO_REUSEADDR 解决 TIME_WAIT
 *
 * 运行：
 *   build/bin/echo_server_fork 8080
 *   多个终端同时： nc localhost 8080  ← 都能连！
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "error.h"
#include "log.h"
#include "wrap_posix.h"

#define MAX_LINE 4096
#define BACKLOG  128

/* ---------- SIGCHLD 信号处理：回收僵尸子进程 ---------- */
/*
 * 子进程退出后不会立即消失，会变成"僵尸进程"（Z 状态），
 * 等父进程 wait/waitpid 来收尸。
 * 如果不回收，僵尸的 PID 和资源会一直占用。
 *
 * 为什么用 while 循环 waitpid？
 *   信号会排队但可能合并：如果 3 个子进程同时退出，
 *   父进程可能只收到 1 次 SIGCHLD（信号不排队）。
 *   while 循环用 WNOHANG 非阻塞地回收所有已退出的子进程。
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;  /* 保存 errno，防止 waitpid 覆盖 */

    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        log_debug("回收子进程 pid=%d", pid);
    }

    errno = saved_errno;  /* 恢复 errno */
}

int main(int argc, char *argv[])
{
    int port = 8080;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    signal(SIGPIPE, SIG_IGN);

    /* 注册 SIGCHLD 处理函数 */
    /*
     * signal vs sigaction：
     *   signal 是老接口，行为在不同系统不一致
     *   sigaction 更可靠，能设置 SA_RESTART 自动重启被中断的系统调用
     *   这里用 signal 简化教学，生产代码用 sigaction
     */
    signal(SIGCHLD, sigchld_handler);

    log_info("=== 阶段 2：多进程 echo server ===");
    log_info("端口: %d", port);

    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);

    int reuse = 1;
    Setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    Bind(listen_fd, (SA *)&server_addr, sizeof(server_addr));
    Listen(listen_fd, BACKLOG);

    log_info("服务器开始监听 0.0.0.0:%d", port);
    printf("\n>>> 多个终端同时 nc localhost %d 都能连 <<<\n\n", port);

    /* ---------- accept + fork 循环 ---------- */
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int conn_fd = Accept(listen_fd, (SA *)&client_addr, &client_len);

        /*
         * fork()：创建子进程
         *   返回 0：子进程
         *   返回 >0：父进程（返回值是子进程的 PID）
         *   返回 -1：失败
         *
         * fork 后，子进程"继承"父进程的整个内存空间和 fd 表
         * 关键：fd 不是复制一份新的，而是引用计数 +1
         *   父进程的 conn_fd=4，子进程的 conn_fd 也是 4，指向同一个内核对象
         *   内核里这个 fd 的引用计数 = 2
         */
        pid_t pid = fork();
        if (pid < 0) {
            err_sys("fork error");
        }

        if (pid == 0) {
            /* ========== 子进程 ========== */

            /*
             * 子进程不需要 listen_fd，关掉它
             * 为什么？如果不关：
             *   - 子进程持有 listen_fd 的引用，引用计数=2
             *   - 父进程退出时，listen_fd 不会真正关闭（引用计数还=1）
             *   - 端口一直被占用
             * 虽然子进程不会调用 accept，但持有引用就是不对的
             */
            Close(listen_fd);

            log_info("子进程 pid=%d 处理连接", getpid());

            /* echo 循环 */
            char buf[MAX_LINE];
            ssize_t n;
            while ((n = Read(conn_fd, buf, sizeof(buf))) > 0) {
                Writen(conn_fd, buf, n);
            }

            Close(conn_fd);
            log_info("子进程 pid=%d 退出", getpid());
            exit(0);  /* 子进程处理完就退出，不要回到 accept 循环 */

        } else {
            /* ========== 父进程 ========== */

            /*
             * 父进程不需要 conn_fd，关掉它
             * 为什么必须关？
             *   - 如果不关，父进程持有 conn_fd 引用
             *   - 子进程 close(conn_fd) 时引用计数从 2 变 1，不会真正关闭
             *   - 连接一直挂着，fd 也泄漏（每来一个客户端泄漏一个 fd）
             *   - 最终耗尽 fd 上限（ulimit -n，通常 1024）
             *
             * 这是最常见的 fd 泄漏原因！
             */
            Close(conn_fd);

            /* 父进程回到 accept，等待下一个连接 */
        }
    }

    Close(listen_fd);
    return 0;
}