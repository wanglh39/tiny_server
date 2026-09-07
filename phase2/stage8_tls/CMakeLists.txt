# stage8_tls/CMakeLists.txt
# TLS/HTTPS 教学服务器

add_executable(tls_server
    tls_server.c
    ../../phase1/stage5_http_parser/http_parser.c
)

target_link_libraries(tls_server
    common_v2
    pthread
    ssl    # OpenSSL
    crypto # OpenSSL 加密库
)

target_include_directories(tls_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../phase1/stage5_http_parser
)