# stage11_reuseport/CMakeLists.txt
# SO_REUSEPORT 多核 echo server

add_executable(reuseport_server reuseport_server.c)

target_link_libraries(reuseport_server
    common_v2
    pthread
)

target_include_directories(reuseport_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)