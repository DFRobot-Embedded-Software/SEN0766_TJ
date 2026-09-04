# -*- coding: utf-8 -*-
from __future__ import annotations

import sys

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.1.193", username="dfrobot", password="123456", timeout=15)
cmd = r"""
python3 - <<'PY'
from pathlib import Path
p = Path('/home/dfrobot/桌面/haos_app/config.yaml')
lines = p.read_text(encoding='utf-8').splitlines()
out = []
in_options = False
in_schema = False
for ln in lines:
    s = ln.strip()
    if s.startswith('options:'):
        in_options, in_schema = True, False
        out.append(ln)
        continue
    if s.startswith('schema:'):
        in_options, in_schema = False, True
        out.append(ln)
        continue
    if in_options and not s.startswith('#') and ':' in s and ln.startswith('  ') and not ln.startswith('    '):
        key = s.split(':', 1)[0].strip()
        if key == 'advertise_ip':
            out.append('  advertise_ip: "192.168.1.210"')
            continue
        if key == 'mqtt_user':
            out.append('  mqtt_user: "dfrobotcd"')
            continue
        if key == 'mqtt_password':
            out.append('  mqtt_password: "df123456"')
            continue
        if key == 'mqtt_host':
            out.append('  mqtt_host: ""')
            continue
    out.append(ln)
p.write_text('\n'.join(out) + '\n', encoding='utf-8')
for ln in out:
    if any(k in ln for k in ('version:', 'advertise_ip:', 'mqtt_user:', 'mqtt_password:', 'mqtt_host:')) and 'str' not in ln and 'password?' not in ln:
        print(ln if 'password' not in ln else ln.split(':')[0] + ': ***')
PY
# package for local addon install
cd /home/dfrobot/桌面
rm -rf /tmp/mcp_sensors_voice_addon
mkdir -p /tmp/mcp_sensors_voice_addon
# HA local addon folder name from docs
cp -a haos_app/. /tmp/mcp_sensors_voice_addon/
# drop caches
find /tmp/mcp_sensors_voice_addon -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
cd /tmp && tar czf /home/dfrobot/桌面/mcp_sensors_voice_0.2.18.tgz mcp_sensors_voice_addon
ls -la /home/dfrobot/桌面/mcp_sensors_voice_0.2.18.tgz
echo '==== verify ensure ===='
grep -n ensure_device_mqtt /home/dfrobot/桌面/haos_app/mcp_gw/device_mqtt.py | head
grep -n 'api/mqtt/push' /home/dfrobot/桌面/haos_app/mcp_gw/app.py
"""
_, o, e = ssh.exec_command(cmd, timeout=60)
sys.stdout.write(o.read().decode("utf-8", "replace"))
sys.stdout.write(e.read().decode("utf-8", "replace"))
ssh.close()
