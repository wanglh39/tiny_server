# stage14_http2/CMakeLists.txt
# HTTP/2 服务器

add_executable(http2_server http2_server.c)
target_link_libraries(http2_server common_v2)
target_include_directories(http2_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})