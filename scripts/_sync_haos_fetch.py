# -*- coding: utf-8 -*-
from __future__ import annotations

import sys

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.1.193", username="dfrobot", password="123456", timeout=15)
for rel in [
    "mcp_gw/device_mqtt.py",
    "mcp_gw/app.py",
    "mcp_gw/api.py",
    "mcp_gw/coap_poll.py",
    "mcp_gw/mdns_devices.py",
    "mcp_gw/config.py",
    "mcp_gw/mdns_advertise.py",
    "run.sh",
    "sync_sources.sh",
]:
    print(f"\n########## {rel} ##########")
    cmd = f"cat '/home/dfrobot/桌面/haos_app/{rel}'"
    _, o, e = ssh.exec_command(cmd, timeout=20)
    sys.stdout.write(o.read().decode("utf-8", "replace"))
    err = e.read().decode("utf-8", "replace")
    if err:
        sys.stdout.write(err)
ssh.close()
