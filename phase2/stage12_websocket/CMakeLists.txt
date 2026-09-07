# stage12_websocket/CMakeLists.txt
# WebSocket 服务器

add_executable(ws_server ws_server.c)

target_link_libraries(ws_server
    common_v2
    pthread
    ssl    # OpenSSL（SHA1 + Base64）
    crypto
)

target_include_directories(ws_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)