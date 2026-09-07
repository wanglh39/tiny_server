# stage1_echo_blocking/CMakeLists.txt

add_executable(echo_server echo_server.c)
target_link_libraries(echo_server common)