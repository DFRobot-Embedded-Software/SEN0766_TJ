# -*- coding: utf-8 -*-
import paramiko, sys
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.1.193", username="dfrobot", password="123456", timeout=15)
cmd = r"""
echo '=== ping 210 ==='
ping -c 1 -W 2 192.168.1.210 2>/dev/null || true
echo '=== ssh/docker on 210 from 193? ==='
ssh -o BatchMode=yes -o ConnectTimeout=3 dfrobot@192.168.1.210 'hostname; docker ps --format "table {{.Names}}\t{{.Ports}}\t{{.Status}}"' 2>&1 | head -20
echo '=== try common HA docker ports on 210 ==='
for p in 8123 8124 8125 4357; do
  timeout 2 bash -c "echo >/dev/tcp/192.168.1.210/$p" 2>/dev/null && echo "port $p OPEN" || echo "port $p closed"
done
echo '=== mqtt test from 193 ==='
timeout 3 bash -c "echo >/dev/tcp/192.168.1.210/1883" 2>/dev/null && echo mqtt1883 OPEN || echo mqtt1883 closed
"""
_, o, e = ssh.exec_command(cmd, timeout=45)
sys.stdout.write(o.read().decode("utf-8", "replace"))
sys.stdout.write(e.read().decode("utf-8", "replace"))
ssh.close()
