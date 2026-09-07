# ============================================================
# phase2 - 进阶阶段
# TLS + io_uring + 内存池 + 异步日志 + WebSocket + 协程 + HTTP/2
#
# 核心设计：IO 抽象层（io_ops）
#   不同协议/IO模型实现统一的 read/write/close 接口
#   业务代码与底层 IO 完全解耦
# ============================================================

# ---------- common 静态库 ----------
add_subdirectory(common)

# ---------- 各阶段可执行文件 ----------
add_subdirectory(stage8_tls)
add_subdirectory(stage9_io_uring)
add_subdirectory(stage10_mempool_log)
add_subdirectory(stage11_reuseport)
add_subdirectory(stage12_websocket)
add_subdirectory(stage13_coroutine)
add_subdirectory(stage14_http2)

# 聚合版进阶服务器
add_subdirectory(server)

message(STATUS "  phase2: 进阶阶段（stage8-14 全部实现）")