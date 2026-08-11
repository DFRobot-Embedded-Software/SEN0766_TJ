#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
echo "栈目录: ${ROOT}"
for p in 9080 8765 8642; do
  if ss -lntp 2>/dev/null | grep -q ":${p} "; then
    echo "OK  :${p}"
  else
    echo "DOWN:${p}"
  fi
done
