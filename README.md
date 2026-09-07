# tiny_server - 用纯 C 从零理解计算机网络底层原理

教学项目。在 WSL2 + Linux 下用纯 C 手写一个 Web 服务器，从 raw socket 抓包开始，逐步演进到 epoll + Reactor + 静态文件服务。

## 演进路线

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

## 快速开始

```bash
# 1. 进入 WSL2
wsl -d Ubuntu

# 2. 安装环境（首次）
bash scripts/setup_env.sh

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
build/bin/webserver 8080 4 ./www     # stage7: Web 服务器（4 线程）
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

运行压测：`bash lab/benchmark.sh`

## 文档

| 文档 | 内容 |
|------|------|
| docs/00_byte_order.md | 网络字节序与结构体对齐 |
| docs/01_raw_sniff.md | stage0：IP/TCP 头部、三次握手 |
| docs/02_echo_evolution.md | stage1-4：IO 模型演进 |
| docs/03_http_and_webserver.md | stage5-7：HTTP 解析与 Web 服务器 |

## 实验脚本

```bash
bash lab/test_echo_servers.sh      # 测试 echo server 功能
bash lab/test_http_servers.sh      # 测试 HTTP server 功能
bash lab/benchmark.sh              # 压测对比
sudo bash lab/tcpdump_handshake.sh # tcpdump 抓三次握手
bash lab/observe_timewait.sh       # 观察 TIME_WAIT
```
