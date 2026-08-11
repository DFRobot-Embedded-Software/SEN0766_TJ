# -*- coding: utf-8 -*-
"""停旧服务并从 mcp-sensors-stack 拉起。"""
from __future__ import annotations

import sys
import time

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = "192.168.1.27"
USER = "haha"
PASSWORD = "123456"
STACK = "/home/haha/\u684c\u9762/mcp-sensors-stack"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(HOST, username=USER, password=PASSWORD, timeout=15)


def run(cmd: str, timeout: int = 90) -> str:
    _i, o, e = ssh.exec_command(cmd, timeout=timeout)
    o.channel.settimeout(timeout)
    try:
        out = o.read().decode("utf-8", errors="replace")
    except Exception as ex:
        out = str(ex)
    try:
        err = e.read().decode("utf-8", errors="replace")
    except Exception:
        err = ""
    return (out + err).strip()


print("=== stop ===")
print(run(f"cd '{STACK}' && ./stop_services.sh || true", timeout=30))
print(
    run(
        "fuser -k 9080/tcp 8765/tcp 2>/dev/null || true; "
        "pkill -9 -f 'python.*-m mcp_gw' 2>/dev/null || true; "
        "pkill -9 -f 'python.*bridge.py' 2>/dev/null || true",
        timeout=15,
    )
)
time.sleep(2)
print("=== start ===")
print(run(f"cd '{STACK}' && chmod +x *.sh && ./start_services.sh", timeout=180))
print("=== layout ===")
print(run(f"find '{STACK}' -maxdepth 2 -type f | sort"))
print("=== env ===")
print(
    run(
        f"test -f '{STACK}/mcp_gw/.env' && echo mcp_gw.env=ok; "
        f"test -f '{STACK}/voice-bridge/.env' && echo bridge.env=ok"
    )
)
ssh.close()
