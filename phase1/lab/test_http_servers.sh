#!/bin/bash
# 测试 stage5-7 的 HTTP server
cd /mnt/c/Users/wlh19/Desktop/webserve
PASS=0
FAIL=0

test_http() {
    local name=$1
    local bin=$2
    local port=$3
    local args=$4

    $bin $port $args > /tmp/http_${name}.log 2>&1 &
    local pid=$!
    sleep 0.5

    # 测试 HTTP 请求
    result=$(curl -s --connect-timeout 2 http://localhost:$port/ 2>/dev/null)
    if echo "$result" | grep -q "200\|Hello\|html\|HTTP"; then
        echo "[OK]   $name: HTTP 响应正常"
        PASS=$((PASS + 1))
    else
        echo "[FAIL] $name: 无响应或错误"
        FAIL=$((FAIL + 1))
    fi

    # 测试 API 请求（stage7）
    if [ "$name" = "stage7_webserver" ]; then
        result2=$(curl -s http://localhost:$port/api/test 2>/dev/null)
        if echo "$result2" | grep -q "worker\|Hello"; then
            echo "[OK]   $name: /api/ 动态请求正常"
            PASS=$((PASS + 1))
        else
            echo "[FAIL] $name: /api/ 请求失败"
            FAIL=$((FAIL + 1))
        fi

        # 测试静态文件
        result3=$(curl -s http://localhost:$port/test.txt 2>/dev/null)
        if echo "$result3" | grep -q "test text file"; then
            echo "[OK]   $name: 静态文件正常"
            PASS=$((PASS + 1))
        else
            echo "[FAIL] $name: 静态文件失败"
            FAIL=$((FAIL + 1))
        fi
    fi

    kill $pid 2>/dev/null
    wait $pid 2>/dev/null
    sleep 0.3
}

echo "=========================================="
echo "  HTTP server 功能测试"
echo "=========================================="
echo ""

test_http "stage5_http"     "build/bin/http_server"      9010 ""
test_http "stage6_reactor"  "build/bin/reactor_server"   9011 "4"
test_http "stage7_webserver" "build/bin/webserver"       9012 "4 ./www"

echo ""
echo "=========================================="
echo "  结果: $PASS 通过, $FAIL 失败"
echo "=========================================="