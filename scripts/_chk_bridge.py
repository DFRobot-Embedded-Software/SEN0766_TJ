# -*- coding: utf-8 -*-
import paramiko
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.0.73", username="dfrobot", password="123456", timeout=15)
cmd = r"""
pgrep -af 'python.*bridge' || true
echo '===== LOG ====='
tail -n 120 /home/dfrobot/桌面/hermes-voice-bridge/bridge_run.log
echo '===== CODE ====='
grep -n 'listen_stop\|_close_connection\|play_start\|uplink_stop\|保持' /home/dfrobot/桌面/hermes-voice-bridge/bridge.py | head -60
"""
stdin, stdout, stderr = ssh.exec_command(cmd, timeout=20)
print(stdout.read().decode("utf-8", errors="replace"))
print(stderr.read().decode("utf-8", errors="replace"))
ssh.close()
