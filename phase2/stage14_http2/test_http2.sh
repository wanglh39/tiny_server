#!/bin/bash
# test_http2.sh —— HTTP/2 服务器测试
cd /mnt/c/Users/wlh19/Desktop/webserve

pkill -f http2_server 2>/dev/null || true
sleep 0.5

timeout 8 ./build/bin/http2_server 8080 &
SERVER_PID=$!
sleep 1.5

echo "=== 测试 1: curl HTTP/2 ==="
curl -s --http2-prior-knowledge -v http://localhost:8080/ 2>&1 | head -30
echo ""

echo "=== 测试 2: HTTP/2 多请求 ==="
curl -s --http2-prior-knowledge http://localhost:8080/test 2>&1
echo ""
curl -s --http2-prior-knowledge http://localhost:8080/api 2>&1
echo ""

echo "=== 测试完成 ==="
sleep 1
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true