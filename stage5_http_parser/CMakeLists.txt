# stage5_http_parser/CMakeLists.txt

add_executable(http_server http_server.c http_parser.c)
target_link_libraries(http_server common)