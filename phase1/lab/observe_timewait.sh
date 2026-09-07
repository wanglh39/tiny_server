#!/bin/bash
# ============================================================
# observe_timewait.sh —— 观察 TIME_WAIT 状态
#
# 用法：
#   bash lab/observe_timewait.sh
#
# 演示：
#   1. 起一个简单的 TCP 服务器
#   2. 客户端连接后主动关闭
#   3. 观察主动关闭方进入 TIME_WAIT
#   4. 等待 2*MSL 后 TIME_WAIT 消失
# ============================================================

PORT=${1:-9090}

echo "=========================================="
echo "  TIME_WAIT 观察实验"
echo "  端口: $PORT"
echo "=========================================="
echo ""

# 用 python 起一个简单的 TCP 服务器
python3 -c "
import socket, threading, time

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('localhost', $PORT))
server.listen(1)
print(f'[服务器] 监听 localhost:$PORT')

def handle(conn):
    data = conn.recv(1024)
    print(f'[服务器] 收到: {data}')
    conn.close()  # 服务器主动关闭
    print('[服务器] 已 close()')

conn, addr = server.accept()
print(f'[服务器] 接受连接 from {addr}')
handle(conn)
print('[服务器] 等待 70 秒，观察 TIME_WAIT...')
time.sleep(70)
server.close()
" &
SERVER_PID=$!

sleep 1

# 客户端连接
echo "[客户端] 连接 localhost:$PORT..."
python3 -c "
import socket, time
s = socket.socket()
s.connect(('localhost', $PORT))
s.send(b'hello')
time.sleep(0.5)
s.close()  # 客户端主动关闭
print('[客户端] 已 close()')
"

echo ""
echo "[观察] 连接状态（每 5 秒查一次，共 70 秒）："
echo ""

for i in $(seq 1 14); do
    echo "--- 第 $((i*5)) 秒 ---"
    ss -tan | grep ":$PORT" || echo "  (无相关连接)"
    sleep 5
done

echo ""
echo "=========================================="
echo "  实验结束"
echo "  你应该看到："
echo "    1. 主动 close 的一方进入 TIME-WAIT"
echo "    2. TIME-WAIT 持续约 60 秒（2*MSL）"
echo "    3. 之后连接消失"
echo "  这就是为什么服务器重启需要 SO_REUSEADDR"
echo "=========================================="

kill $SERVER_PID 2>/dev/null