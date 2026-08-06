# -*- coding: utf-8 -*-
import sys
import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
REMOTE = "/home/dfrobot/\u684c\u9762/hermes-voice-bridge"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.0.73", username="dfrobot", password="123456", timeout=15)

def run(cmd, timeout=25):
    _i, o, e = ssh.exec_command(cmd, timeout=timeout)
    o.channel.settimeout(timeout)
    e.channel.settimeout(timeout)
    try:
        out = o.read().decode("utf-8", errors="replace")
    except Exception as ex:
        out = f"<err {ex}>"
    try:
        err = e.read().decode("utf-8", errors="replace")
    except Exception:
        err = ""
    return (out + err).strip()

print("=== procs/port ===")
print(run("pgrep -af 'python.*bridge.py' || echo none"))
print(run("ss -lntp | grep 8765 || echo free"))
print("\n=== log size / mtime ===")
print(run(f"ls -la '{REMOTE}/bridge_run.log'; wc -l '{REMOTE}/bridge_run.log'"))
print("\n=== dialogue keywords (last 120) ===")
print(run(
    f"grep -nE 'ASR|最终|transcript|Hermes|退出|dialog_end|dialog_idle|"
    f"uplink_|设备连接|断开|play_stop|reply|退下|Bye|再见|session' "
    f"'{REMOTE}/bridge_run.log' | tail -n 120"
))
print("\n=== last 80 lines ===")
print(run(f"tail -n 80 '{REMOTE}/bridge_run.log'"))
ssh.close()
