/*
 * config.c —— 服务器配置模块实现
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void config_init(server_config_t *cfg)
{
    cfg->port        = 8080;
    cfg->num_workers = 4;
    strcpy(cfg->www_root, "./www");
    cfg->backlog     = 512;
    cfg->log_file[0] = '\0';
}

int config_parse_args(server_config_t *cfg, int argc, char *argv[])
{
    /*
     * 命令行参数：
     *   -p PORT       端口
     *   -w WORKERS    工作线程数
     *   -r ROOT       静态文件根目录
     *   -f FILE       配置文件路径
     *   -l FILE       日志文件路径
     *   -h            帮助
     */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            cfg->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            cfg->num_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            strncpy(cfg->www_root, argv[++i], sizeof(cfg->www_root) - 1);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            config_parse_file(cfg, argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            strncpy(cfg->log_file, argv[++i], sizeof(cfg->log_file) - 1);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("用法: server [选项]\n");
            printf("  -p PORT     监听端口 (默认 8080)\n");
            printf("  -w WORKERS  工作线程数 (默认 4)\n");
            printf("  -r ROOT     静态文件根目录 (默认 ./www)\n");
            printf("  -f FILE     配置文件路径\n");
            printf("  -l FILE     日志文件路径\n");
            return -1;
        }
    }

    if (cfg->num_workers < 1) cfg->num_workers = 1;
    if (cfg->num_workers > 32) cfg->num_workers = 32;
    if (cfg->port < 1 || cfg->port > 65535) cfg->port = 8080;

    return 0;
}

int config_parse_file(server_config_t *cfg, const char *path)
{
    /*
     * 配置文件格式（简单的 key=value）：
     *   port = 8080
     *   workers = 4
     *   root = ./www
     *   log = server.log
     *
     * 为什么不用 JSON/YAML？
     *   教学项目，保持简单。key=value 足够用。
     *   生产项目可以用 libconfig / cJSON 等。
     */
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;  /* 文件不存在，不报错，用默认值 */
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* 跳过注释和空行 */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }

        /* 找等号 */
        char *eq = strchr(line, '=');
        if (!eq) continue;

        /* 分割 key 和 value */
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        /* 去掉前后空格 */
        while (*key == ' ' || *key == '\t') key++;
        char *kend = key + strlen(key) - 1;
        while (kend > key && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';

        while (*val == ' ' || *val == '\t') val++;
        char *vend = val + strlen(val) - 1;
        while (vend > val && (*vend == ' ' || *vend == '\t' || *vend == '\n' || *vend == '\r')) *vend-- = '\0';

        /* 赋值 */
        if (strcmp(key, "port") == 0) {
            cfg->port = atoi(val);
        } else if (strcmp(key, "workers") == 0) {
            cfg->num_workers = atoi(val);
        } else if (strcmp(key, "root") == 0) {
            strncpy(cfg->www_root, val, sizeof(cfg->www_root) - 1);
        } else if (strcmp(key, "log") == 0) {
            strncpy(cfg->log_file, val, sizeof(cfg->log_file) - 1);
        }
    }

    fclose(fp);
    return 0;
}

void config_print(const server_config_t *cfg)
{
    printf("  端口:       %d\n", cfg->port);
    printf("  工作线程:   %d\n", cfg->num_workers);
    printf("  根目录:     %s\n", cfg->www_root);
    printf("  日志文件:   %s\n", cfg->log_file[0] ? cfg->log_file : "(stderr)");
}