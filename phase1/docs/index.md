# tiny_server 教学文档

> 用纯 C 从零理解计算机网络底层原理 —— 从 raw socket 抓包到完整 Web 服务器

## 这是什么？

一个在 WSL2 + Linux 下用纯 C 手写的 Web 服务器教学项目。不封装、不抽象，
让你看清每一个 POSIX 系统调用的本来面目。

## 演进路线

| 阶段 | 主题 | 核心知识点 |
|------|------|-----------|
| 0 | raw socket 抓包 | IP/TCP 头部、网络字节序、三次握手 |
| 1 | 阻塞 echo server | socket API、fd、listen backlog |
| 2 | 多进程并发 | fork、fd 引用计数、TIME_WAIT、SIGCHLD |
| 3 | select 多路复用 | FD_SETSIZE、O(n) 遍历 |
| 4 | epoll (LT/ET) | 就绪队列、水平触发 vs 边沿触发 |
| 5 | HTTP 状态机解析 | 粘包、状态机、keep-alive |
| 6 | 主从 Reactor | 线程池、惊群、eventfd |
| 7 | 完整 Web 服务器 | 路由、sendfile、mmap、异步日志 |

## 压测结果

```
stage1 阻塞      → 压测失败（只能一个连接）
stage2 fork      →  65744 QPS
stage3 select    → 120576 QPS
stage4 epoll     → 139899 QPS
stage7 webserver →   2003 QPS（含磁盘 IO）
```

## 快速开始

```bash
# WSL2 里
bash scripts/setup_env.sh        # 安装工具链
cmake -B build -S . && cmake --build build  # 编译

# 运行
sudo build/bin/raw_sniff 8080         # stage0: 抓包
build/bin/echo_server 8080            # stage1: echo
build/bin/webserver 8080 4 ./www      # stage7: Web 服务器
build/bin/server -p 8080 -w 4 -r ./www # 聚合版
```

## 文档导航

- [字节序与结构体对齐](00_byte_order.md) —— 前置知识
- [stage0 - raw socket 抓包](01_raw_sniff.md) —— IP/TCP 头部、三次握手
- [stage1 - 阻塞 echo server](02_echo_blocking.md) —— socket API、fd、listen backlog
- [stage2 - 多进程并发](03_echo_fork.md) —— fork、fd 引用计数、TIME_WAIT、SIGCHLD
- [stage3 - select 多路复用](04_echo_select.md) —— FD_SETSIZE、O(n) 遍历
- [stage4 - epoll LT/ET](05_echo_epoll.md) —— 就绪队列、水平触发 vs 边沿触发
- [stage5 - HTTP 状态机解析](06_http_parser.md) —— 粘包、状态机、keep-alive
- [stage6 - 主从 Reactor](07_reactor.md) —— 线程池、惊群、eventfd
- [stage7 - 完整 Web 服务器](08_webserver.md) —— 路由、sendfile、mmap、异步日志
- [聚合版服务器](09_server.md) —— 完整架构与使用