#ifndef SERVER_ROUTER_H
#define SERVER_ROUTER_H

/*
 * router.h —— 路由模块
 *
 * 路由规则：
 *   精确匹配：URI 完全等于 pattern
 *   前缀匹配：URI 以 pattern 开头（用 * 结尾标记）
 *
 * 示例：
 *   router_add("/api/echo",  ROUTE_EXACT,  handle_echo);
 *   router_add("/api/*",     ROUTE_PREFIX, handle_api);
 *   其他 → 静态文件
 */

#include "http_parser.h"

/* 路由匹配类型 */
typedef enum {
    ROUTE_EXACT  = 0,  /* 精确匹配 */
    ROUTE_PREFIX = 1,  /* 前缀匹配 */
} route_type_t;

/* 请求处理函数类型 */
typedef void (*handler_t)(int fd, const http_request_t *req, void *userdata);

/* 路由规则 */
typedef struct {
    char        pattern[256];  /* 匹配模式 */
    route_type_t type;         /* 匹配类型 */
    handler_t   handler;       /* 处理函数 */
} route_t;

/* 路由表 */
typedef struct {
    route_t routes[32];  /* 最多 32 条规则 */
    int     count;
} router_t;

/*
 * router_init —— 初始化路由表
 */
void router_init(router_t *r);

/*
 * router_add —— 添加路由规则
 */
void router_add(router_t *r, const char *pattern,
                route_type_t type, handler_t handler);

/*
 * router_match —— 匹配路由
 * 返回匹配到的路由，NULL 表示没匹配（交给静态文件处理）
 */
route_t *router_match(router_t *r, const char *uri);

#endif /* SERVER_ROUTER_H */