# tiny_server - 用纯 C 从零理解计算机网络底层原理

教学项目。在 WSL2 + Linux 下用纯 C 手写一个 Web 服务器，从 raw socket 抓包开始，逐步演进到 epoll + Reactor + 静态文件服务。

项目分两个阶段：

- **phase1（基础）**：IO 模型演进 + HTTP 解析 + Reactor + 完整 Web 服务器
- **phase2（进阶）**：TLS + io_uring + 内存池 + WebSocket + 协程 + HTTP/2

## 目录结构

```
tiny_server/
├── phase1/                # 第一阶段：基础
│   ├── stage0-7/          # 8 个教学阶段
│   ├── server/            # 聚合版服务器
│   ├── common/            # 公共模块
│   ├── docs/              # 教学文档
│   └── lab/               # 实验脚本
├── phase2/                # 第二阶段：进阶
│   ├── stage8-14/         # 7 个进阶阶段（逐步实现）
│   ├── server/            # 聚合版进阶服务器
│   ├── common/            # 公共模块（io_ops 抽象层）
│   ├── docs/              # 教学文档
│   └── lab/               # 实验脚本
├── mkdocs.yml             # 文档站配置
└── CMakeLists.txt         # 顶层构建
```

## 第一阶段：演进路线

| 阶段 | 目录 | 主题 | 可执行文件 |
|------|------|------|-----------|
| 0 | stage0_raw_sniff | raw socket 抓包 | raw_sniff |
| 1 | stage1_echo_blocking | 阻塞 echo server | echo_server |
| 2 | stage2_echo_fork | 多进程并发 | echo_server_fork |
| 3 | stage3_echo_select | select 多路复用 | echo_server_select |
| 4 | stage4_echo_epoll | epoll LT/ET | echo_server_epoll_lt / _et |
| 5 | stage5_http_parser | HTTP 状态机解析 | http_server |
| 6 | stage6_reactor | 主从 Reactor | reactor_server |
| 7 | stage7_webserver | 完整 Web 服务器 | webserver |

## 第二阶段：进阶路线

| 阶段 | 目录 | 主题 |
|------|------|------|
| 8 | stage8_tls | TLS/HTTPS |
| 9 | stage9_io_uring | io_uring 异步 IO |
| 10 | stage10_mempool_log | 内存池 + 异步日志 |
| 11 | stage11_reuseport | SO_REUSEPORT 多核 |
| 12 | stage12_websocket | WebSocket |
| 13 | stage13_coroutine | 协程 |
| 14 | stage14_http2 | HTTP/2 |

## 快速开始

```bash
# 1. 进入 WSL2
wsl -d Ubuntu

# 2. 安装环境（首次）
bash phase1/scripts/setup_env.sh

# 3. 编译
cd /mnt/c/Users/wlh19/Desktop/webserve
cmake -B build -S .
cmake --build build

# 4. 运行各阶段
sudo build/bin/raw_sniff 8080        # stage0: 抓包（需要 root）
build/bin/echo_server 8080           # stage1: 阻塞 echo
build/bin/echo_server_fork 8080      # stage2: 多进程 echo
build/bin/echo_server_select 8080    # stage3: select echo
build/bin/echo_server_epoll_lt 8080  # stage4: epoll LT
build/bin/echo_server_epoll_et 8080  # stage4: epoll ET
build/bin/http_server 8080           # stage5: HTTP server
build/bin/reactor_server 8080 4      # stage6: Reactor（4 线程）
build/bin/webserver 8080 4 ./phase1/www  # stage7: Web 服务器
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

## 文档

在线文档：https://wanglh39.github.io/tiny_server/

| 文档 | 内容 |
|------|------|
| phase1/docs/00_byte_order.md | 网络字节序与结构体对齐 |
| phase1/docs/01_raw_sniff.md | stage0：IP/TCP 头部、三次握手 |
| phase1/docs/02_echo_evolution.md | stage1-4：IO 模型演进 |
| phase1/docs/03_http_and_webserver.md | stage5-7：HTTP 解析与 Web 服务器 |
| phase1/docs/04_server.md | 聚合版服务器架构 |
