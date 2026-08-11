# -*- coding: utf-8 -*-
"""重启远端 mcp_gw，并在远端跑 smoke_test（resources）。"""
from __future__ import annotations

import sys
import time

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = "192.168.1.27"
USER = "haha"
PASSWORD = "123456"
STACKS = [
    "/home/haha/\u684c\u9762/mcp-sensors-stack",
    "/home/haha/\u684c\u9762/server_stack",
]

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(HOST, username=USER, password=PASSWORD, timeout=15)


def run(cmd: str, timeout: int = 120) -> str:
    _i, o, e = ssh.exec_command(cmd, timeout=timeout)
    o.channel.settimeout(timeout)
    try:
        out = o.read().decode("utf-8", errors="replace")
    except Exception as ex:
        out = str(ex)
    try:
        err = e.read().decode("utf-8", errors="replace")
    except Exception:
        err = ""
    return (out + err).strip()


# Prefer mcp-sensors-stack as canonical; sync mcp_gw tree into server_stack if present
src = STACKS[0]
dst = STACKS[1]
print(
    run(
        f"test -d '{dst}/mcp_gw' && "
        f"rsync -a --delete --exclude venv --exclude .env '{src}/mcp_gw/' '{dst}/mcp_gw/' && "
        f"echo synced_to_server_stack || echo no_server_stack"
    )
)

active = src if run(f"test -d '{src}/mcp_gw' && echo ok") == "ok" else dst
print("active", active)

print(run(f"cd '{active}' && ./stop_services.sh || true", timeout=30))
print(
    run(
        "fuser -k 9080/tcp 2>/dev/null || true; "
        "pkill -9 -f 'python.*-m mcp_gw' 2>/dev/null || true",
        timeout=15,
    )
)
time.sleep(1)
print(run(f"cd '{active}' && ./start_services.sh", timeout=180))

print("=== smoke ===")
print(
    run(
        f"cd '{active}/mcp_gw' && "
        f"./venv/bin/python smoke_test.py",
        timeout=90,
    )
)

print("=== live resources/list (if device online) ===")
print(
    run(
        "python3 - <<'PY'\n"
        "import json, urllib.request\n"
        "devs=json.load(urllib.request.urlopen('http://127.0.0.1:9080/api/devices', timeout=5))\n"
        "print('devices', json.dumps(devs, ensure_ascii=False)[:800])\n"
        "req=urllib.request.Request('http://127.0.0.1:9080/mcp', data=json.dumps({\n"
        "  'jsonrpc':'2.0','id':1,'method':'initialize',\n"
        "  'params':{'protocolVersion':'2025-03-26','capabilities':{},'clientInfo':{'name':'chk','version':'0'}}\n"
        "}).encode(), headers={'Content-Type':'application/json','Accept':'application/json'})\n"
        "r=urllib.request.urlopen(req, timeout=10)\n"
        "sid=r.headers.get('MCP-Session-Id') or r.headers.get('mcp-session-id')\n"
        "print('init caps', json.loads(r.read().decode()).get('result',{}).get('capabilities'))\n"
        "req2=urllib.request.Request('http://127.0.0.1:9080/mcp', data=json.dumps({\n"
        "  'jsonrpc':'2.0','id':2,'method':'resources/list','params':{}\n"
        "}).encode(), headers={'Content-Type':'application/json','Accept':'application/json',\n"
        "  **({'MCP-Session-Id':sid} if sid else {})})\n"
        "r2=urllib.request.urlopen(req2, timeout=10)\n"
        "res=json.loads(r2.read().decode())\n"
        "uris=[x.get('uri') for x in (res.get('result') or {}).get('resources') or []]\n"
        "print('resource uris', uris)\n"
        "PY"
    )
)
ssh.close()
