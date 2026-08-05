# -*- coding: utf-8 -*-
import sys
import time

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REMOTE_DIR = "/home/dfrobot/\u684c\u9762/hermes-voice-bridge"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.0.73", username="dfrobot", password="123456", timeout=15)


def run(cmd: str, timeout: int = 20) -> str:
    _i, o, e = ssh.exec_command(cmd, timeout=timeout)
    o.channel.settimeout(timeout)
    out = o.read().decode("utf-8", errors="replace")
    err = e.read().decode("utf-8", errors="replace")
    return (out + (("\nSTDERR:\n" + err) if err.strip() else "")).strip()


print("before:", run("pgrep -af 'python.*bridge.py' || echo none"))
print("kill:", run("pkill -f '[p]ython.*bridge.py'; sleep 1; pgrep -af 'python.*bridge.py' || echo killed"))
# start detached via setsid
start = (
    f"cd '{REMOTE_DIR}' && "
    "bash -lc 'set -a; [ -f .env ] && . ./.env; set +a; "
    "setsid ./venv/bin/python -u bridge.py >> bridge_run.log 2>&1 < /dev/null &'"
)
print("start:", run(start, timeout=10))
time.sleep(2)
print("after:", run("pgrep -af 'python.*bridge.py' || echo none"))
print("log:", run(f"tail -n 25 '{REMOTE_DIR}/bridge_run.log'"))
ssh.close()
