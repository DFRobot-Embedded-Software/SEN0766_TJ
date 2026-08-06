# -*- coding: utf-8 -*-
import sys
import time

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
REMOTE = "/home/dfrobot/\u684c\u9762/hermes-voice-bridge"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.0.73", username="dfrobot", password="123456", timeout=15)


def run(cmd, timeout=20):
    _i, o, e = ssh.exec_command(cmd, timeout=timeout)
    # Avoid blocking forever on backgrounded shells
    o.channel.settimeout(timeout)
    e.channel.settimeout(timeout)
    try:
        out = o.read().decode("utf-8", errors="replace")
    except Exception as ex:
        out = f"<stdout timeout/err: {ex}>"
    try:
        err = e.read().decode("utf-8", errors="replace")
    except Exception as ex:
        err = f"<stderr timeout/err: {ex}>"
    return (out + err).strip()


print("kill:", run("pkill -9 -f 'python.*bridge.py' || true", timeout=8))
time.sleep(1.5)
print("after kill:", run("pgrep -af 'python.*bridge' || echo none", timeout=8))
print("port:", run("ss -lntp | grep 8765 || echo free", timeout=8))

# Start detached; don't wait on the long-lived process
chan = ssh.get_transport().open_session()
chan.exec_command(
    f"cd '{REMOTE}' && set -a && . ./.env && set +a && "
    f"nohup ./venv/bin/python -u bridge.py >> bridge_run.log 2>&1 < /dev/null &"
)
time.sleep(0.3)
chan.close()
time.sleep(2)

print("procs:", run("pgrep -af 'python.*bridge.py' || echo none", timeout=8))
print("port2:", run("ss -lntp | grep 8765 || echo free", timeout=8))
print("has_退下:", run(f"grep -c '退下' '{REMOTE}/bridge.py'", timeout=8))
print("has_reminder:", run(f"grep -c 'HERMES_EXIT_REMINDER' '{REMOTE}/bridge.py'", timeout=8))
print("log:", run(f"tail -n 12 '{REMOTE}/bridge_run.log'", timeout=8))
ssh.close()
