# -*- coding: utf-8 -*-
import sys
import time

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

LOCAL = r"c:\Users\13348\Desktop\mcp_sensors\scripts\_bridge_remote\bridge.py"
REMOTE_DIR = "/home/dfrobot/\u684c\u9762/hermes-voice-bridge"
REMOTE_FILE = REMOTE_DIR + "/bridge.py"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.0.73", username="dfrobot", password="123456", timeout=15)

sftp = ssh.open_sftp()
sftp.put(LOCAL, REMOTE_FILE)
sftp.close()
print("uploaded ok")

def run(cmd: str) -> str:
    _i, o, e = ssh.exec_command(cmd, timeout=45)
    out = o.read().decode("utf-8", errors="replace")
    err = e.read().decode("utf-8", errors="replace")
    return out + (("\nSTDERR:\n" + err) if err.strip() else "")

print(run(f"grep -n 'hermes_session_id\\|X-Hermes-Session-Id\\|reset_dialog\\|reset_messages' '{REMOTE_FILE}' | head -40"))
print(run("pkill -f 'python.*bridge.py' || true"))
time.sleep(1.5)
print(
    run(
        f"cd '{REMOTE_DIR}' && set -a && [ -f .env ] && . ./.env; set +a && "
        f"nohup ./venv/bin/python -u bridge.py > bridge_run.log 2>&1 & sleep 2; "
        f"pgrep -af 'python.*bridge'; echo '---'; tail -n 20 bridge_run.log"
    )
)
ssh.close()
