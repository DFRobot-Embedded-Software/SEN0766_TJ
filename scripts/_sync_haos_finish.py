# -*- coding: utf-8 -*-
from __future__ import annotations

import sys

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.1.193", username="dfrobot", password="123456", timeout=15)

cmds = r"""
set -e
APP=/home/dfrobot/桌面/haos_app
STACK=/home/dfrobot/桌面/server_stack
echo '==== stack ===='
ls -la "$STACK/mcp_gw/mcp_gw" 2>/dev/null | head -30 || echo 'no desktop server_stack'
# mirror critical files into sibling server_stack so sync_sources.sh stays consistent
if [[ -d "$STACK/mcp_gw/mcp_gw" ]]; then
  cp -a "$APP/mcp_gw/device_mqtt.py" "$STACK/mcp_gw/mcp_gw/device_mqtt.py"
  cp -a "$APP/mcp_gw/app.py" "$STACK/mcp_gw/mcp_gw/app.py"
  cp -a "$APP/mcp_gw/api.py" "$STACK/mcp_gw/mcp_gw/api.py"
  echo mirrored to server_stack
fi
# also check common addon build paths / homeassistant share
ls /mnt 2>/dev/null | head
ls /home/dfrobot/桌面 | head -30
# try talk to HAOS supervisor from 193
curl -sS --max-time 3 http://192.168.1.210:9080/health | head -c 500; echo
# check if ha CLI or docker can reach HA
which ha 2>/dev/null; docker ps 2>/dev/null | head -5 || true
# verify syntax of uploaded modules
python3 - <<'PY'
import ast
from pathlib import Path
app=Path('/home/dfrobot/桌面/haos_app/mcp_gw')
for name in ['device_mqtt.py','app.py','api.py']:
    t=(app/name).read_text(encoding='utf-8')
    ast.parse(t)
    print('syntax ok', name, len(t))
print('ensure_device_mqtt' in (app/'device_mqtt.py').read_text(encoding='utf-8'))
print('push_mqtt' in (app/'api.py').read_text(encoding='utf-8'))
print('/api/mqtt/push' in (app/'app.py').read_text(encoding='utf-8'))
PY
# patch config.yaml options defaults for this site
python3 - <<'PY'
from pathlib import Path
p=Path('/home/dfrobot/桌面/haos_app/config.yaml')
t=p.read_text(encoding='utf-8')
# set non-empty defaults under options: for this LAN (user can change in UI)
import re
def set_opt(text, key, value):
    # only replace within options: block roughly
    pat=rf'(options:\n(?:.*\n)*?  {key}: )([^\n]*)'
    def repl(m):
        return m.group(1)+value
    nt,n=re.subn(pat, repl, text, count=1)
    return nt if n else text
t2=set_opt(t, 'advertise_ip', '"192.168.1.210"')
t2=set_opt(t2, 'mqtt_host', '""')  # keep empty -> 127.0.0.1 connect
t2=set_opt(t2, 'mqtt_user', '"dfrobotcd"')
t2=set_opt(t2, 'mqtt_password', '"df123456"')
p.write_text(t2, encoding='utf-8')
for ln in t2.splitlines():
    if any(k in ln for k in ('version:', 'advertise_ip:', 'mqtt_user:', 'mqtt_password:', 'mqtt_host:')):
        if 'password' in ln:
            print(ln.split(':')[0]+': ***')
        else:
            print(ln)
PY
"""
_, o, e = ssh.exec_command(cmds, timeout=40)
sys.stdout.write(o.read().decode("utf-8", "replace"))
sys.stdout.write(e.read().decode("utf-8", "replace"))
ssh.close()
