# ============================================================
# tiny_server 顶层 CMakeLists.txt
# 教学项目：从 raw socket 到完整 Web 服务器的演进
# ============================================================
cmake_minimum_required(VERSION 3.10)

# ---------- 项目信息 ----------
project(tiny_server
    VERSION 0.1.0
    DESCRIPTION "教学用 C 语言 Web 服务器，理解计算机网络底层原理"
    LANGUAGES C
)

# ---------- C 标准 ----------
# C99：够用，且大多数教材基于 C99
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------- 编译选项 ----------
# -Wall -Wextra：开启常用警告，教学项目要看到所有潜在问题
# -g：生成调试信息，方便 gdb 调试
# -O0：不优化，让变量和执行流与源码一一对应
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -O0")

# ---------- 输出目录 ----------
# 所有可执行文件放到 build/bin，方便统一运行
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------- common 静态库 ----------
# 所有 stage 共用的工具函数（POSIX 包装、错误处理、日志等）
add_subdirectory(common)

# ---------- 各阶段可执行文件 ----------
# 每个阶段是一个独立的可执行文件，能单独编译运行
# 按教学顺序逐步添加
add_subdirectory(stage0_raw_sniff)
add_subdirectory(stage1_echo_blocking)
add_subdirectory(stage2_echo_fork)
add_subdirectory(stage3_echo_select)
add_subdirectory(stage4_echo_epoll)
add_subdirectory(stage5_http_parser)
add_subdirectory(stage6_reactor)
add_subdirectory(stage7_webserver)

# 聚合版完整服务器（整合所有阶段）
add_subdirectory(server)

# ---------- 信息打印 ----------
message(STATUS "==========================================")
message(STATUS "  tiny_server 教学项目")
message(STATUS "  C 标准: C${CMAKE_C_STANDARD}")
message(STATUS "  编译器: ${CMAKE_C_COMPILER}")
message(STATUS "  构建类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "==========================================")