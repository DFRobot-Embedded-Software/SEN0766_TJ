# -*- coding: utf-8 -*-
"""修复远端 .env CRLF，上传 bridge.py，重启 voice-bridge（同时覆盖 server_stack / mcp-sensors-stack）。"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = "192.168.1.27"
USER = "haha"
PASSWORD = "123456"
LOCAL = Path(__file__).resolve().parents[1] / "server_stack" / "voice-bridge" / "bridge.py"
STACKS = [
    "/home/haha/\u684c\u9762/server_stack",
    "/home/haha/\u684c\u9762/mcp-sensors-stack",
]

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(HOST, username=USER, password=PASSWORD, timeout=15)


def run(cmd: str, timeout: int = 60) -> str:
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


sftp = ssh.open_sftp()
for stack in STACKS:
    bridge = f"{stack}/voice-bridge"
    if run(f"test -d '{bridge}' && echo ok") != "ok":
        print("skip missing", bridge)
        continue
    sftp.put(str(LOCAL), f"{bridge}/bridge.py")
    print("put bridge.py →", bridge)
    # CRLF → LF for .env
    print(
        run(
            f"python3 - <<'PY'\n"
            f"from pathlib import Path\n"
            f"p = Path('{bridge}/.env')\n"
            f"if p.exists():\n"
            f"    raw = p.read_bytes().replace(b'\\r\\n', b'\\n').replace(b'\\r', b'\\n')\n"
            f"    p.write_bytes(raw)\n"
            f"    print('normalized', p, 'crlf_gone')\n"
            f"else:\n"
            f"    print('no .env', p)\n"
            f"PY"
        )
    )
sftp.close()

print(run("fuser -k 8765/tcp 2>/dev/null || true; pkill -9 -f 'python.*bridge.py' 2>/dev/null || true", timeout=15))
time.sleep(1)

# Prefer the path user is actually using (server_stack from log)
active = "/home/haha/\u684c\u9762/server_stack"
if run(f"test -d '{active}/voice-bridge' && echo ok") != "ok":
    active = "/home/haha/\u684c\u9762/mcp-sensors-stack"

bridge = f"{active}/voice-bridge"
logs = f"{active}/logs"
run(f"mkdir -p '{logs}'")
chan = ssh.get_transport().open_session()
chan.exec_command(
    f"cd '{bridge}' && set -a && "
    f"tmp=$(mktemp) && tr -d '\\r' < .env >\"$tmp\" && . \"$tmp\" && rm -f \"$tmp\" && set +a && "
    f"nohup ./venv/bin/python -u bridge.py >> '{logs}/bridge.log' 2>&1 < /dev/null &"
)
time.sleep(0.3)
chan.close()
time.sleep(2)
print("procs:", run("pgrep -af 'python.*bridge.py' || echo none"))
print("port:", run("ss -lntp | grep 8765 || echo free"))
print("key check:", run(
    "python3 - <<'PY'\n"
    "from pathlib import Path\n"
    "import glob\n"
    "for path in glob.glob('/proc/[0-9]*/cmdline'):\n"
    "  try: cmd=open(path,'rb').read().replace(b'\\x00',b' ').decode()\n"
    "  except Exception: continue\n"
    "  if 'bridge.py' not in cmd: continue\n"
    "  pid=path.split('/')[2]\n"
    "  for item in Path(f'/proc/{pid}/environ').read_bytes().split(b'\\x00'):\n"
    "    if item.startswith(b'DASHSCOPE_API_KEY='):\n"
    "      v=item.split(b'=',1)[1]\n"
    "      print(pid, 'len', len(v), 'ctrl', [b for b in v if b<32 or b==127])\n"
    "PY"
))
ssh.close()
