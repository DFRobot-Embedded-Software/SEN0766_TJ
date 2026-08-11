# -*- coding: utf-8 -*-
"""上传并重启 voice-bridge（位于远端 mcp-sensors-stack/voice-bridge）。"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = "192.168.1.27"
USER = "haha"
PASSWORD = "123456"
REMOTE_DIR = "/home/haha/\u684c\u9762/mcp-sensors-stack/voice-bridge"
LOCAL = Path(__file__).resolve().parents[1] / "server_stack" / "voice-bridge" / "bridge.py"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(HOST, username=USER, password=PASSWORD, timeout=15)

sftp = ssh.open_sftp()
sftp.put(str(LOCAL), f"{REMOTE_DIR}/bridge.py")
sftp.close()
print("uploaded bridge.py →", REMOTE_DIR)


def run(cmd: str, timeout: int = 60) -> str:
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


print(run("fuser -k 8765/tcp 2>/dev/null || true; pkill -9 -f 'python.*bridge.py' 2>/dev/null || true", timeout=15))
time.sleep(1)
chan = ssh.get_transport().open_session()
chan.exec_command(
    f"cd '{REMOTE_DIR}' && set -a && . ./.env && set +a && "
    f"nohup ./venv/bin/python -u bridge.py >> ../logs/bridge.log 2>&1 < /dev/null &"
)
time.sleep(0.3)
chan.close()
time.sleep(2)
print("procs:", run("pgrep -af 'python.*bridge.py' || echo none", timeout=10))
print("port:", run("ss -lntp | grep 8765 || echo free", timeout=10))
ssh.close()
