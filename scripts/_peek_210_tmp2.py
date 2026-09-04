# -*- coding: utf-8 -*-
import paramiko, sys
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.1.193", username="dfrobot", password="123456", timeout=15)
cmd = r"""
echo '=== curl 210 ports ==='
for url in \
  'http://192.168.1.210:4357/' \
  'http://192.168.1.210:4357/info' \
  'http://192.168.1.210:8123/' \
  'http://192.168.1.210:8124/' \
  'http://192.168.1.210:8125/' \
  'http://192.168.1.210:8126/' \
  'http://192.168.1.210:8127/' \
  'http://192.168.1.210:8128/' \
  'http://192.168.1.210:8129/' \
  'http://192.168.1.210:8130/'; do
  code=$(curl -s -o /tmp/curlout -w '%{http_code}' --max-time 2 "$url" 2>/dev/null || echo err)
  head=$(head -c 80 /tmp/curlout 2>/dev/null | tr '\n' ' ')
  echo "$url -> $code $head"
done
echo '=== docker on 193 ==='
docker ps --format 'table {{.Names}}\t{{.Ports}}\t{{.Status}}' 2>/dev/null | head -10 || echo no docker on 193
echo '=== ha cli on 193 ==='
which ha 2>/dev/null; ha supervisor info 2>/dev/null | head -5 || true
"""
_, o, e = ssh.exec_command(cmd, timeout=60)
sys.stdout.write(o.read().decode("utf-8", "replace"))
ssh.close()
