#!/bin/bash
# 完整三次握手测试
cd /mnt/c/Users/wlh19/Desktop/webserve

# 用 python 起一个 TCP 服务器
python3 << 'PYEOF' &
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('localhost', 8080))
s.listen(1)
s.settimeout(4)
try:
    conn, addr = s.accept()
    data = conn.recv(1024)
    conn.send(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello")
    time.sleep(0.5)
    conn.close()
except:
    pass
s.close()
PYEOF
sleep 0.5

# 启动 raw_sniff 抓包
nohup timeout 3 build/bin/raw_sniff 8080 > /tmp/sniff_full.txt 2>&1 &
sleep 1

# 发起 HTTP 请求
curl -s http://localhost:8080/ 2>/dev/null

sleep 3
cat /tmp/sniff_full.txt