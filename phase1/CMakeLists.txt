# ============================================================
# phase1 - 基础阶段
# IO 模型演进 + HTTP 解析 + Reactor + 完整 Web 服务器
# ============================================================

# ---------- common 静态库 ----------
add_subdirectory(common)

# ---------- 各阶段可执行文件 ----------
add_subdirectory(stage0_raw_sniff)
add_subdirectory(stage1_echo_blocking)
add_subdirectory(stage2_echo_fork)
add_subdirectory(stage3_echo_select)
add_subdirectory(stage4_echo_epoll)
add_subdirectory(stage5_http_parser)
add_subdirectory(stage6_reactor)
add_subdirectory(stage7_webserver)

# 聚合版完整服务器
add_subdirectory(server)

message(STATUS "  phase1: 基础阶段（stage0-7 + server）")
