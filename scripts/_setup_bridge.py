# -*- coding: utf-8 -*-
"""重建远端 voice-bridge 的 venv 并启动（跨机拷贝 venv 损坏时用）。"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = "192.168.1.27"
USER = "haha"
PASSWORD = "123456"
STACK = "/home/haha/\u684c\u9762/mcp-sensors-stack"
REMOTE = f"{STACK}/voice-bridge"
LOCAL_BRIDGE = Path(__file__).resolve().parents[1] / "server_stack" / "voice-bridge" / "bridge.py"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(HOST, username=USER, password=PASSWORD, timeout=15)


def run(cmd, timeout=180):
    _i, o, e = ssh.exec_command(cmd, timeout=timeout)
    o.channel.settimeout(timeout)
    try:
        out = o.read().decode("utf-8", errors="replace")
    except Exception as ex:
        out = f"<err {ex}>"
    try:
        err = e.read().decode("utf-8", errors="replace")
    except Exception:
        err = ""
    return (out + err).strip()


sftp = ssh.open_sftp()
sftp.put(str(LOCAL_BRIDGE), f"{REMOTE}/bridge.py")
sftp.close()
print("uploaded bridge.py")

print(run("fuser -k 8765/tcp 2>/dev/null || true; pkill -9 -f 'python.*bridge.py' 2>/dev/null || true", timeout=10))
time.sleep(1)

print(
    run(
        f"cd '{REMOTE}' && rm -rf venv && python3 -m venv venv && "
        f"./venv/bin/pip install -q -U pip && ./venv/bin/pip install -q -r requirements.txt",
        timeout=300,
    )
)
print("venv:", run(f"'{REMOTE}/venv/bin/python' -V"))

chan = ssh.get_transport().open_session()
chan.exec_command(
    f"cd '{REMOTE}' && set -a && . ./.env && set +a && "
    f"nohup ./venv/bin/python -u bridge.py >> '{STACK}/logs/bridge.log' 2>&1 < /dev/null &"
)
time.sleep(0.3)
chan.close()
time.sleep(2)
print("procs:", run("pgrep -af 'python.*bridge.py' || echo none", timeout=10))
print("port:", run("ss -lntp | grep 8765 || echo free", timeout=10))
print("log:", run(f"tail -n 15 '{STACK}/logs/bridge.log'", timeout=10))
ssh.close()
