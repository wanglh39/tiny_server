/*
 * server.c —— 聚合版高级服务器主程序
 *
 * ============================================================
 *  整合 phase2 全部 7 个进阶特性到一个服务器：
 *    SO_REUSEPORT 多线程 + TLS + WebSocket + HTTP/2
 *    + 内存池 + 异步日志 + io_uring 异步文件读
 *
 *  架构：
 *    main → 解析配置 → 初始化异步日志 → 创建 SSL_CTX
 *         → 启动 N 个工作线程（每个线程独立 listen_fd via SO_REUSEPORT）
 *         → 等待线程结束
 *
 *  每个工作线程：
 *    独立 listen_fd + epoll → accept → 读前几字节检测协议
 *    → TLS / HTTP2 / WebSocket / HTTP1.1 分发
 *
 *  用法：
 *    build/bin/advanced_server -p 8443 -w 4 -r ./www
 *        --cert certs/cert.pem --key certs/key.pem
 * ============================================================
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* ---------- 全局服务器实例 ---------- */
server_t g_server;

/* ---------- 信号处理 ---------- */

static void on_signal(int sig)
{
    (void)sig;
    g_server.running = 0;
    /* 写一个字节到 pipe 唤醒 epoll_wait（简化：用 1 秒超时） */
}

/* ---------- 配置解析 ---------- */

void config_init(server_config_t *cfg)
{
    cfg->port        = 8443;
    cfg->num_workers = 4;
    strcpy(cfg->www_root, "./www");
    cfg->backlog     = 512;

    cfg->tls_enabled = 1;
    strcpy(cfg->cert_path, "certs/cert.pem");
    strcpy(cfg->key_path,  "certs/key.pem");

    cfg->uring_enabled = 1;
    cfg->log_file[0]   = '\0';
}

int config_parse_args(server_config_t *cfg, int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            cfg->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            cfg->num_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            strncpy(cfg->www_root, argv[++i], sizeof(cfg->www_root) - 1);
        } else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
            strncpy(cfg->cert_path, argv[++i], sizeof(cfg->cert_path) - 1);
            cfg->tls_enabled = 1;
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            strncpy(cfg->key_path, argv[++i], sizeof(cfg->key_path) - 1);
        } else if (strcmp(argv[i], "--no-tls") == 0) {
            cfg->tls_enabled = 0;
        } else if (strcmp(argv[i], "--no-uring") == 0) {
            cfg->uring_enabled = 0;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            strncpy(cfg->log_file, argv[++i], sizeof(cfg->log_file) - 1);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("\n");
            printf("用法: advanced_server [选项]\n\n");
            printf("选项:\n");
            printf("  -p PORT       监听端口 (默认 8443)\n");
            printf("  -w WORKERS    工作线程数 (默认 4)\n");
            printf("  -r ROOT       静态文件根目录 (默认 ./www)\n");
            printf("  --cert PATH   TLS 证书路径\n");
            printf("  --key PATH    TLS 私钥路径\n");
            printf("  --no-tls      禁用 TLS\n");
            printf("  --no-uring    禁用 io_uring\n");
            printf("  -l FILE       日志文件路径\n");
            printf("  -h            显示帮助\n\n");
            printf("示例:\n");
            printf("  advanced_server -p 8080 -w 4 -r ./www --no-tls\n");
            printf("  advanced_server -p 8443 --cert certs/cert.pem --key certs/key.pem\n");
            return -1;
        }
    }

    if (cfg->num_workers < 1) cfg->num_workers = 1;
    if (cfg->num_workers > 32) cfg->num_workers = 32;
    if (cfg->port < 1 || cfg->port > 65535) cfg->port = 8443;

    return 0;
}

void config_print(const server_config_t *cfg)
{
    printf("  监听端口:     %d\n", cfg->port);
    printf("  工作线程:     %d\n", cfg->num_workers);
    printf("  根目录:       %s\n", cfg->www_root);
    printf("  TLS:          %s", cfg->tls_enabled ? "启用" : "禁用");
    if (cfg->tls_enabled) printf(" (%s)", cfg->cert_path);
    printf("\n");
    printf("  io_uring:     %s\n", cfg->uring_enabled ? "启用" : "禁用");
    printf("  日志文件:     %s\n", cfg->log_file[0] ? cfg->log_file : "(stderr)");
}

/* ---------- 工作线程入口（在 worker.c 中实现） ---------- */

extern void *worker_main(void *arg);

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    /* 1. 解析配置 */
    config_init(&g_server.cfg);
    if (config_parse_args(&g_server.cfg, argc, argv) < 0) {
        return 0;
    }

    /* 2. 初始化异步日志 */
    if (g_server.cfg.log_file[0]) {
        async_log_init(g_server.cfg.log_file, LOG_LEVEL_INFO);
        alog_info("=== advanced_server 启动 ===");
    }

    /* 3. 初始化 OpenSSL */
    if (g_server.cfg.tls_enabled) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();

        g_server.ssl_ctx = tls_ctx_create(
            g_server.cfg.cert_path, g_server.cfg.key_path);
        if (!g_server.ssl_ctx) {
            fprintf(stderr, "TLS 初始化失败，退化为明文模式\n");
            g_server.cfg.tls_enabled = 0;
        }
    }

    /* 4. 打印启动信息 */
    printf("\n");
    printf("==========================================\n");
    printf("  advanced_server - 聚合版高级服务器\n");
    printf("  TLS + io_uring + 内存池 + 异步日志\n");
    printf("  + SO_REUSEPORT + WebSocket + HTTP/2\n");
    printf("==========================================\n");
    config_print(&g_server.cfg);
    printf("==========================================\n\n");

    /* 5. 信号处理 */
    g_server.running = 1;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* 6. 创建工作线程 */
    g_server.num_workers = g_server.cfg.num_workers;
    g_server.workers = calloc(g_server.num_workers, sizeof(worker_t));

    for (int i = 0; i < g_server.num_workers; i++) {
        g_server.workers[i].thread_id   = i;
        g_server.workers[i].num_threads = g_server.num_workers;
        g_server.workers[i].cfg         = &g_server.cfg;
        g_server.workers[i].ssl_ctx     = g_server.ssl_ctx;

        pthread_create(&g_server.workers[i].thread, NULL,
                       worker_main, &g_server.workers[i]);
    }

    printf(">>> curl http://localhost:%d/  (HTTP/1.1) <<<\n", g_server.cfg.port);
    if (g_server.cfg.tls_enabled)
        printf(">>> curl -k https://localhost:%d/  (TLS) <<<\n", g_server.cfg.port);
    printf(">>> curl --http2-prior-knowledge http://localhost:%d/  (HTTP/2) <<<\n", g_server.cfg.port);
    printf(">>> Ctrl-C 优雅退出 <<<\n\n");

    /* 7. 等待工作线程 */
    for (int i = 0; i < g_server.num_workers; i++) {
        pthread_join(g_server.workers[i].thread, NULL);
    }

    /* 8. 清理 */
    printf("\n>>> 服务器已关闭 <<<\n");

    if (g_server.ssl_ctx) {
        tls_ctx_free(g_server.ssl_ctx);
    }

    if (g_server.cfg.log_file[0]) {
        alog_info("=== advanced_server 关闭 ===");
        async_log_stop();
    }

    free(g_server.workers);
    return 0;
}