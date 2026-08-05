# -*- coding: utf-8 -*-
import paramiko
import os
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
LOCAL_DIR = r"c:\Users\13348\Desktop\mcp_sensors\scripts\_bridge_remote"
os.makedirs(LOCAL_DIR, exist_ok=True)
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.0.73", username="dfrobot", password="123456", timeout=15)
sftp = ssh.open_sftp()
sftp.get("/home/dfrobot/桌面/hermes-voice-bridge/bridge.py", os.path.join(LOCAL_DIR, "bridge.py"))
sftp.close()
ssh.close()
print("pulled", os.path.getsize(os.path.join(LOCAL_DIR, "bridge.py")))
