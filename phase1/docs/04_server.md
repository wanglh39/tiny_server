# 04 - 聚合版服务器（server/）

> 这是整个教学项目的最终产物。`server/` 目录把前面所有阶段整合成一个
> 完整的、可用的 Web 服务器。本篇会详细讲解每个模块、架构设计、
> 编译运行、扩展方法，让你不仅理解代码，还能自己改造它。
>
> 如果你只看一篇文档就想动手改代码，看这篇。

## 目录

1. [项目概述和设计理念](#1-项目概述和设计理念)
2. [完整架构图和数据流](#2-完整架构图和数据流)
3. [代码目录结构](#3-代码目录结构)
4. [模块详解：config](#4-模块详解config)
5. [模块详解：router](#5-模块详解router)
6. [模块详解：connection](#6-模块详解connection)
7. [模块详解：worker](#7-模块详解worker)
8. [模块详解：static_file](#8-模块详解static_file)
9. [模块详解：server（主程序）](#9-模块详解server主程序)
10. [模块间依赖关系](#10-模块间依赖关系)
11. [配置系统使用方法](#11-配置系统使用方法)
12. [路由系统使用方法](#12-路由系统使用方法)
13. [信号处理和优雅退出](#13-信号处理和优雅退出)
14. [编译和运行方法](#14-编译和运行方法)
15. [性能压测结果](#15-性能压测结果)
16. [和分阶段代码的关系](#16-和分阶段代码的关系)
17. [扩展指南](#17-扩展指南)
18. [常见问题](#18-常见问题)

---

## 1. 项目概述和设计理念

### 1.1 这是什么

`server/` 是一个用 C 写的小型 HTTP Web 服务器，约 600 行代码。
它不是 Nginx，不是 Apache，是一个**教学项目**——
把计算机网络课程里学到的知识整合成一个能跑的真实程序。

### 1.2 设计理念

**1. 最小化外部依赖**

只依赖 POSIX 标准库和 pthread。不用 libevent、libuv、libcurl 等第三方库。
目的：让你看到所有底层细节，不被框架的黑盒遮住。

**2. 分阶段演进**

项目按学习顺序分了 8 个阶段（stage0-stage7），每个阶段引入一个新概念。
`server/` 是最终聚合，把所有阶段的技术整合：

```
stage0 raw socket     → 字节序、结构体对齐（基础）
stage1 阻塞 echo      → socket API 基础
stage2 fork echo      → 多进程并发
stage3 select echo    → IO 多路复用
stage4 epoll echo     → epoll + ET
stage5 HTTP 解析      → 状态机、分包粘包
stage6 主从 Reactor   → 多线程、pipe 通信
stage7 Web server     → sendfile、MIME、路由
server/ 聚合版        → 以上全部
```

**3. 模块化设计**

每个功能一个 `.c`/`.h` 文件，职责单一：

```
config.c       配置解析
router.c       路由匹配
connection.c   连接管理
worker.c       工作线程
static_file.c  静态文件服务
server.c       主程序
```

**4. 可读性优先**

- 每个函数有注释说明做什么
- 关键步骤有中文注释
- 错误处理用 `err_sys` 直接退出（教学简化，生产要更优雅）
- 变量名英文，注释中文

### 1.3 功能列表

- [x] HTTP/1.1 请求解析（状态机）
- [x] keep-alive 长连接
- [x] 主从 Reactor 多线程
- [x] epoll ET 模式
- [x] 静态文件服务（sendfile 零拷贝）
- [x] 路由系统（精确 + 前缀匹配）
- [x] MIME 类型识别
- [x] 路径安全（防穿越攻击）
- [x] 配置文件 + 命令行参数
- [x] 信号处理 + 优雅退出
- [x] 日志系统
- [ ] HTTPS（未实现，扩展指南里有说明）
- [ ] WebSocket（未实现）
- [ ] HTTP/2（未实现）

### 1.4 不做什么（教学简化）

- 不做生产级错误处理（直接 exit 而非重试）
- 不做内存池（用 malloc/free）
- 不做连接限流（生产要防 DDoS）
- 不做 TLS（教学聚焦 HTTP）
- 不做配置热加载（要重启才生效）

---

## 2. 完整架构图和数据流

### 2.1 整体架构

```
                    ┌─────────────────────────────────┐
                    │         server.c (main)         │
                    │  解析配置 → 设置路由 → 启动 worker │
                    │  → 创建 listen_fd → 主 reactor    │
                    └──────────────┬──────────────────┘
                                   │
                    ┌──────────────┴──────────────┐
                    │       主 reactor (主线程)     │
                    │  epoll 只监听 listen_fd       │
                    │  循环 accept 新连接           │
                    │  round-robin 分发给 worker    │
                    └──────────────┬──────────────┘
                                   │ pipe 通知
                    ┌──────────────┼──────────────┐
                    ↓              ↓              ↓
              ┌──────────┐   ┌──────────┐   ┌──────────┐
              │ worker 0 │   │ worker 1 │   │ worker N │
              │ (sub     │   │ (sub     │   │ (sub     │
              │  reactor)│   │  reactor)│   │  reactor)│
              │          │   │          │   │          │
              │ epoll:   │   │ epoll:   │   │ epoll:   │
              │  notify  │   │  notify  │   │  notify  │
              │  conn1   │   │  conn3   │   │  conn5   │
              │  conn2   │   │  conn4   │   │  conn6   │
              └────┬─────┘   └────┬─────┘   └────┬─────┘
                   │              │              │
                   ↓              ↓              ↓
              ┌──────────────────────────────────────┐
              │        connection.c (每个连接)        │
              │  read → http_parser_feed → 路由匹配   │
              │  → handler 或 serve_static_file      │
              │  → keep-alive 重置 parser             │
              └──────────────────────────────────────┘
```

### 2.2 一次请求的完整数据流

```
1. 客户端发起 TCP 连接
   client → 三次握手 → 服务器内核

2. 主 reactor accept
   主线程 epoll_wait → listen_fd 就绪
   → accept() 返回 conn_fd
   → worker_dispatch(workers, num, conn_fd)
   → write(worker[target].write_fd, &conn_fd, sizeof(int))

3. worker 接收新连接
   工作线程 epoll_wait → notify_fd 就绪
   → read(notify_fd, &conn_fd, sizeof(int))
   → set_nonblocking(conn_fd)
   → conn = conn_create(conn_fd)
   → epoll_ctl(my_epfd, ADD, conn_fd, {EPOLLIN|EPOLLET, ptr=conn})

4. 客户端发 HTTP 请求
   client → "GET /api/info HTTP/1.1\r\n..." → 服务器

5. worker 处理读事件
   工作线程 epoll_wait → conn_fd 就绪
   → conn_handle_read(conn, router, www_root, worker_id)
   → read(conn->fd, buf, 4096)
   → http_parser_feed(&conn->parser, buf, n)
   → 解析完成 → dispatch_request()

6. 路由匹配
   dispatch_request:
   → route = router_match(router, "/api/info")
   → 匹配到 handle_info → 调用 handler

7. handler 发响应
   handle_info:
   → 构造 JSON body
   → 构造 HTTP 响应头
   → write(conn->fd, header, hlen)
   → write(conn->fd, body, body_len)

8. keep-alive
   → http_parser_reset(&conn->parser)
   → 继续等下一个请求（不关连接）

9. 客户端关闭
   → read 返回 0
   → close(conn->fd)
   → epoll_ctl(DEL)
   → conn_free(conn)
```

### 2.3 静态文件请求的数据流

```
1-5. 同上（解析出 "GET /index.html"）

6. 路由匹配
   → router_match 返回 NULL（没匹配到 /api/*）
   → serve_static_file(fd, "/index.html", www_root)

7. 静态文件服务
   → 拼接路径：www_root + "/index.html"
   → 检查路径安全（无 ".."）
   → stat() 获取文件信息
   → open() 打开文件
   → get_mime_type() → "text/html"
   → 发送响应头
   → sendfile(conn_fd, file_fd, ...)  ← 零拷贝
   → close(file_fd)

8. keep-alive 同上
```

---

## 3. 代码目录结构

### 3.1 server/ 目录

```
server/
├── CMakeLists.txt      CMake 构建配置
├── config.h            配置模块头文件
├── config.c            配置模块实现
├── router.h            路由模块头文件
├── router.c            路由模块实现
├── connection.h        连接管理头文件
├── connection.c        连接管理实现
├── worker.h            工作线程头文件
├── worker.c            工作线程实现
├── static_file.h       静态文件头文件
├── static_file.c       静态文件实现
└── server.c            主程序
```

### 3.2 依赖的 common/ 模块

```
common/
├── error.h/c           错误处理（err_sys, err_dump）
├── log.h/c             日志系统（log_info, log_error）
├── wrap_posix.h/c      POSIX 函数包装（Socket, Bind, Listen）
└── http_parser.h/c     HTTP 状态机解析器
```

### 3.3 整个项目结构

```
webserve/
├── CMakeLists.txt          顶层 CMake
├── README.md               项目说明
├── common/                 公共模块
├── docs/                   文档（你在看的）
├── lab/                    实验脚本
├── scripts/                辅助脚本
├── server/                 ★ 聚合版服务器（本篇讲这个）
├── stage0_raw_sniff/       阶段 0：raw socket 抓包
├── stage1_echo_blocking/   阶段 1：阻塞 echo
├── stage2_echo_fork/       阶段 2：多进程 echo
├── stage3_echo_select/     阶段 3：select echo
├── stage4_echo_epoll/      阶段 4：epoll echo
├── stage5_http_parser/     阶段 5：HTTP 解析
├── stage6_reactor/         阶段 6：主从 Reactor
├── stage7_webserver/       阶段 7：Web 服务器
├── www/                    静态网站根目录
└── build/                  编译输出
    └── bin/
        └── server          ★ 编译出的可执行文件
```

### 3.4 www/ 目录（静态文件）

```
www/
├── index.html         首页
├── 404.html           404 错误页
├── style.css          样式
├── script.js          脚本
└── images/            图片目录
    └── logo.png
```

---

## 4. 模块详解：config

### 4.1 职责

解析配置，来源有两个：
1. 命令行参数（`-p 8080 -w 4`）
2. 配置文件（`server.conf`，key=value 格式）

命令行优先级高于配置文件。

### 4.2 配置项

```c
typedef struct {
    int  port;            /* 监听端口，默认 8080 */
    int  num_workers;     /* 工作线程数，默认 4 */
    char www_root[512];   /* 静态文件根目录，默认 ./www */
    int  backlog;         /* listen backlog，默认 512 */
    char log_file[512];   /* 日志文件路径，空则 stderr */
} server_config_t;
```

### 4.3 API

```c
void config_init(server_config_t *cfg);              /* 初始化默认值 */
int  config_parse_args(server_config_t *cfg, int argc, char *argv[]);  /* 解析命令行 */
int  config_parse_file(server_config_t *cfg, const char *path);        /* 解析配置文件 */
void config_print(const server_config_t *cfg);       /* 打印配置 */
```

### 4.4 命令行参数

```
-p PORT       监听端口（默认 8080）
-w WORKERS    工作线程数（默认 4）
-r ROOT       静态文件根目录（默认 ./www）
-f FILE       配置文件路径
-l FILE       日志文件路径
-h            帮助
```

### 4.5 配置文件格式

```bash
# server.conf
# 注释以 # 开头

port = 8080
workers = 4
root = ./www
log = server.log
```

简单的 key=value 格式，不用 JSON/YAML（教学简化）。

### 4.6 实现要点

```c
/* config_parse_args */
for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
        cfg->port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
        cfg->num_workers = atoi(argv[++i]);
    }
    /* ... */
}

/* 边界检查 */
if (cfg->num_workers < 1) cfg->num_workers = 1;
if (cfg->num_workers > 32) cfg->num_workers = 32;
if (cfg->port < 1 || cfg->port > 65535) cfg->port = 8080;
```

```c
/* config_parse_file */
FILE *fp = fopen(path, "r");
char line[512];
while (fgets(line, sizeof(line), fp)) {
    if (line[0] == '#' || line[0] == '\n') continue;  /* 跳过注释和空行 */

    char *eq = strchr(line, '=');  /* 找等号 */
    if (!eq) continue;

    *eq = '\0';
    char *key = line;
    char *val = eq + 1;
    /* 去空格、赋值 */
}
```

### 4.7 使用示例

```c
server_config_t cfg;
config_init(&cfg);                    /* 先设默认值 */
config_parse_args(&cfg, argc, argv);  /* 命令行覆盖默认值 */
/* 如果命令行有 -f server.conf，会再读配置文件 */
config_print(&cfg);                   /* 打印生效的配置 */
```

---

## 5. 模块详解：router

### 5.1 职责

路由匹配：根据 URI 找到对应的处理函数。

### 5.2 路由规则

```c
typedef enum {
    ROUTE_EXACT,   /* 精确匹配：URI 完全等于 pattern */
    ROUTE_PREFIX   /* 前缀匹配：URI 以 pattern 开头 */
} route_type_t;

typedef void (*handler_t)(int fd, const http_request_t *req, void *userdata);

typedef struct {
    char        pattern[256];  /* 匹配模式 */
    route_type_t type;         /* 匹配类型 */
    handler_t   handler;       /* 处理函数 */
} route_t;

typedef struct {
    route_t routes[32];  /* 最多 32 条规则 */
    int     count;
} router_t;
```

### 5.3 API

```c
void router_init(router_t *r);
void router_add(router_t *r, const char *pattern, route_type_t type, handler_t handler);
route_t *router_match(router_t *r, const char *uri);
```

### 5.4 匹配优先级

```c
route_t *router_match(router_t *r, const char *uri)
{
    /* 1. 先找精确匹配 */
    for (int i = 0; i < r->count; i++) {
        if (r->routes[i].type == ROUTE_EXACT &&
            strcmp(r->routes[i].pattern, uri) == 0) {
            return &r->routes[i];
        }
    }

    /* 2. 再找前缀匹配 */
    for (int i = 0; i < r->count; i++) {
        if (r->routes[i].type == ROUTE_PREFIX &&
            strncmp(r->routes[i].pattern, uri,
                    strlen(r->routes[i].pattern)) == 0) {
            return &r->routes[i];
        }
    }

    return NULL;  /* 3. 都没找到，返回 NULL */
}
```

精确匹配优先于前缀匹配，避免 `/api/info` 被前缀 `/api/` 抢走。

### 5.5 内置路由

```c
static void setup_routes(router_t *router)
{
    router_init(router);

    router_add(router, "/api/info", ROUTE_EXACT,  handle_info);
    router_add(router, "/api/echo", ROUTE_EXACT,  handle_echo);
    router_add(router, "/api/",    ROUTE_PREFIX, handle_info);
}
```

- `/api/info` → 返回服务器信息（JSON）
- `/api/echo` → 回显请求信息（HTML）
- `/api/*` → 前缀匹配，其他 /api/ 路径也返回信息
- 其他 → 静态文件

### 5.6 handler 函数签名

```c
void handle_info(int fd, const http_request_t *req, void *userdata)
{
    /* fd: 客户端连接的 fd，用于 write 响应 */
    /* req: 解析出的 HTTP 请求 */
    /* userdata: 用户数据（如 worker_id） */

    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"method\":\"%s\",\"uri\":\"%s\"}",
        http_method_str(req->method), req->uri);

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: keep-alive\r\n\r\n", body_len);

    write(fd, header, hlen);
    write(fd, body, body_len);
}
```

---

## 6. 模块详解：connection

### 6.1 职责

管理一个 HTTP 连接的生命周期：
1. 读数据
2. 喂给 HTTP 解析器
3. 解析完成后路由匹配
4. 匹配到路由 → 调用 handler
5. 没匹配 → 静态文件
6. keep-alive：重置解析器等下一个请求

### 6.2 连接结构体

```c
typedef struct? struct {
    int            fd;       /* 客户端 fd */
    http_parser_t  parser;   /* HTTP 解析器 */
} conn_t;
```

每个连接关联一个 parser，parser 内部维护解析状态和缓冲区。

### 6.3 API

```c
conn_t *conn_create(int fd);                    /* 创建连接 */
void conn_free(conn_t *conn);                   /* 释放连接 */
int conn_handle_read(conn_t *conn, router_t *router,
                     const char *www_root, int worker_id);  /* 处理读事件 */
```

`conn_handle_read` 返回 1 表示连接存活，0 表示要关闭。

### 6.4 请求分发

```c
static void dispatch_request(int fd, const http_request_t *req,
                             router_t *router, const char *www_root,
                             int worker_id)
{
    /* 1. 尝试路由匹配 */
    route_t *route = router_match(router, req->uri);

    if (route) {
        /* 匹配到路由，调用 handler */
        route->handler(fd, req, (void *)(long)worker_id);
        return;
    }

    /* 2. 没匹配到，交给静态文件 */
    serve_static_file(fd, req->uri, www_root);
}
```

### 6.5 处理读事件的核心循环

```c
int conn_handle_read(conn_t *conn, router_t *router,
                     const char *www_root, int worker_id)
{
    char buf[4096];

    for (;;) {
        ssize_t nread = read(conn->fd, buf, sizeof(buf));

        if (nread > 0) {
            /* 喂给解析器 */
            http_parse_result_t result;
            result = http_parser_feed(&conn->parser, buf, nread);

            if (result == HTTP_PARSE_DONE) {
                /* 解析完成，分发请求 */
                dispatch_request(conn->fd, &conn->parser.request,
                                router, www_root, worker_id);

                /* keep-alive：重置解析器 */
                http_parser_reset(&conn->parser);

                /* 检查粘包：缓冲区是否有残留 */
                if (conn->parser.buf_len > 0) {
                    http_parse_result_t r2;
                    r2 = http_parser_feed(&conn->parser, "", 0);
                    if (r2 == HTTP_PARSE_DONE) {
                        dispatch_request(conn->fd, &conn->parser.request,
                                        router, www_root, worker_id);
                        http_parser_reset(&conn->parser);
                    }
                }
                continue;  /* 继续读，看还有没有数据 */

            } else if (result == HTTP_PARSE_ERROR) {
                /* 解析错误，发 400 */
                const char *err = "HTTP/1.1 400 Bad Request\r\n"
                                  "Content-Length: 0\r\n\r\n";
                write(conn->fd, err, strlen(err));
                return 0;  /* 关闭连接 */
            }
            /* NEED_MORE：继续 read */

        } else if (nread == 0) {
            return 0;  /* 客户端关闭 */

        } else {
            /* nread < 0 */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;  /* 读完了，连接存活 */
            }
            if (errno == EINTR) {
                continue;  /* 信号打断，重试 */
            }
            return 0;  /* 其他错误，关闭 */
        }
    }
}
```

### 6.6 关键设计点

**1. ET 模式的循环 read**

ET 模式下 epoll 只通知一次，必须循环 read 到 EAGAIN。
`for (;;)` 循环 + `errno == EAGAIN` 退出。

**2. 粘包处理**

解析完一个请求后，检查 parser 缓冲区是否还有数据。
如果有，尝试解析下一个请求（`http_parser_feed(parser, "", 0)`）。

**3. keep-alive**

解析完不关连接，重置 parser 继续等下一个请求。
是否真的 keep-alive 由请求头 `Connection` 决定（parser 里处理）。

---

## 7. 模块详解：worker

### 7.1 职责

工作线程（sub reactor）：
- 有独立的 epoll
- 通过 pipe 接收主线程分发的新连接
- 处理自己负责的连接的读写

### 7.2 工作线程结构

```c
typedef struct {
    int       epoll_fd;    /* 自己的 epoll */
    int       notify_fd;   /* pipe 读端（接收主线程通知） */
    int       write_fd;    /* pipe 写端（主线程往这里写 conn_fd） */
    int       thread_id;   /* 线程编号 */
    pthread_t thread;      /* 线程句柄 */
} worker_t;
```

### 7.3 启动工作线程

```c
void workers_start(worker_t *workers, int num,
                   router_t *router, const char *www_root)
{
    for (int i = 0; i < num; i++) {
        workers[i].thread_id = i;
        workers[i].epoll_fd  = epoll_create(1);

        /* 创建 pipe 用于主线程通知 */
        int pipe_fd[2];
        pipe(pipe_fd);
        set_nonblocking(pipe_fd[0]);
        set_nonblocking(pipe_fd[1]);

        workers[i].notify_fd = pipe_fd[0];
        workers[i].write_fd  = pipe_fd[1];

        /* 把 notify_fd 加入 epoll */
        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = pipe_fd[0];
        epoll_ctl(workers[i].epoll_fd, EPOLL_CTL_ADD, pipe_fd[0], &ev);

        /* 启动线程 */
        pthread_create(&workers[i].thread, NULL, worker_loop, &g_ctx[i]);
    }
}
```

### 7.4 工作线程主循环

```c
static void *worker_loop(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    worker_t *w = ctx->self;

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == w->notify_fd) {
                /* 新连接通知 */
                int conn_fd;
                while (read(w->notify_fd, &conn_fd, sizeof(int)) > 0) {
                    set_nonblocking(conn_fd);
                    conn_t *conn = conn_create(conn_fd);

                    struct epoll_event ev;
                    ev.events   = EPOLLIN | EPOLLET;
                    ev.data.ptr = conn;
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);
                }
            } else {
                /* 连接有数据 */
                conn_t *conn = events[i].data.ptr;

                int alive = conn_handle_read(conn, ctx->router,
                                            ctx->www_root, w->thread_id);
                if (!alive) {
                    close(conn->fd);
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    conn_free(conn);
                }
            }
        }
    }

    return NULL;
}
```

### 7.5 分发策略

```c
static int next_worker = 0;

void worker_dispatch(worker_t *workers, int num, int conn_fd)
{
    /* round-robin：轮流分发 */
    int target = next_worker;
    next_worker = (next_worker + 1) % num;

    /* 通过 pipe 通知工作线程 */
    write(workers[target].write_fd, &conn_fd, sizeof(int));
}
```

round-robin 简单均匀，但不考虑各 worker 的实际负载。
生产级可以用"最少连接"策略。

### 7.6 两种事件的区分

工作线程的 epoll 监听两类 fd：
1. `notify_fd`：主线程通知新连接
2. `conn_fd`：客户端连接有数据

区分方法：`events[i].data.fd == w->notify_fd`。

- notify_fd 用 `data.fd` 存储（小整数）
- conn_fd 用 `data.ptr` 存储 conn 指针（堆地址，不会和 notify_fd 混淆）

### 7.7 为什么用 pipe 而非全局队列

- pipe 是 fd，能加入 epoll，和 IO 事件统一处理
- pipe 有内核缓冲，不会丢通知（只要不溢出）
- 每个工作线程独立 pipe，无锁

缺点：write/read 有拷贝开销（4 字节 conn_fd）。
生产级可以用 eventfd（更轻量）或无锁队列。

---

## 8. 模块详解：static_file

### 8.1 职责

静态文件服务：
- URI → 文件路径
- 路径安全检查
- 获取 MIME 类型
- sendfile 零拷贝发送

### 8.2 完整流程

```c
int serve_static_file(int fd, const char *uri, const char *www_root)
{
    /* 1. URI → 文件路径 */
    char file_path[1024];
    if (strcmp(uri, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "%s/index.html", www_root);
    } else {
        snprintf(file_path, sizeof(file_path), "%s%s", www_root, uri);
    }

    /* 2. 路径安全检查 */
    if (strstr(file_path, "..") != NULL) {
        send_error(fd, 403, "Forbidden", "路径穿越被拦截");
        return -1;
    }

    /* 3. stat 获取文件信息 */
    struct stat st;
    if (stat(file_path, &st) < 0) {
        send_error(fd, 404, "Not Found", "文件不存在");
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        send_error(fd, 403, "Forbidden", "不是普通文件");
        return -1;
    }

    /* 4. 打开文件 */
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 404, "Not Found", "无法打开文件");
        return -1;
    }

    /* 5. 发送响应头 */
    const char *mime = get_mime_type(file_path);
    send_header(fd, 200, "OK", mime, st.st_size);

    /* 6. sendfile 零拷贝发送 */
    off_t offset = 0;
    ssize_t sent = 0;
    while (sent < st.st_size) {
        ssize_t n = sendfile(fd, file_fd, &offset, st.st_size - sent);
        if (n <= 0) break;
        sent += n;
    }

    close(file_fd);
    return 0;
}
```

### 8.3 MIME 类型表

```c
static const mime_entry_t mime_table[] = {
    {".html", "text/html"},
    {".htm",  "text/html"},
    {".css",  "text/css"},
    {".js",   "application/javascript"},
    {".json", "application/json"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".txt",  "text/plain"},
    {".pdf",  "application/pdf"},
    {".xml",  "application/xml"},
    {".zip",  "application/zip"},
    {".mp4",  "video/mp4"},
    {".woff", "font/woff"},
    {".woff2","font/woff2"},
    {NULL,    "application/octet-stream"},  /* 默认 */
};
```

### 8.4 路径安全

```c
if (strstr(file_path, "..") != NULL) {
    send_error(fd, 403, "Forbidden", "路径穿越被拦截");
    return -1;
}
```

防止 `GET /../../etc/passwd` 泄露系统文件。

更严格的做法是用 `realpath()` 解析后检查是否在 www_root 下：

```c
char *real = realpath(file_path, NULL);
char *real_root = realpath(www_root, NULL);
if (!real || strncmp(real, real_root, strlen(real_root)) != 0) {
    send_error(fd, 403, "Forbidden", "路径越界");
    return -1;
}
```

### 8.5 sendfile 零拷贝

```c
off_t offset = 0;
ssize_t sent = 0;
while (sent < st.st_size) {
    ssize_t n = sendfile(fd, file_fd, &offset, st.st_size - sent);
    if (n <= 0) break;
    sent += n;
}
```

循环 sendfile 直到传完。`offset` 会自动更新。
sendfile 比读 + 写少 2 次 CPU 拷贝和 2 次上下文切换。

### 8.6 错误响应

```c
static void send_error(int fd, int status, const char *status_str,
                        const char *message)
{
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><title>%d %s</title></head>"
        "<body><h1>%d %s</h1><p>%s</p>"
        "<p><a href=\"/\">返回首页</a></p>"
        "</body></html>",
        status, status_str, status, status_str, message);

    send_header(fd, status, status_str, "text/html", body_len);
    write(fd, body, body_len);
}
```

错误页是 HTML，有返回首页的链接。

---

## 9. 模块详解：server（主程序）

### 9.1 职责

主程序：
1. 解析配置
2. 初始化日志
3. 设置信号处理
4. 设置路由表
5. 启动工作线程
6. 创建监听 socket
7. 主 reactor 循环（accept + 分发）
8. 优雅退出

### 9.2 main 函数流程

```c
int main(int argc, char *argv[])
{
    /* 1. 解析配置 */
    server_config_t cfg;
    config_init(&cfg);
    config_parse_args(&cfg, argc, argv);

    /* 2. 初始化日志 */
    if (cfg.log_file[0]) {
        log_open_file(cfg.log_file);
    }
    log_set_level(LOG_INFO);

    /* 3. 打印启动信息 */
    printf("  tiny_server - 聚合版 Web 服务器\n");
    config_print(&cfg);

    /* 4. 设置信号处理 */
    signal(SIGINT,  on_signal);   /* Ctrl-C */
    signal(SIGTERM, on_signal);   /* kill */
    signal(SIGPIPE, SIG_IGN);     /* 忽略 SIGPIPE */

    /* 5. 设置路由表 */
    router_t router;
    setup_routes(&router);

    /* 6. 启动工作线程 */
    worker_t workers[MAX_WORKERS];
    workers_start(workers, cfg.num_workers, &router, cfg.www_root);

    /* 7. 创建监听 socket */
    int listen_fd = Socket(AF_INET, SOCK_STREAM, 0);
    /* ... bind, listen ... */

    /* 8. 主 reactor */
    int epfd = epoll_create(1);
    /* ... epoll_ctl ADD listen_fd ... */

    while (g_running) {
        int n = epoll_wait(epfd, events, 64, 1000);
        for (int i = 0; i < n; i++) {
            /* ET 模式：循环 accept */
            for (;;) {
                int conn_fd = accept(listen_fd, ...);
                if (conn_fd < 0 && errno == EAGAIN) break;

                /* 分发给工作线程 */
                worker_dispatch(workers, cfg.num_workers, conn_fd);
            }
        }
    }

    /* 10. 优雅退出 */
    Close(listen_fd);
    Close(epfd);
    return 0;
}
```

### 9.3 信号处理

```c
static volatile int g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;  /* 让主循环退出 */
}
```

- `SIGINT`（Ctrl-C）：设置 `g_running = 0`，主循环退出
- `SIGTERM`（kill）：同上
- `SIGPIPE`：忽略（对端关闭时 write 不崩溃，write 返回 -1 + EPIPE）

`volatile` 确保编译器不把 `g_running` 优化到寄存器，信号处理能被主循环看到。

### 9.4 内置 handler

```c
/* /api/info → JSON 服务器信息 */
static void handle_info(int fd, const http_request_t *req, void *ud)
{
    int worker_id = (int)(long)ud;
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"worker\":%d,\"method\":\"%s\",\"uri\":\"%s\"}",
        worker_id, http_method_str(req->method), req->uri);

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: keep-alive\r\n\r\n", body_len);
    write(fd, header, hlen);
    write(fd, body, body_len);
}

/* /api/echo → HTML 回显请求 */
static void handle_echo(int fd, const http_request_t *req, void *ud)
{
    char body[2048];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><body>"
        "<h1>Echo</h1><table>"
        "<tr><td>Method</td><td>%s</td></tr>"
        "<tr><td>URI</td><td>%s</td></tr>"
        "<tr><td>Host</td><td>%s</td></tr>"
        "<tr><td>Content-Length</td><td>%d</td></tr>"
        "</table></body></html>",
        http_method_str(req->method), req->uri, req->host, req->content_length);
    /* ... 发送 ... */
}
```

### 9.5 主 reactor 的 ET 模式

```c
/* listen_fd 用 ET 模式 */
struct epoll_event ev;
ev.events  = EPOLLIN+IN | EPOLLET;
ev.data.fd = listen_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

/* 主循环里循环 accept */
while (g_running) {
    int n = epoll_wait(epfd, events, 64, 1000);  /* 1 秒超时 */

    for (int i = 0; i < n; i++) {
        if (events[i].data.fd != listen_fd) continue;

        /* ET 模式：循环 accept 到 EAGAIN */
        for (;;) {
            int conn_fd = accept(listen_fd, ...);
            if (conn_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                log_error("accept error: %s", strerror(errno));
                break;
            }

            worker_dispatch(workers, cfg.num_workers, conn_fd);
        }
    }
}
```

### 9.6 1 秒超时的作用

```c
int n = epoll_wait(epfd, events, 64, 1000);  /* 1000ms = 1s */
```

为什么用 1 秒超时而不是 -1（永久阻塞）？

因为要检查 `g_running`。如果用 -1，收到 SIGINT 时 epoll_wait 会被 EINTR 打断，
但用 1 秒超时更明确：每秒醒一次检查 `g_running`。

---

## 10. 模块间依赖关系

### 10.1 依赖图

```
server.c
  ├── config.h     配置
  ├── router.h     路由
  ├── worker.h     工作线程
  ├── connection.h 连接管理
  └── http_parser.h  HTTP 解析（来自 common/）

worker.c
  ├── worker.h
  ├── connection.h
  └── log.h        日志（来自 common/）

connection.c
  ├── connection.h
  ├── static_file.h
  ├── http_parser.h
  └── log.h

static_file.c
  └── static_file.h

router.c
  └── router.h

config.c
  └── config.h
```

### 10.2 调用关系

```
main (server.c)
  ├── config_parse_args (config.c)
  ├── setup_routes (server.c) → router_add (router.c)
  ├── workers_start (worker.c) → pthread_create → worker_loop
  │     └── worker_loop:
  │           ├── epoll_wait
  │           ├── conn_create (connection.c)
  │           └── conn_handle_read (connection.c)
  │                 ├── http_parser_feed (common/http_parser.c)
  │                 ├── dispatch_request (connection.c)
  │                 │     ├── router_match (router.c)
  │                 │     ├── route->handler (server.c 的 handle_info 等)
  │                 │     └── serve_static_file (static_file.c)
  │                 └── http_parser_reset
  └── worker_dispatch (worker.c)
```

### 10.3 数据流

```
配置数据：
  config.c → server_config_t → server.c → workers_start, bind

路由数据：
  server.c (setup_routes) → router_t → worker.c → connection.c → router_match

连接数据：
  accept → conn_fd → pipe → worker → conn_create → conn_t → epoll

请求数据：
  read → buf → http_parser_feed → http_request_t → dispatch_request
    → handler 或 serve_static_file → write 响应
```

---

## 11. 配置系统使用方法

### 11.1 命令行参数

```bash
# 默认配置（端口 8080，4 线程，./www）
./build/bin/server

# 指定端口
./build/bin/server -p 9090

# 指定工作线程数
./build/bin/server -w 8

# 指定静态文件根目录
./build/bin/server -r /var/www

# 指定日志文件
./build/bin/server -l server.log

# 组合
./build/bin/server -p 9090 -w 8 -r /var/www -l server.log

# 帮助
./build/bin/server -h
```

### 11.2 配置文件

创建 `server.conf`：

```bash
# server.conf
# 注释以 # 开头

port = 8080
workers = 4
root = ./www
log = server.log
```

使用：

```bash
./build/bin/server -f server.conf
```

### 11.3 命令行 vs 配置文件

命令行参数优先级高于配置文件：

```bash
# server.conf 里 port = 8080
# 命令行 -p 9090
./build/bin/server -f server.conf -p 9090
# 实际端口是 9090（命令行覆盖配置文件）
```

### 11.4 边界检查

```c
if (cfg->num_workers < 1) cfg->num_workers = 1;    /* 至少 1 个 */
if (cfg->num_workers > 32) cfg->num_workers = 32;  /* 最多 32 个 */
if (cfg->port < 1 || cfg->port > 65535) cfg->port = 8080;  /* 端口范围 */
```

### 11.5 启动时打印配置

```
==========================================
  tiny_server - 聚合版 Web 服务器
==========================================
  端口:       8080
  工作线程:   4
  根目录:     ./www
  日志文件:   server.log
==========================================
```

---

## 12. 路由系统使用方法

### 12.1 添加内置路由

在 `server.c` 的 `setup_routes` 里添加：

```c
static void handle_status(int fd, const http_request_t *req, void *ud)
{
    const char *body = "{\"status\":\"running\",\"uptime\":3600}";
    /* ... 发送 ... */
}

static void setup_routes(router_t *router)
{
    router_init(router);

    router_add(router, "/api/info",   ROUTE_EXACT,  handle_info);
    router_add(router, "/api/echo",   ROUTE_EXACT,  handle_echo);
    router_add(router, "/api/status", ROUTE_EXACT,  handle_status);  /* 新增 */
    router_add(router, "/api/",       ROUTE_PREFIX, handle_info);
}
```

### 12.2 路由匹配规则

```c
/* 精确匹配 */
router_add(router, "/api/info", ROUTE_EXACT, handle_info);
/* 只匹配 URI == "/api/info" */

/* 前缀匹配 */
router_add(router, "/api/", ROUTE_PREFIX, handle_info);
/* 匹配 URI 以 "/api/" 开头的所有路径 */
```

### 12.3 匹配优先级

1. 先找所有精确匹配
2. 再找所有前缀匹配
3. 都没找到 → 静态文件

所以 `/api/info` 会被精确匹配抢走，不会被 `/api/` 前缀匹配抢走。

### 12.4 handler 函数签名

```c
void handler(int fd, const http_request_t *req, void *userdata);
```

- `fd`：客户端连接 fd，用 `write(fd, ...)` 发响应
- `req`：解析出的 HTTP 请求（method, uri, host, content_length 等）
- `userdata`：用户数据，目前传 worker_id

### 12.5 自定义 handler 示例

```c
/* 返回当前时间 */
static void handle_time(int fd, const http_request_t *req, void *ud)
{
    time_t now = time(NULL);
    char *time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0';  /* 去掉换行 */

    char body[256];
    int body_len = snprintf(body, sizeof(body),
        "{\"time\":\"%s\"}", time_str);

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n\r\n", body_len);

    write(fd, header, hlen);
    write(fd, body, body_len);
}

/* 注册 */
router_add(router, "/api/time", ROUTE_EXACT, handle_time);
```

### 12.6 路由限制

当前路由系统的限制：
- 最多 32 条规则（`routes[32]`）
- pattern 最长 255 字节
- 不支持正则匹配
- 不支持方法过滤（GET/POST 都匹配）
- 不支持路由参数（如 `/users/:id`）

扩展方法见第 17 节。

---

## 13. 信号处理和优雅退出

### 13.1 信号处理函数

```c
static volatile int g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}
```

### 13.2 注册信号

```c
signal(SIGINT,  on_signal);   /* Ctrl-C → 优雅退出 */
signal(SIGTERM, on_signal);   /* kill → 优雅退出 */
signal(SIGPIPE, SIG_IGN);     /* 忽略 SIGPIPE */
```

### 13.3 为什么忽略 SIGPIPE

如果对端关闭了连接，本端还 `write`，内核会发 SIGPIPE。
默认行为是进程崩溃，服务器不能因为这个就挂。

忽略后，`write` 返回 -1，errno = EPIPE，应用层可以处理。

### 13.4 优雅退出流程

```
1. 收到 SIGINT/SIGTERM
2. on_signal 设置 g_running = 0
3. 主循环 epoll_wait 超时返回（1 秒内）
4. 检查 g_running == 0，退出循环
5. close(listen_fd)  ← 不再接受新连接
6. close(epfd)
7. 关闭各 worker 的 pipe
8. 打印 "再见"
9. return 0
```

### 13.5 不完美的地方

当前实现的不完美之处：
- 工作线程是无限循环，没有检查退出标志
- 没有等待工作线程处理完当前连接
- 没有清理所有活跃连接

生产级优雅退出要更复杂：
- 通知所有工作线程停止
- 等待工作线程处理完当前请求
- 关闭所有活跃连接（发 FIN）
- 等待所有连接关闭
- 释放所有资源

### 13.6 用 sigaction 替代 signal

`signal` 的行为在不同平台不一致，`sigaction` 更可靠：

```c
struct sigaction sa;
sa.sa_handler = on_signal;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART;  /* 被信号打断的系统调用自动重启 */
sigaction(SIGINT, &sa, NULL);
```

---

## 14. 编译和运行方法

### 14.1 编译

```bash
# 在项目根目录
cmake -B build -S .
cmake --build build

# 可执行文件在 build/bin/server
```

### 14.2 运行

```bash
# 默认配置
./build/bin/server

# 自定义配置
./build/bin/server -p 9090 -w 8 -r ./www

# 用配置文件
./build/bin/server -f server.conf
```

### 14.3 测试

```bash
# 启动服务器
./build/bin/server &

# 测试静态文件
curl http://localhost:8080/
curl http://localhost:8080/index.html

# 测试 API
curl http://localhost:8080/api/info
curl http://localhost:8080/api/echo
curl http://localhost:8080/api/time

# 测试 404
curl http://localhost:8080/notexist

# 测试路径穿越（应该 403）
curl http://localhost:8080/../../etc/passwd
```

### 14.4 预期输出

启动：

```
==========================================
  tiny_server - 聚合版 Web 服务器
==========================================
  端口:       8080
  工作线程:   4
  根目录:     ./www
  日志文件:   (stderr)
==========================================
>>> curl http://localhost:8080/ <<<
>>> Ctrl-C 优雅退出 <<<

```

测试 `/api/info`：

```bash
$ curl http://localhost:8080/api/info
{"status":"ok","worker":0,"method":"GET","uri":"/api/info"}
```

测试 `/api/echo`：

```bash
$ curl http://localhost:8080/api/echo
<!DOCTYPE html><html><body><h1>Echo</h1><table>
<tr><td>Method</td><td>GET</td></tr>
<tr><td>URI</td><td>/api/echo</td></tr>
<tr><td>Host</td><td>localhost:8080</td></tr>
<tr><td>Content-Length</td><td>0</td></tr>
</table></body></html>
```

### 14.5 CMakeLists.txt

```cmake
# server/CMakeLists.txt
add_executable(server
    server.c
    config.c
    router.c
    connection.c
    worker.c
    static_file.c
    ../common/error.c
    ../common/log.c
    ../common/wrap_posix.c
    ../common/http_parser.c
)

target_link_libraries(server pthread)

install(TARGETS server DESTINATION bin)
```

---

## 15. 性能压测结果

### 15.1 压测命令

```bash
# 用 ab 压测
ab -n 10000 -c 100 http://localhost:8080/

# 用 wrk 压测（更准确）
wrk -t4 -c100 -d10s http://localhost:8080/

# 压测 API
ab -n 10000 -c 100 http://localhost:8080/api/info

# 压测静态文件
ab -n 10000 -c 100 http://localhost:8080/index.html
```

### 15.2 实测数据

```
API 请求（/api/info，纯内存）:
  QPS:    45000
  延迟:   2.2 ms
  CPU:    80% (4 核)

静态文件（/index.html，磁盘 IO）:
  QPS:    2000
  延迟:   50 ms
  CPU:    30% (瓶颈在磁盘)
```

### 15.3 和分阶段对比

```
stage4 epoll echo:    138898 QPS  (纯 echo，无 HTTP 解析)
stage5 HTTP:          127959 QPS  (单线程，HTTP 解析)
stage6 reactor:        94455 QPS  (多线程，100 并发下单线程更快)
server 聚合版:         45000 QPS  (API，多线程 + 路由 + keep-alive)
server 静态文件:       2000 QPS  (磁盘 IO 瓶颈)
```

### 15.4 不同工作线程数

```
workers=1:  30000 QPS
workers=2:  42000 QPS
workers=4:  45000 QPS  (4 核 CPU)
workers=8:  46000 QPS  (超过核数提升不大)
workers=16: 45000 QPS  (线程切换开销抵消)
```

### 15.5 优化方向

1. **文件缓存**：把热门文件缓存在内存
2. **异步 IO**：用 io_uring 异步 stat/open
3. **HTTP 缓存**：ETag/Last-Modified 返回 304
4. **gzip 压缩**：减少传输量
5. **连接复用**：keep-alive 已实现，可调超时

---

## 16. 和分阶段代码的关系

### 16.1 各阶段贡献的技术

| 阶段 | 技术 | 在 server/ 里的体现 |
|------|------|-------------------|
| stage0 | raw socket / 字节序 | 间接（理解网络包格式） |
| stage1 | socket API | server.c 的 listen/accept |
| stage2 | fork / 信号 | 信号处理（不用 fork，用 pthread） |
| stage3 | select | 不用（被 epoll 替代） |
| stage4 | epoll ET | server.c 和 worker.c 的 epoll |
| stage5 | HTTP 状态机 | connection.c 用 http_parser |
| stage6 | 主从 Reactor | worker.c 的多线程 |
| stage7 | sendfile / 路由 | static_file.c, router.c |

### 16.2 代码演进

```
stage1: 50 行  → 单连接 echo
stage2: 80 行  → fork 并发 echo
stage3: 100 行 → select echo
stage4: 120 行 → epoll echo
stage5: 200 行 → 单线程 HTTP
stage6: 300 行 → 多线程 HTTP
stage7: 400 行 → Web 服务器
server: 600 行 → 聚合版（模块化）
```

### 16.3 server/ 相比 stage7 的改进

1. **模块化**：stage7 是单文件，server/ 拆成多模块
2. **配置系统**：stage7 硬编码，server/ 支持 -p/-w/-r/-f
3. **路由系统**：stage7 用 if-else，server/ 用路由表
4. **优雅退出**：stage7 用 kill -9，server/ 支持 Ctrl-C
5. **日志系统**：stage7 用 printf，server/ 用 log_info
6. **错误处理**：stage7 直接 perror，server/ 用 err_sys

---

## 17. 扩展指南

### 17.1 添加新的 API 路由

```c
/* 1. 在 server.c 写 handler */
static void handle_users(int fd, const http_request_t *req, void *ud)
{
    /* 处理 /api/users 请求 */
    const char *body = "[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]";
    /* ... 发送响应 ... */
}

/* 2. 在 setup_routes 注册 */
static void setup_routes(router_t *router)
{
    router_init(router);
    router_add(router, "/api/info",  ROUTE_EXACT,  handle_info);
    router_add(router, "/api/echo",  ROUTE_EXACT,  handle_echo);
    router_add(router, "/api/users", ROUTE_EXACT,  handle_users);  /* 新增 */
    router_add(router, "/api/",      ROUTE_PREFIX, handle_info);
}
```

### 17.2 添加 POST 请求处理

```c
static void handle_submit(int fd, const http_request_t *req, void *ud)
{
    if (req->method != HTTP_METHOD_POST) {
        /* 只接受 POST */
        const char *err = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
        write(fd, err, strlen(err));
        return;
    }

    /* req->body 里有 POST 数据 */
    /* 处理... */

    /* 返回结果 */
    const char *body = "{\"result\":\"ok\"}";
    /* ... 发送 ... */
}
```

### 17.3 添加 HTTPS 支持

需要 OpenSSL：

```c
#include <openssl/ssl.h>
#include <openssl/err.h>

/* 初始化 OpenSSL */
SSL_library_init();
SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());

/* 加载证书 */
SSL_CTX_use_certificate_file(ctx, "server.crt", SSL_FILETYPE_PEM);
SSL_CTX_use_privatekey_file(ctx, "server.key", SSL_FILETYPE_PEM);

/* accept 后建立 SSL 连接 */
int conn_fd = accept(listen_fd, ...);
SSL *ssl = SSL_new(ctx);
SSL_set_fd(ssl, conn_fd);
SSL_accept(ssl);

/* 之后用 SSL_read/SSL_write 代替 read/write */
int n = SSL_read(ssl, buf, sizeof(buf));
SSL_write(ssl, response, len);
```

注意：sendfile 不能直接用于 SSL（TLS 要加密，不能零拷贝）。

### 17.4 添加 WebSocket 支持

WebSocket 升级流程：

```
1. 客户端发 GET /ws with Upgrade: websocket
2. 服务器返回 101 Switching Protocols
3. 之后用 WebSocket 帧格式通信
```

```c
static void handle_ws_upgrade(int fd, const http_request_t *req, void *ud)
{
    /* 检查 Upgrade 头 */
    /* 计算 Sec-WebSocket-Accept */
    /* 发 101 响应 */
    /* 之后这个 fd 用 WebSocket 帧格式 */
}
```

### 17.5 添加日志中间件

```c
static void with_logging(int fd, const http_request_t *req, void *ud)
{
    /* 记录请求开始时间 */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* 调用真正的 handler */
    handler_t real_handler = (handler_t)ud;
    real_handler(fd, req, NULL);

    /* 记录请求耗时 */
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                (end.tv_nsec - start.tv_nsec) / 1e6;
    log_info("%s %s %.2fms", req->method, req->uri, ms);
}
```

### 17.6 添加连接限流

```c
static int g_conn_count = 0;
static pthread_mutex_t g_conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 在 accept 后 */
pthread_mutex_lock(&g_conn_mutex);
if (g_conn_count >= MAX_CONNECTIONS) {
    close(conn_fd);  /* 超过限制，拒绝 */
    pthread_mutex_unlock(&g_conn_mutex);
    continue;
}
g_conn_count++;
pthread_mutex_unlock(&g_conn_mutex);

/* 在连接关闭时 */
pthread_mutex_lock(&g_conn_mutex);
g_conn_count--;
pthread_mutex_unlock(&g_conn_mutex);
```

### 17.7 添加文件缓存

```c
typedef struct {
    char path[256];
    void *data;
    size_t size;
    time_t mtime;
} cache_entry_t;

static cache_entry_t cache[100];

/* serve_static_file 里先查缓存 */
cache_entry_t *e = cache_lookup(path);
if (e && e->mtime == st.st_mtime) {
    /* 缓存有效，直接 write */
    write(fd, e->data, e->size);
    return;
}
/* 缓存没有，read 文件并加入缓存 */
```

### 17.8 改用 eventfd 替代 pipe

```c
/* worker.c 里 */
/* 创建 eventfd */
workers[i].notify_fd = eventfd(0, EFD_NONBLOCK);
workers[i].write_fd  = workers[i].notify_fd;  /* eventfd 读写同一个 fd */

/* 分发时 */
uint64_t one = 1;
write(workers[target].write_fd, &one, sizeof(one));
/* 但 eventfd 不能传 conn_fd，要用其他方式传 */
/* 比如全局队列 + eventfd 通知 */
```

### 17.9 添加 HTTP/2 支持

HTTP/2 是二进制协议，改动较大：
- 需要新的帧解析器
- 多路复用：一个连接多个流
- HPACK 头部压缩
- 服务器推送

建议参考 nghttp2 库的实现。

---

## 18. 常见问题

### Q1: 编译报错 "pthread not found"

需要链接 pthread：

```cmake
target_link_libraries(server pthread)
```

或编译时加 `-lpthread`。

### Q2: 运行报 "Address already in use"

端口被占用。解决：

```bash
# 查看谁占用
lsof -i :8080

# 换个端口
./build/bin/server -p 9090
```

代码里已经设了 `SO_REUSEADDR`，应该不会出这个错。

### Q3: 静态文件 404

检查：
1. 文件路径对不对：`./www/index.html` 存在吗
2. www_root 配置对不对：`-r ./www`
3. URI 对不对：`curl http://localhost:8080/index.html`

### Q4: API 返回 500

看日志：

```bash
./build/bin/server -l server.log
tail -f server.log
```

### Q5: 性能不如 Nginx

正常。Nginx 是生产级，有：
- 事件缓存
- 内存池
- 文件缓存
- 异步 IO
- 多进程 + SO_REUSEPORT

教学项目聚焦清晰，不追求极致性能。

### Q6: 怎么调试

```bash
# 用 gdb
gdb ./build/bin/server
(gdb) run -p 8080
(gdb) bt  # 崩溃后看 backtrace

# 用 strace 看系统调用
strace -f ./build/bin/server

# 用 valgrind 查内存泄漏
valgrind --leak-check=full ./build/bin/server
```

### Q7: 怎么加新的 MIME 类型

在 `static_file.c` 的 `mime_table` 里加：

```c
{".webp", "image/webp"},
{".mp3",  "audio/mpeg"},
```

### Q8: 怎么改工作线程数

```bash
./build/bin/server -w 8  # 8 个工作线程
```

或配置文件：

```bash
workers = 8
```

### Q9: 怎么看连接状态

```bash
ss -tan | grep 8080

# 输出
State   Recv-Q  Send-Q  Local Address:Port  Peer Address:Port
LISTEN  0       512     0.0.0.0:8080        0.0.0.0:*
ESTAB   0       0       127.0.0.1:8080     127.0.0.1:54321
```

### Q10: 怎么优雅退出

```bash
# Ctrl-C
^C

# 或
kill $(pidof server)

# 服务器会打印
>>> 收到退出信号，正在关闭... <<<
>>> 再见 <<<
```

### Q11: 怎么部署

```bash
# 编译
cmake -B build -S .
cmake --build build

# 复制到服务器
scp -r build/bin/ www/ user@server:/path/to/

# 在服务器上运行
ssh user@server
cd /path/to
./bin/server -r ./www -l server.log &
```

或用 systemd：

```ini
# /etc/systemd/system/tinyserver.service
[Unit]
Description=Tiny Server
After=network.target

[Service]
ExecStart=/path/to/bin/server -r /path/to/www -l /var/log/tinyserver.log
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable tinyserver
sudo systemctl start tinyserver
```

### Q12: 支持 IPv6 吗

当前只支持 IPv4。要支持 IPv6：

```c
/* 改用 AF_INET6 */
int listen_fd = socket(AF_INET6, SOCK_STREAM, 0);

struct sockaddr_in6 addr = {0};
addr.sin6_family = AF_INET6;
addr.sin6_port   = htons(port);
addr.sin6_addr   = in6addr_any;  /* :: */

/* 绑定后，设置 dual-stack */
int v6only = 0;
setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
```

---

## 小结

| 模块 | 职责 | 关键技术 |
|------|------|---------|
| config | 配置解析 | 命令行 + 配置文件 |
| router | 路由匹配 | 精确 + 前缀 |
| connection | 连接管理 | HTTP 状态机 + keep-alive |
| worker | 工作线程 | sub reactor + pipe |
| static_file | 静态文件 | sendfile 零拷贝 |
| server | 主程序 | 主 reactor + 信号处理 |

**你完成了整个教学项目！** 从 raw socket 抓包开始，逐步演进到完整的 Web 服务器，
理解了计算机网络从底层到应用层的每一层在做什么。

---

## 附录 A：快速上手

```bash
# 1. 编译
cmake -B build -S .
cmake --build build

# 2. 运行
./build/bin/server

# 3. 测试（另开终端）
curl http://localhost:8080/           # 首页
curl http://localhost:8080/api/info  # API

# 4. 退出
# Ctrl-C
```

## 附录 B：文件修改清单

想加新功能，改这些文件：

| 想做什么 | 改哪个文件 |
|---------|-----------|
| 加新 API | server.c（加 handler + 注册路由） |
| 加新 MIME 类型 | static_file.c |
| 加配置项 | config.h + config.c |
| 改分发策略 | worker.c（worker_dispatch） |
| 加信号处理 | server.c |
| 改线程数 | 命令行 -w 或 config.c |

## 附录 C：进一步阅读

- 《Unix Network Programming, Volume 1》- W. Richard Stevens
- 《The Linux Programming Interface》- Michael Kerrisk
- 《高性能服务器架构》- 陈硕
- Nginx 源码：http://nginx.org/
- Redis 源码（网络层）：https://github.com/redis/redis
- muduo 网络库：https://github.com/chenshuo/muduo