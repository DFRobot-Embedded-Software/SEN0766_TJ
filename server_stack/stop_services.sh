#!/usr/bin/env bash
# 一键停止本目录相关服务
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

green() { printf '\033[0;32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[0;33m%s\033[0m\n' "$*"; }

kill_port() {
  local port="$1"
  if command -v fuser >/dev/null 2>&1; then
    fuser -k "${port}/tcp" 2>/dev/null || true
  fi
}

echo ">>> 停止服务（栈目录: ${ROOT}）"
yellow "[停止] mcp_gw"
pkill -f 'python -m mcp_gw' 2>/dev/null || true
kill_port 9080

yellow "[停止] 语音 bridge"
pkill -f 'python.*bridge.py' 2>/dev/null || true
kill_port 8765

yellow "[停止] 爱马仕 gateway"
pkill -f 'hermes gateway' 2>/dev/null || true
kill_port 8642

sleep 1
green "完成。可用 ./status_services.sh 确认。"
