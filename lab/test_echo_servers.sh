#!/bin/bash
# 测试 stage1-4 的 echo server
cd /mnt/c/Users/wlh19/Desktop/webserve
PASS=0
FAIL=0

test_echo() {
    local name=$1
    local bin=$2
    local port=$3

    # 启动 server
    $bin $port > /tmp/server_${name}.log 2>&1 &
    local server_pid=$!
    sleep 0.5

    # 用 python 测试 echo
    python3 << PYEOF
import socket, sys
try:
    s = socket.socket()
    s.settimeout(3)
    s.connect(('localhost', $port))
    s.send(b"hello world\n")
    resp = s.recv(1024)
    if resp == b"hello world\n":
        print("[OK]   $name: echo 正确")
        sys.exit(0)
    else:
        print("[FAIL] $name: 收到 %r 期望 b'hello world\\n'" % resp)
        sys.exit(1)
except Exception as e:
    print("[FAIL] $name: %s" % e)
    sys.exit(1)
finally:
    s.close()
PYEOF

    if [ $? -eq 0 ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi

    kill $server_pid 2>/dev/null
    wait $server_pid 2>/dev/null
    sleep 0.3
}

echo "=========================================="
echo "  echo server 功能测试"
echo "=========================================="
echo ""

test_echo "stage1_blocking"      "build/bin/echo_server"           9001
test_echo "stage2_fork"          "build/bin/echo_server_fork"      9002
test_echo "stage3_select"        "build/bin/echo_server_select"    9003
test_echo "stage4_epoll_lt"      "build/bin/echo_server_epoll_lt"  9004
test_echo "stage4_epoll_et"      "build/bin/echo_server_epoll_et"  9005

echo ""
echo "=========================================="
echo "  结果: $PASS 通过, $FAIL 失败"
echo "=========================================="