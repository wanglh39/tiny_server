# stage6_reactor/CMakeLists.txt

add_executable(reactor_server reactor_server.c ../stage5_http_parser/http_parser.c)
target_link_libraries(reactor_server common pthread)
target_include_directories(reactor_server PRIVATE ../stage5_http_parser)