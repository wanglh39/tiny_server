/*
 * router.c —— 路由模块实现
 */

#include "router.h"

#include <string.h>
#include <stdio.h>

void router_init(router_t *r)
{
    r->count = 0;
}

void router_add(router_t *r, const char *pattern,
                route_type_t type, handler_t handler)
{
    if (r->count >= 32) {
        return;  /* 满了 */
    }
    strncpy(r->routes[r->count].pattern, pattern,
            sizeof(r->routes[r->count].pattern) - 1);
    r->routes[r->count].type    = type;
    r->routes[r->count].handler = handler;
    r->count++;
}

route_t *router_match(router_t *r, const char *uri)
{
    /*
     * 匹配优先级：
     *   1. 先找精确匹配
     *   2. 再找前缀匹配
     *   3. 都没找到返回 NULL（交给静态文件）
     */
    for (int i = 0; i < r->count; i++) {
        if (r->routes[i].type == ROUTE_EXACT &&
            strcmp(r->routes[i].pattern, uri) == 0) {
            return &r->routes[i];
        }
    }

    for (int i = 0; i < r->count; i++) {
        if (r->routes[i].type == ROUTE_PREFIX &&
            strncmp(r->routes[i].pattern, uri,
                    strlen(r->routes[i].pattern)) == 0) {
            return &r->routes[i];
        }
    }

    return NULL;
}