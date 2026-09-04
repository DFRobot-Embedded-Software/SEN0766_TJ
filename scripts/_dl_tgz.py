# -*- coding: utf-8 -*-
import pathlib
import sys

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.1.193", username="dfrobot", password="123456", timeout=20)
_, o, e = ssh.exec_command("ls -la /home/dfrobot/桌面/*.tgz")
print(o.read().decode("utf-8", "replace"))
print(e.read().decode("utf-8", "replace"))
sftp = ssh.open_sftp()
remote = "/home/dfrobot/桌面/mcp_sensors_voice_0.2.18.tgz"
local = pathlib.Path(r"c:\Users\13348\Desktop\mcp_sensors\scripts\mcp_sensors_voice_0.2.18.tgz")
sftp.get(remote, str(local))
print("saved", local, local.stat().st_size)
sftp.close()
ssh.close()
