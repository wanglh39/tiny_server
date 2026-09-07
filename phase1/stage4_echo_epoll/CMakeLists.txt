# stage4_echo_epoll/CMakeLists.txt

# LT 版本
add_executable(echo_server_epoll_lt echo_server_epoll_lt.c)
target_link_libraries(echo_server_epoll_lt common)

# ET 版本
add_executable(echo_server_epoll_et echo_server_epoll_et.c)
target_link_libraries(echo_server_epoll_et common)