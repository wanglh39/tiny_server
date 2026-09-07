#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

/*
 * config.h —— 服务器配置模块
 *
 * 支持两种配置方式：
 *   1. 命令行参数（优先级高）
 *   2. 配置文件（server.conf，简单的 key=value 格式）
 *
 * 用法：
 *   ./server -p 8080 -w 4 -r ./www
 *   或
 *   ./server -f server.conf
 */

#include <stddef.h>

typedef struct {
    int  port;            /* 监听端口，默认 8080 */
    int  num_workers;     /* 工作线程数，默认 4 */
    char www_root[512];   /* 静态文件根目录，默认 ./www */
    int  backlog;         /* listen backlog，默认 512 */
    char log_file[512];   /* 日志文件路径，空则输出到 stderr */
} server_config_t;

/*
 * config_init —— 初始化为默认值
 */
void config_init(server_config_t *cfg);

/*
 * config_parse_args —— 解析命令行参数
 * 返回 0 成功，-1 失败
 */
int config_parse_args(server_config_t *cfg, int argc, char *argv[]);

/*
 * config_parse_file —— 解析配置文件
 * 返回 0 成功，-1 失败（文件不存在不报错，用默认值）
 */
int config_parse_file(server_config_t *cfg, const char *path);

/*
 * config_print —— 打印配置（启动时显示）
 */
void config_print(const server_config_t *cfg);

#endif /* SERVER_CONFIG_H */