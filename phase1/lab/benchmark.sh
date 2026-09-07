#!/bin/bash
# ============================================================
# benchmark.sh —— 压测对比：select vs epoll LT vs epoll ET vs Reactor
#
# 用 ab (Apache Bench) 压测各阶段服务器，对比 QPS
# 用法： bash lab/benchmark.sh
# ============================================================

cd /mnt/c/Users/wlh19/Desktop/webserve

# 压测参数
CONCURRENCY=100   # 并发连接数
REQUESTS=2000     # 总请求数
PORT_BASE=9100

echo "=========================================="
echo "  压测对比实验"
echo "  并发: $CONCURRENCY, 总请求: $REQUESTS"
echo "=========================================="
echo ""

run_bench() {
    local name=$1
    local bin=$2
    local port=$3
    local extra=$4

    $bin $port $extra > /dev/null 2>&1 &
    local pid=$!
    sleep 0.5

    # 先发一个请求预热
    curl -s http://localhost:$port/ > /dev/null 2>&1

    # ab 压测
    # -n：总请求数
    # -c：并发数
    # -k：keep-alive
    local result=$(ab -n $REQUESTS -c $CONCURRENCY -k -l \
                    http://localhost:$port/ 2>/dev/null)

    local qps=$(echo "$result" | grep "Requests per second" | awk '{print $4}')
    local tpr=$(echo "$result" | grep "Time per request" | head -1 | awk '{print $4}')
    local failed=$(echo "$result" | grep "Failed requests" | awk '{print $3}')

    if [ -n "$qps" ]; then
        printf "  %-20s  QPS: %8.1f   延迟: %6.2f ms   失败: %s\n" \
               "$name" "$qps" "$tpr" "$failed"
    else
        printf "  %-20s  压测失败\n" "$name"
    fi

    kill $pid 2>/dev/null
    wait $pid 2>/dev/null
    sleep 0.3
}

echo "--- echo server (stage 1-4) ---"
echo ""
run_bench "stage1_blocking"    "build/bin/echo_server"          $PORT_BASE ""
run_bench "stage2_fork"        "build/bin/echo_server_fork"     $((PORT_BASE+1)) ""
run_bench "stage3_select"      "build/bin/echo_server_select"   $((PORT_BASE+2)) ""
run_bench "stage4_epoll_lt"    "build/bin/echo_server_epoll_lt" $((PORT_BASE+3)) ""
run_bench "stage4_epoll_et"    "build/bin/echo_server_epoll_et" $((PORT_BASE+4)) ""

echo ""
echo "--- HTTP server (stage 5-7) ---"
echo ""
run_bench "stage5_http"        "build/bin/http_server"          $((PORT_BASE+5)) ""
run_bench "stage6_reactor(4t)" "build/bin/reactor_server"       $((PORT_BASE+6)) "4"
run_bench "stage7_webserver"   "build/bin/webserver"            $((PORT_BASE+7)) "4 ./www"

echo ""
echo "=========================================="
echo "  说明："
echo "    QPS = 每秒处理请求数（越高越好）"
echo "    延迟 = 平均请求延迟（越低越好）"
echo "    stage3 select 在高并发下应明显慢于 epoll"
echo "    stage6/7 多线程应比单线程吞吐更高"
echo "=========================================="