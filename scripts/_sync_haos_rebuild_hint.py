# -*- coding: utf-8 -*-
from __future__ import annotations

import sys

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.1.193", username="dfrobot", password="123456", timeout=15)
cmd = r"""
APP=/home/dfrobot/桌面/haos_app
python3 - <<'PY'
from pathlib import Path
import ast
app=Path('/home/dfrobot/桌面/haos_app')
for name in ['mcp_gw/device_mqtt.py','mcp_gw/app.py','mcp_gw/api.py']:
    t=(app/name).read_text(encoding='utf-8')
    ast.parse(t)
    print('ok', name)
cfg=(app/'config.yaml').read_text(encoding='utf-8')
for ln in cfg.splitlines():
    if any(k in ln for k in ('version:','advertise_ip:','mqtt_user:','mqtt_password:','mqtt_host:')):
        print('CFG', ln if 'password' not in ln else ln.split(':')[0]+': ***')
PY
echo '==== docs rebuild ===='
ls "$APP/docs"
grep -nE '重建|rebuild|安装|本地|addons' "$APP/docs"/*.md 2>/dev/null | head -40
echo '==== find HA share ===='
find /home/dfrobot /mnt /media -maxdepth 4 -iname '*addon*' 2>/dev/null | head -40
ls /home/dfrobot/桌面/haos_app/homeassistant 2>/dev/null
# try scp-like: can 193 ssh to HAOS?
ssh -o BatchMode=yes -o ConnectTimeout=3 -o StrictHostKeyChecking=no root@192.168.1.210 'echo HAOS_OK; ls /addons 2>/dev/null | head' 2>&1 | head -20
"""
_, o, e = ssh.exec_command(cmd, timeout=45)
sys.stdout.write(o.read().decode("utf-8", "replace"))
sys.stdout.write(e.read().decode("utf-8", "replace"))
ssh.close()
