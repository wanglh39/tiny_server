#!/bin/bash
# test_co_echo.sh —— 协程 echo 服务器测试
cd /mnt/c/Users/wlh19/Desktop/webserve

pkill -f co_echo_server 2>/dev/null || true
sleep 0.5

timeout 8 ./build/bin/co_echo_server 8080 &
SERVER_PID=$!
sleep 1.5

echo "=== 测试 1: 单连接 echo ==="
echo "hello coroutine" | nc -q 1 localhost 8080
echo ""

echo "=== 测试 2: 多消息 ==="
(echo "msg1"; sleep 0.2; echo "msg2"; sleep 0.2; echo "msg3") | nc -q 1 localhost 8080
echo ""

echo "=== 测试 3: 并发连接 ==="
for i in 1 2 3 4 5; do
    echo "conn$i" | nc -q 1 localhost 8080 &
done
wait
echo ""

echo "=== 所有测试完成 ==="
sleep 1
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true