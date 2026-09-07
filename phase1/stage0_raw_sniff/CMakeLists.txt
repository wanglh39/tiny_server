# stage0_raw_sniff/CMakeLists.txt

add_executable(raw_sniff sniff.c)

# 链接 common 静态库
target_link_libraries(raw_sniff common)