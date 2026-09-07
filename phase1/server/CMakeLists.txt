# server/CMakeLists.txt
# 聚合版完整服务器

add_executable(server
    server.c
    config.c
    router.c
    connection.c
    static_file.c
    worker.c
    ../stage5_http_parser/http_parser.c
)

target_link_libraries(server common pthread)

# 能找到 http_parser.h
target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../stage5_http_parser
)