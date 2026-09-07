#!/bin/bash
# ============================================================
# tcpdump_handshake.sh —— 对照实验：用 tcpdump 验证三次握手
#
# 用法：
#   sudo bash lab/tcpdump_handshake.sh
#
# 这个脚本做三件事：
#   1. 启动 tcpdump 抓包（后台）
#   2. 发起一个 TCP 连接
#   3. 展示抓到的三次握手 + 四次挥手
#
# 对比我们的 raw_sniff 输出，应该完全一致。
# ============================================================

PORT=${1:-8080}
DURATION=5  # 抓包时长（秒）

echo "=========================================="
echo "  三次握手对照实验"
echo "  端口: $PORT"
echo "  抓包时长: ${DURATION}s"
echo "=========================================="
echo ""

# 检查权限
if [ "$EUID" -ne 0 ]; then
    echo "需要 sudo 权限运行"
    exit 1
fi

# 检查工具
if ! command -v tcpdump &>/dev/null; then
    echo "tcpdump 未安装，请先运行 scripts/setup_env.sh"
    exit 1
fi

echo "[1] 启动 tcpdump 抓包（后台 ${DURATION}s）..."
echo "    命令: tcpdump -i lo port $PORT -nn -S -c 20"
echo ""

# 启动 tcpdump，最多抓 20 个包或 ${DURATION} 秒
# -i lo：监听本地回环接口
# -nn：不解析主机名和端口名（更快，输出 IP:port）
# -S：打印绝对序列号（默认是相对的）
# -c 20：最多抓 20 个包
timeout ${DURATION} tcpdump -i lo port $PORT -nn -S -c 20 &
TCPDUMP_PID=$!

# 等一下让 tcpdump 就绪
sleep 0.5

echo "[2] 发起 TCP 连接..."
echo "    命令: curl http://localhost:$PORT/"
echo ""

# 发起连接（即使服务器不存在，SYN 也会发出去）
curl -s --connect-timeout 2 http://localhost:$PORT/ 2>/dev/null || true

# 等 tcpdump 结束
wait $TCPDUMP_PID 2>/dev/null

echo ""
echo "=========================================="
echo "  实验结束"
echo "  对比说明："
echo "    tcpdump 的 'Flags [S]'  = 我们的 [SYN]"
echo "    tcpdump 的 'Flags [S.]' = 我们的 [SYN,ACK]"
echo "    tcpdump 的 'Flags [.]'  = 我们的 [ACK]"
echo "    tcpdump 的 'Flags [F.]' = 我们的 [FIN,ACK]"
echo "    tcpdump 的 'seq xxx'    = 我们的 seq=xxx"
echo "    tcpdump 的 'ack xxx'    = 我们的 ack=xxx"
echo "=========================================="