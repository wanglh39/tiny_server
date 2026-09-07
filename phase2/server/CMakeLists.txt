# server/CMakeLists.txt
# 聚合版高级服务器：整合 phase2 全部 7 个进阶特性

add_executable(advanced_server
    server.c
    worker.c
    connection.c
    tls_conn.c
    ws_conn.c
    h2_conn.c
    static_file.c
    # 外部依赖源码
    ../../phase1/stage5_http_parser/http_parser.c
    ../stage10_mempool_log/mempool.c
    ../stage10_mempool_log/async_log.c
)

target_link_libraries(advanced_server
    common_v2
    pthread
    ssl
    crypto
)

target_include_directories(advanced_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../phase1/stage5_http_parser
    ../stage10_mempool_log
    ../../phase1/common
)