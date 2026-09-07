#!/bin/bash
# ============================================================
# run_stage0.sh —— 一键运行 stage0 抓包实验
#
# 用法：
#   sudo bash lab/run_stage0.sh
#
# 这个脚本自动完成：
#   1. 启动 raw_sniff 抓包
#   2. 等 1 秒后发起 TCP 连接
#   3. 让你看到完整的三次握手 + 数据 + 四次挥手
#   4. 3 秒后自动退出
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BIN="$PROJECT_ROOT/build/bin/raw_sniff"

if [ "$EUID" -ne 0 ]; then
    echo "需要 sudo 权限（raw socket 需要 root）"
    echo "用法： sudo bash lab/run_stage0.sh"
    exit 1
fi

if [ ! -f "$BIN" ]; then
    echo "raw_sniff 未编译，请先执行："
    echo "  cmake -B build -S ."
    echo "  cmake --build build"
    exit 1
fi

PORT=8080

echo "=========================================="
echo "  stage0 一键实验"
echo "=========================================="
echo ""

# 启动一个临时的 TCP 服务器（用 nc）
nc -l $PORT &
NC_PID=$!
sleep 0.5

# 启动抓包（后台，抓 3 秒）
timeout 3 $BIN $PORT &
SNIFF_PID=$!

sleep 0.5

# 发起连接
echo ">>> 发起连接 curl http://localhost:$PORT/"
curl -s http://localhost:$PORT/ 2>/dev/null || true

# 等抓包结束
wait $SNIFF_PID 2>/dev/null

# 清理
kill $NC_PID 2>/dev/null

echo ""
echo "=========================================="
echo "  实验结束"
echo "  查看详细讲解： docs/01_raw_sniff.md"
echo "=========================================="