# ============================================================
# tiny_server 顶层 CMakeLists.txt
# 教学项目：从 raw socket 到完整 Web 服务器的演进
#
# 项目分两个阶段：
#   phase1 - 基础阶段：IO 模型演进 + HTTP + Reactor + Web 服务器
#   phase2 - 进阶阶段：TLS + io_uring + 内存池 + WebSocket + 协程 + HTTP/2
# ============================================================
cmake_minimum_required(VERSION 3.10)

project(tiny_server
    VERSION 0.2.0
    DESCRIPTION "教学用 C 语言 Web 服务器，理解计算机网络底层原理"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -O0")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------- 第一阶段：基础 ----------
add_subdirectory(phase1)

# ---------- 第二阶段：进阶 ----------
add_subdirectory(phase2)

# ---------- 信息打印 ----------
message(STATUS "==========================================")
message(STATUS "  tiny_server 教学项目")
message(STATUS "  C 标准: C${CMAKE_C_STANDARD}")
message(STATUS "  编译器: ${CMAKE_C_COMPILER}")
message(STATUS "  阶段: phase1(基础) + phase2(进阶)")
message(STATUS "==========================================")