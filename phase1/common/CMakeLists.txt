# common/CMakeLists.txt
# 把公共模块编译成静态库 libcommon.a，供各 stage 链接

add_library(common STATIC
    error.c
    log.c
    byte_order.c
    wrap_posix.c
)

# 需要 pthread（log.c 里 localtime_r 在某些系统需要）
# 需要 rt（clock_gettime 在某些系统需要）
target_link_libraries(common pthread)

# 让各 stage 能 #include "error.h" 等
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})