# -*- coding: utf-8 -*-
from __future__ import annotations

import sys

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.1.193", username="dfrobot", password="123456", timeout=15)
cmd = r"""
set -e
APP=/home/dfrobot/桌面/haos_app
ls -la "$APP" | head -40
echo '==== mcp_gw ===='
ls -la "$APP/mcp_gw" | head -80
echo '==== device_mqtt ===='
ls -la "$APP/mcp_gw/device_mqtt.py" 2>/dev/null || echo MISSING
echo '==== config.yaml head ===='
head -60 "$APP/config.yaml"
echo '==== files mentioning push/mqtt ===='
grep -RIn --include='*.py' -E 'push_if_missing|device_mqtt|/api/mqtt' "$APP/mcp_gw" 2>/dev/null | head -40
echo '==== app.py lifespan/routes ===='
grep -nE 'lifespan|Route|coap_poll|mdns|mqtt|create_app' "$APP/mcp_gw/app.py" | head -60
"""
_, o, e = ssh.exec_command(cmd, timeout=40)
sys.stdout.write(o.read().decode("utf-8", "replace"))
sys.stdout.write(e.read().decode("utf-8", "replace"))
ssh.close()
