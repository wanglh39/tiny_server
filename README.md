# tiny_server - 用纯 C 从零理解计算机网络底层原理

教学项目。在 WSL2 + Linux 下用纯 C 手写一个 Web 服务器，从 raw socket 抓包开始，逐步演进到 epoll + Reactor + HTTP/2 + 协程。

项目分两个阶段，共 15 个 stage + 2 个聚合版服务器，每个 stage 有独立文档（1000+ 行）：

- **phase1（基础）**：IO 模型演进 + HTTP 解析 + Reactor + 完整 Web 服务器（stage0-7）
- **phase2（进阶）**：TLS + io_uring + 内存池 + WebSocket + 协程 + HTTP/2（stage8-14）
- **聚合版**：phase1/server/ 和 phase2/server/，整合各阶段全部特性

## 目录结构

```
tiny_server/
├── phase1/                    # 第一阶段：基础
│   ├── stage0_raw_sniff/      # raw socket 抓包
│   ├── stage1_echo_blocking/  # 阻塞 echo
│   ├── stage2_echo_fork/      # 多进程 fork
│   ├── stage3_echo_select/    # select 多路复用
│   ├── stage4_echo_epoll/     # epoll LT/ET
│   ├── stage5_http_parser/    # HTTP 状态机解析
│   ├── stage6_reactor/        # 主从 Reactor
│   ├── stage7_webserver/      # 完整 Web 服务器
│   ├── server/                # 聚合版服务器
│   ├── common/                # 公共模块（error/log/wrap_posix）
│   ├── docs/                  # 教学文档（每 stage 独立，1000+ 行）
│   ├── lab/                   # 压测脚本
│   └── www/                   # 静态文件
├── phase2/                    # 第二阶段：进阶
│   ├── stage8_tls/            # TLS/HTTPS
│   ├── stage9_io_uring/       # io_uring 异步 IO
│   ├── stage10_mempool_log/   # 内存池 + 异步日志
│   ├── stage11_reuseport/     # SO_REUSEPORT 多核
│   ├── stage12_websocket/     # WebSocket
│   ├── stage13_coroutine/     # 协程（ucontext）
│   ├── stage14_http2/         # HTTP/2
│   ├── server/                # 聚合版高级服务器（7 特性整合）
│   ├── common/                # 公共模块（复用 phase1）
│   └── docs/                  # 教学文档（每 stage 独立，1000+ 行）
├── mkdocs.yml                 # 文档站配置
└── CMakeLists.txt             # 顶层构建
```

## 第一阶段：基础（stage0-7）

| 阶段 | 目录 | 主题 | 可执行文件 | 文档 |
|------|------|------|-----------|------|
| 0 | stage0_raw_sniff | raw socket 抓包 | raw_sniff | 01_raw_sniff.md |
| 1 | stage1_echo_blocking | 阻塞 echo server | echo_server | 02_echo_blocking.md |
| 2 | stage2_echo_fork | 多进程并发 | echo_server_fork | 03_echo_fork.md |
| 3 | stage3_echo_select | select 多路复用 | echo_server_select | 04_echo_select.md |
| 4 | stage4_echo_epoll | epoll LT/ET | echo_server_epoll_lt/_et | 05_echo_epoll.md |
| 5 | stage5_http_parser | HTTP 状态机解析 | http_server | 06_http_parser.md |
| 6 | stage6_reactor | 主从 Reactor | reactor_server | 07_reactor.md |
| 7 | stage7_webserver | 完整 Web 服务器 | webserver | 08_webserver.md |

## 第二阶段：进阶（stage8-14）

| 阶段 | 目录 | 主题 | 可执行文件 | 文档 |
|------|------|------|-----------|------|
| 8 | stage8_tls | TLS/HTTPS | tls_server | 10_tls_https.md |
| 9 | stage9_io_uring | io_uring 异步 IO | uring_echo_server | 11_io_uring.md |
| 10 | stage10_mempool_log | 内存池 + 异步日志 | mempool_log_demo | 12_mempool_log.md |
| 11 | stage11_reuseport | SO_REUSEPORT 多核 | reuseport_server | 13_reuseport.md |
| 12 | stage12_websocket | WebSocket 协议 | ws_server | 14_websocket.md |
| 13 | stage13_coroutine | 协程（ucontext） | co_demo / co_echo_server | 15_coroutine.md |
| 14 | stage14_http2 | HTTP/2 协议 | http2_server | 16_http2.md |

### 聚合版服务器

| 服务器 | 目录 | 整合特性 | 可执行文件 | 文档 |
|--------|------|---------|-----------|------|
| phase1 聚合版 | phase1/server | epoll + Reactor + HTTP + sendfile | server | 09_server.md |
| phase2 聚合版 | phase2/server | TLS + io_uring + 内存池 + 异步日志 + SO_REUSEPORT + WebSocket + HTTP/2 | advanced_server | 17_advanced_server.md |

## 快速开始

