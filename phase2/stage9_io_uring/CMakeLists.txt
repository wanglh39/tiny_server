# stage9_io_uring/CMakeLists.txt
# io_uring 异步 IO 教学服务器

add_executable(uring_echo_server uring_echo_server.c)

target_link_libraries(uring_echo_server
    common_v2
    uring   # liburing
)

target_include_directories(uring_echo_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)