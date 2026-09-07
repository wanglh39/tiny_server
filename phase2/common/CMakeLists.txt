# phase2/common/CMakeLists.txt
# 第二阶段公共模块
# 包含第一阶段的 error/log/wrap_posix + 新增 io_ops 抽象层

# 复用第一阶段的公共模块源码
add_library(common_v2 STATIC
    ../../phase1/common/error.c
    ../../phase1/common/log.c
    ../../phase1/common/byte_order.c
    ../../phase1/common/wrap_posix.c
)

target_link_libraries(common_v2 pthread)
target_include_directories(common_v2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../phase1/common
)