# -*- coding: utf-8 -*-
"""把整个 server_stack 同步到服务器桌面 mcp-sensors-stack/（方便移植）。"""
from __future__ import annotations

import os
import stat
import sys
from pathlib import Path

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = os.environ.get("STACK_HOST", "192.168.1.193")
USER = os.environ.get("STACK_USER", "dfrobot")
PASSWORD = os.environ.get("STACK_PASS", "123456")
REMOTE_ROOT = os.environ.get("STACK_REMOTE", "/home/dfrobot/\u684c\u9762/server_stack")
LOCAL_ROOT = Path(__file__).resolve().parents[1] / "server_stack"

SKIP_DIRS = {"venv", "__pycache__", "logs", ".git"}
SKIP_NAMES = {".env"}  # 保留远端已有密钥；首次用 example


def should_skip(path: Path, root: Path) -> bool:
    rel = path.relative_to(root)
    for part in rel.parts:
        if part in SKIP_DIRS:
            return True
    if path.name in SKIP_NAMES:
        return True
    return False


ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(HOST, username=USER, password=PASSWORD, timeout=15)


def run(cmd: str, timeout: int = 60) -> str:
    _i, o, e = ssh.exec_command(cmd, timeout=timeout)
    o.channel.settimeout(timeout)
    try:
        return (o.read() + e.read()).decode("utf-8", errors="replace").strip()
    except Exception as ex:
        return str(ex)


print(run(f"mkdir -p '{REMOTE_ROOT}/mcp_gw' '{REMOTE_ROOT}/voice-bridge' '{REMOTE_ROOT}/logs'"))
sftp = ssh.open_sftp()

uploaded = 0
for path in LOCAL_ROOT.rglob("*"):
    if not path.is_file():
        continue
    if should_skip(path, LOCAL_ROOT):
        continue
    rel = path.relative_to(LOCAL_ROOT).as_posix()
    remote = f"{REMOTE_ROOT}/{rel}"
    # ensure parent
    parent = "/".join(remote.split("/")[:-1])
    run(f"mkdir -p '{parent}'", timeout=15)
    sftp.put(str(path), remote)
    if path.suffix == ".sh":
        sftp.chmod(remote, stat.S_IRWXU | stat.S_IRGRP | stat.S_IXGRP | stat.S_IROTH | stat.S_IXOTH)
    uploaded += 1
    print("put", rel)

# 远端若无 .env，从 example 生成
print(
    run(
        f"test -f '{REMOTE_ROOT}/mcp_gw/.env' || "
        f"cp '{REMOTE_ROOT}/mcp_gw/.env.example' '{REMOTE_ROOT}/mcp_gw/.env'"
    )
)
print(
    run(
        f"test -f '{REMOTE_ROOT}/voice-bridge/.env' || "
        f"(test -f /home/dfrobot/\u684c\u9762/hermes-voice-bridge/.env && "
        f"cp /home/dfrobot/\u684c\u9762/hermes-voice-bridge/.env '{REMOTE_ROOT}/voice-bridge/.env' || "
        f"cp '{REMOTE_ROOT}/voice-bridge/.env.example' '{REMOTE_ROOT}/voice-bridge/.env')"
    )
)
# 若旧桌面目录有 .env，优先合并过来
print(
    run(
        f"test -f /home/dfrobot/\u684c\u9762/mcp-sensors-gw/.env && "
        f"cp -n /home/dfrobot/\u684c\u9762/mcp-sensors-gw/.env '{REMOTE_ROOT}/mcp_gw/.env' || true"
    )
)

sftp.close()

# 确保 ADVERTISE_IP
print(
    run(
        f"python3 - <<'PY'\n"
        f"from pathlib import Path\n"
        f"p = Path('{REMOTE_ROOT}/mcp_gw/.env')\n"
        f"text = p.read_text(encoding='utf-8') if p.exists() else ''\n"
        f"ip = {HOST!r}\n"
        f"lines = [ln for ln in text.splitlines() if not ln.startswith('MCP_GW_ADVERTISE_IP=')]\n"
        f"lines.append(f'MCP_GW_ADVERTISE_IP={{ip}}')\n"
        f"p.write_text('\\n'.join(lines)+'\\n', encoding='utf-8')\n"
        f"print('advertise', ip)\n"
        f"PY"
    )
)

print(f"uploaded {uploaded} files → {REMOTE_ROOT}")
print("=== restart mcp_gw ===")
print(
    run(
        "fuser -k 9080/tcp 2>/dev/null || true; "
        "pkill -9 -f 'python.*-m mcp_gw' 2>/dev/null || true",
        timeout=15,
    )
)
print(run(f"cd '{REMOTE_ROOT}' && chmod +x *.sh && ./start_services.sh", timeout=180))
print("on server: cd ~/桌面/server_stack && ./status_services.sh")
ssh.close()
