# stage10_mempool_log/CMakeLists.txt
# 内存池 + 异步日志

add_executable(mempool_log_demo
    demo.c
    mempool.c
    async_log.c
)

target_link_libraries(mempool_log_demo
    common_v2
    pthread
)

target_include_directories(mempool_log_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)