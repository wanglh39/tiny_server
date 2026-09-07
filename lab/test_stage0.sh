#!/bin/bash
# 临时测试脚本：验证 stage0 抓包
cd /mnt/c/Users/wlh19/Desktop/webserve

# 启动 nc 服务器监听 8080
nc -l 8080 &
NC_PID=$!
sleep 0.5

# 启动 raw_sniff 后台，抓 3 秒
timeout 3 build/bin/raw_sniff 8080 &
SNIFF_PID=$!
sleep 1

# 发起连接触发三次握手
curl -s --connect-timeout 1 http://localhost:8080/ 2>/dev/null

# 等待 raw_sniff 结束
wait $SNIFF_PID 2>/dev/null

# 清理
kill $NC_PID 2>/dev/null
wait 2>/dev/null