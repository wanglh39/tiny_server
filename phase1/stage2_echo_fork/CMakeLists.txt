# stage2_echo_fork/CMakeLists.txt

add_executable(echo_server_fork echo_server_fork.c)
target_link_libraries(echo_server_fork common)