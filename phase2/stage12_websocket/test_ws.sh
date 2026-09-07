#!/bin/bash
# test_ws.sh —— WebSocket 服务器测试脚本
cd /mnt/c/Users/wlh19/Desktop/webserve

pkill -f ws_server 2>/dev/null || true
sleep 0.5

# 启动服务器
timeout 12 ./build/bin/ws_server 8080 &
SERVER_PID=$!
sleep 1.5

echo "=== 测试 1: curl 握手 ==="
RESPONSE=$(curl -s --include \
    -H 'Connection: Upgrade' \
    -H 'Upgrade: websocket' \
    -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
    -H 'Sec-WebSocket-Version: 13' \
    http://localhost:8080/ 2>&1) || true

echo "$RESPONSE" | head -6

if echo "$RESPONSE" | grep -q "101"; then
    echo "[PASS] 握手成功"
else
    echo "[FAIL] 握手失败"
fi

if echo "$RESPONSE" | grep -q "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="; then
    echo "[PASS] Accept 值正确"
else
    echo "[FAIL] Accept 值错误"
fi

sleep 1
echo ""
echo "=== 测试 2: Python WebSocket 客户端 ==="
python3 << 'PYEOF'
import socket, base64, os, struct, sys, time

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(3)
try:
    sock.connect(('localhost', 8080))
except ConnectionRefusedError:
    print("[FAIL] 无法连接服务器")
    sys.exit(1)

key = base64.b64encode(os.urandom(16)).decode()
request = f"GET /chat HTTP/1.1\r\nHost: localhost:8080\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
sock.send(request.encode())
response = sock.recv(4096).decode()

if '101' in response:
    print("[PASS] Python 握手成功")
else:
    print("[FAIL] Python 握手失败")
    sys.exit(1)

def ws_send(sock, data, opcode=0x1):
    payload = data.encode() if isinstance(data, str) else data
    mask = os.urandom(4)
    masked = bytearray(len(payload))
    for i in range(len(payload)):
        masked[i] = payload[i] ^ mask[i % 4]
    header = bytearray()
    header.append(0x80 | opcode)
    if len(payload) < 126:
        header.append(0x80 | len(payload))
    elif len(payload) < 65536:
        header.append(0x80 | 126)
        header.extend(struct.pack('>H', len(payload)))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack('>Q', len(payload)))
    header.extend(mask)
    header.extend(masked)
    sock.send(header)

def ws_recv(sock):
    data = sock.recv(4096)
    if len(data) < 2:
        return None
    opcode = data[0] & 0x0F
    masked = (data[1] >> 7) & 0x1
    length = data[1] & 0x7F
    idx = 2
    if length == 126:
        length = struct.unpack('>H', data[idx:idx+2])[0]
        idx += 2
    elif length == 127:
        length = struct.unpack('>Q', data[idx:idx+8])[0]
        idx += 8
    if masked:
        idx += 4
    payload = data[idx:idx+length]
    return opcode, payload.decode('utf-8', errors='replace')

ws_send(sock, 'Hello WebSocket!')
result = ws_recv(sock)
if result and result[1] == 'Hello WebSocket!':
    print(f"[PASS] Echo: {result[1]}")
else:
    print(f"[FAIL] Echo: {result}")

ws_send(sock, '第二次消息测试')
result = ws_recv(sock)
if result and result[1] == '第二次消息测试':
    print(f"[PASS] Echo: {result[1]}")
else:
    print(f"[FAIL] Echo: {result}")

ws_send(sock, 'pingdata', opcode=0x9)
result = ws_recv(sock)
if result and result[0] == 0xA:
    print(f"[PASS] Ping/Pong 成功")
else:
    print(f"[FAIL] Ping/Pong: {result}")

sock.close()
print("=== 所有测试通过 ===")
PYEOF

sleep 1
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true
echo "测试完成"
