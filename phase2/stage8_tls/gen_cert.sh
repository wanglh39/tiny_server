#!/bin/bash
# ============================================================
# gen_cert.sh —— 生成自签名 TLS 证书（教学用）
#
# 生产环境请用 Let's Encrypt 或正规 CA 签发的证书！
# ============================================================

CERT_DIR="$(dirname "$0")/certs"
mkdir -p "$CERT_DIR"

echo "=== 生成自签名证书 ==="
echo "证书目录: $CERT_DIR"
echo ""

# 生成 RSA 私钥（2048 位）
openssl genrsa -out "$CERT_DIR/key.pem" 2048 2>/dev/null

# 生成自签名证书（有效期 365 天）
#   -x509: 直接生成自签名证书（而不是 CSR）
#   -nodes: 不加密私钥（教学用，生产应加密）
#   -days 365: 有效期一年
openssl req -new -x509 -key "$CERT_DIR/key.pem" \
    -out "$CERT_DIR/cert.pem" -days 365 \
    -subj "/C=CN/ST=Teaching/L=WSL2/O=tiny_server/OU=stage8/CN=localhost" 2>/dev/null

echo "证书文件: $CERT_DIR/cert.pem"
echo "私钥文件: $CERT_DIR/key.pem"
echo ""

# 验证证书
echo "=== 证书信息 ==="
openssl x509 -in "$CERT_DIR/cert.pem" -noout -subject -dates
echo ""

echo "=== 完成 ==="
echo "现在可以启动 HTTPS 服务器："
echo "  build/bin/tls_server 8443 4 ./phase2/www \\"
echo "    $CERT_DIR/cert.pem $CERT_DIR/key.pem"
echo ""
echo "测试："
echo "  curl -k https://localhost:8443/"
echo "  （-k 跳过证书验证，因为自签名证书不被信任）"