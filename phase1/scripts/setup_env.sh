#!/bin/bash
# ============================================================
# WSL2 环境准备脚本 —— 为 tiny_server 教学项目安装工具链
# 用法：在 WSL2 里执行  bash setup_env.sh
# ============================================================

set -e  # 任何命令失败立即退出

echo "=========================================="
echo "  tiny_server 环境准备"
echo "=========================================="

# ---------- 1. 更新 apt 索 ----------
echo "[1/6] 更新 apt 软件源..."
sudo apt-get update -y

# ---------- 2. 安装编译工具链 ----------
echo "[2/6] 安装编译工具链 (gcc / g++ / make / cmake)..."
sudo apt-get install -y build-essential cmake

# ---------- 3. 安装网络抓包与压测工具 ----------
echo "[3/6] 安装网络工具 (tcpdump / netcat / curl / wget)..."
sudo apt-get install -y tcpdump netcat-openbsd curl wget

echo "[3.1] 安装压测工具 (apache2-utils 提供 ab)..."
sudo apt-get install -y apache2-utils

# ---------- 4. 安装调试工具 ----------
echo "[4/6] 安装调试工具 (strace / ltrace / gdb)..."
sudo apt-get install -y strace ltrace gdb

# ---------- 5. 验证安装 ----------
echo "[5/6] 验证安装结果..."
echo "------------------------------------------"
check_cmd() {
    if command -v "$1" &>/dev/null; then
        printf "  [OK]   %-12s -> %s\n" "$1" "$(command -v "$1")"
    else
        printf "  [FAIL] %-12s 未找到\n" "$1"
        return 1
    fi
}
check_cmd gcc
check_cmd g++
check_cmd make
check_cmd cmake
check_cmd tcpdump
check_cmd nc
check_cmd curl
check_cmd ab
check_cmd strace
check_cmd gdb
echo "------------------------------------------"

# ---------- 6. 检查 gcc 版本 ----------
echo "[6/6] 编译器版本信息："
gcc --version | head -1
cmake --version | head -1

echo ""
echo "=========================================="
echo "  环境准备完成！"
echo "  接下来在项目根目录执行："
echo "    cmake -B build -S ."
echo "    cmake --build build"
echo "=========================================="