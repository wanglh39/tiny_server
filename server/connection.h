#ifndef SERVER_CONNECTION_H
#define SERVER_CONNECTION_H

/*
 * connection.h —— 连接管理模块
 *
 * 每个连接关联一个 http_parser，负责：
 *   1. 读取数据
 *   2. 喂给 http_parser
 *   3. 解析完成后路由匹配
 *   4. 匹配到路由 → 调用 handler
 *   5. 没匹配 → 静态文件
 *   6. keep-alive：重置 parser 等下一个请求
 */

#include "http_parser.h"
#include "router.h"

/* 连接结构体 */
typedef struct {
    int            fd;
    http_parser_t  parser;
} conn_t;

/*
 * conn_create —— 创建连接
 */
conn_t *conn_create(int fd);

/*
 * conn_free —— 释放连接
 */
void conn_free(conn_t *conn);

/*
 * conn_handle_read —— 处理读事件
 *   返回 1 连接存活，0 连接关闭
 */
int conn_handle_read(conn_t *conn, router_t *router,
                     const char *www_root, int worker_id);

#endif /* SERVER_CONNECTION_H */