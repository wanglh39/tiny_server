# stage13_coroutine/CMakeLists.txt
# 协程（ucontext 实现）

# 协程库
add_library(coroutine STATIC coroutine.c)
target_include_directories(coroutine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(coroutine common_v2)

# 基础演示
add_executable(co_demo co_demo.c)
target_link_libraries(co_demo coroutine common_v2)

# 协程版 echo 服务器
add_executable(co_echo_server co_echo_server.c)
target_link_libraries(co_echo_server coroutine common_v2)