```bash
# 1. 进入 WSL2
wsl -d Ubuntu

# 2. 编译全部
cd /mnt/c/Users/wlh19/Desktop/webserve
cmake -B build -S .
cmake --build build

# 3. 运行 phase1
sudo build/bin/raw_sniff 8080             # stage0: 抓包（需要 root）
build/bin/echo_server 8080                # stage1: 阻塞 echo
build/bin/echo_server_fork 8080           # stage2: 多进程 echo
build/bin/echo_server_select 8080         # stage3: select echo
build/bin/echo_server_epoll_lt 8080       # stage4: epoll LT
build/bin/echo_server_epoll_et 8080       # stage4: epoll ET
build/bin/http_server 8080                # stage5: HTTP server
build/bin/reactor_server 8080 4           # stage6: Reactor（4 线程）
build/bin/webserver 8080 4 ./phase1/www   # stage7: Web 服务器

# 4. 运行 phase2
build/bin/tls_server 8443                 # stage8: TLS/HTTPS
build/bin/uring_echo_server 8080          # stage9: io_uring echo
build/bin/mempool_log_demo                # stage10: 内存池+日志演示
build/bin/reuseport_server 8080 4         # stage11: SO_REUSEPORT（4 线程）
build/bin/ws_server 8080                  # stage12: WebSocket
build/bin/co_demo                         # stage13: 协程演示
build/bin/co_echo_server 8080             # stage13: 协程 echo 服务器
build/bin/http2_server 8080               # stage14: HTTP/2 服务器

# 5. 运行聚合版高级服务器
build/bin/advanced_server -p 8443 -w 4 -r phase2/www \
    --cert phase2/certs/cert.pem --key phase2/certs/key.pem
# 支持 HTTP/1.1 + HTTPS + HTTP/2 + WebSocket，一个端口四种协议
```

## 测试命令速查

```bash
# echo 服务器
echo "hello" | nc localhost 8080

# HTTP 服务器
curl http://localhost:8080/

# TLS/HTTPS
curl -k https://localhost:8443/

# WebSocket（用 wscat 或浏览器）
# wscat -c ws://localhost:8080/

# HTTP/2
curl --http2-prior-knowledge http://localhost:8080/
```

## 压测结果（100 并发，2000 请求）

```
stage1_blocking       压测失败（只能一个连接）
stage2_fork           QPS:  65744    延迟: 1.52 ms
stage3_select         QPS: 120576    延迟: 0.83 ms
stage4_epoll_lt       QPS: 138898    延迟: 0.72 ms
stage4_epoll_et       QPS: 139899    延迟: 0.71 ms
stage5_http           QPS: 127959    延迟: 0.78 ms
stage6_reactor(4t)    QPS:  94455    延迟: 1.06 ms
stage7_webserver      QPS:   2003    延迟: 49.92 ms
```

运行压测：`bash phase1/lab/benchmark.sh`

## 在线文档

**https://wanglh39.github.io/tiny_server/**

每个 stage 都有独立的教学文档（1000+ 行），从原理到代码逐行讲解：

### phase1 文档

| 文档 | 行数 | 内容 |
|------|------|------|
| 00_byte_order.md | 2615 | 网络字节序与结构体对齐 |
| 01_raw_sniff.md | 1644 | stage0：IP/TCP 头部、三次握手抓包 |
| 02_echo_blocking.md | 1106 | stage1：阻塞 IO、socket API、TCP 握手挥手 |
| 03_echo_fork.md | 1083 | stage2：fork 内核实现、COW、僵尸进程、信号 |
| 04_echo_select.md | 1025 | stage3：select 内核实现、fd_set、poll 对比 |
| 05_echo_epoll.md | 1267 | stage4：epoll 内核数据结构、LT vs ET、调优 |
| 06_http_parser.md | 1337 | stage5：HTTP 协议、状态机解析、分包粘包 |
| 07_reactor.md | 1514 | stage6：Reactor 模式、线程通信、线程池、惊群 |
| 08_webserver.md | 1791 | stage7：sendfile 零拷贝、mmap、路由、安全 |
| 09_server.md | 2050 | 聚合版服务器架构详解 |

### phase2 文档

| 文档 | 行数 | 内容 |
|------|------|------|
| 10_tls_https.md | 1235 | stage8：TLS 1.3 握手、OpenSSL、自签证书 |
| 11_io_uring.md | 1105 | stage9：io_uring SQ/CQ、异步 IO、零系统调用 |
| 12_mempool_log.md | 1021 | stage10：Nginx 内存池、muduo 双缓冲异步日志 |
| 13_reuseport.md | 1008 | stage11：SO_REUSEPORT、内核负载均衡、无惊群 |
| 14_websocket.md | 2042 | stage12：WebSocket 握手、帧解析、掩码、Ping/Pong |
| 15_coroutine.md | 1335 | stage13：ucontext 协程、调度器、同步写异步 |
| 16_http2.md | 1064 | stage14：HTTP/2 二进制帧、多路复用、HPACK |

## 技术栈

- **语言**：纯 C（C99），不用 C++ 封装
- **构建**：CMake
- **平台**：WSL2 + Ubuntu（Linux 6.18 内核）
- **依赖**：OpenSSL 3.0（TLS）、liburing 2.5（io_uring）
- **文档**：MkDocs Material，自动部署到 GitHub Pages

## 学习路线

```
phase1（基础）                          phase2（进阶）
┌──────────────────────┐               ┌──────────────────────┐
│ stage0  raw socket    │               │ stage8  TLS/HTTPS     │
│ stage1  阻塞 IO       │               │ stage9  io_uring      │
│ stage2  多进程 fork   │─── 基础完成 ─→│ stage10 内存池+日志   │
│ stage3  select        │               │ stage11 SO_REUSEPORT  │
│ stage4  epoll         │               │ stage12 WebSocket     │
│ stage5  HTTP 解析     │               │ stage13 协程          │
│ stage6  Reactor       │               │ stage14 HTTP/2        │
│ stage7  Web 服务器    │               └──────────────────────┘
└──────────────────────┘
```

建议按顺序学习，每个 stage 都建立在前一个的基础上。
