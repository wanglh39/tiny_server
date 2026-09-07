# stage7_webserver/CMakeLists.txt

add_executable(webserver webserver.c ../stage5_http_parser/http_parser.c)
target_link_libraries(webserver common pthread)
target_include_directories(webserver PRIVATE ../stage5_http_parser)