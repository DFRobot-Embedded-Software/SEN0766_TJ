# -*- coding: utf-8 -*-
"""在 192.168.1.27 安装 Hermes、配置 MCP 聚合网关、启用 API Server。"""
from __future__ import annotations

import sys
import time

import paramiko
import yaml  # may not exist locally — use plain text merge if needed

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = "192.168.1.27"
USER = "haha"
PASSWORD = "123456"
REMOTE_BRIDGE_ENV = "/home/haha/\u684c\u9762/mcp-sensors-stack/voice-bridge/.env"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(HOST, username=USER, password=PASSWORD, timeout=20)


def run(cmd: str, timeout: int = 120) -> str:
    _i, o, e = ssh.exec_command(cmd, timeout=timeout)
    o.channel.settimeout(timeout)
    try:
        out = o.read().decode("utf-8", errors="replace")
    except Exception as ex:
        out = f"<stdout {ex}>"
    try:
        err = e.read().decode("utf-8", errors="replace")
    except Exception:
        err = ""
    return (out + err).strip()


print("=== precheck ===")
print(run("git --version; curl --version | head -1; dpkg -l xz-utils 2>/dev/null | tail -1 || true"))
print(run("which hermes; test -d ~/.hermes && ls ~/.hermes | head || echo 'no ~/.hermes'"))

# read bridge API key to align
bridge_env = run(f"cat '{REMOTE_BRIDGE_ENV}' 2>/dev/null || true")
api_key = "hermes-local-dev"
for line in bridge_env.splitlines():
    if line.startswith("HERMES_API_KEY=") and "=" in line:
        val = line.split("=", 1)[1].strip().strip('"').strip("'")
        if val:
            api_key = val
            break
print("using API key from bridge .env:", "(set)" if api_key else "(empty)")

print("\n=== install hermes (skip interactive setup / browser) ===")
# Long install — give plenty of time
install_out = run(
    "export PATH=\"$HOME/.local/bin:$PATH\"; "
    "curl -fsSL https://hermes-agent.nousresearch.com/install.sh | bash -s -- --skip-setup --skip-browser",
    timeout=900,
)
print(install_out[-4000:] if len(install_out) > 4000 else install_out)

print("\n=== verify hermes ===")
print(run("export PATH=\"$HOME/.local/bin:$PATH\"; which hermes; hermes --version 2>&1 | head -5"))

# Ensure PATH in bashrc
print(
    run(
        "grep -q '.local/bin' ~/.bashrc || echo 'export PATH=\"$HOME/.local/bin:$PATH\"' >> ~/.bashrc"
    )
)

# Write/merge ~/.hermes/.env for API server
print("\n=== configure API server .env ===")
print(
    run(
        f"""
mkdir -p ~/.hermes
touch ~/.hermes/.env
# upsert keys
python3 - <<'PY'
from pathlib import Path
p = Path.home() / ".hermes" / ".env"
text = p.read_text(encoding="utf-8") if p.exists() else ""
lines = [ln for ln in text.splitlines() if ln.strip() and not ln.strip().startswith("#")]
kv = {{}}
for ln in lines:
    if "=" in ln:
        k,v = ln.split("=",1)
        kv[k.strip()] = v.strip()
kv["API_SERVER_ENABLED"] = "true"
kv["API_SERVER_HOST"] = "127.0.0.1"
kv["API_SERVER_PORT"] = "8642"
kv["API_SERVER_KEY"] = {api_key!r}
# keep existing provider keys if any
out = "\\n".join(f"{{k}}={{v}}" for k,v in kv.items()) + "\\n"
p.write_text(out, encoding="utf-8")
print("wrote", p)
print("API_SERVER_ENABLED=true PORT=8642")
PY
"""
    )
)

# Write MCP into config.yaml
print("\n=== configure mcp_servers ===")
print(
    run(
        r"""
python3 - <<'PY'
from pathlib import Path
try:
    import yaml
except ImportError:
    import subprocess, sys
    subprocess.check_call([sys.executable, "-m", "pip", "install", "-q", "pyyaml"])
    import yaml

cfg_path = Path.home() / ".hermes" / "config.yaml"
data = {}
if cfg_path.exists():
    data = yaml.safe_load(cfg_path.read_text(encoding="utf-8")) or {}

mcp = data.setdefault("mcp_servers", {})
mcp["mcp_sensors_gw"] = {
    "url": "http://127.0.0.1:9080/mcp",
    "enabled": True,
    "connect_timeout": 30,
    "timeout": 120,
    "skip_preflight": True,
}
# optional gateway api_server block if supported
gw = data.setdefault("gateway", {})
api = gw.setdefault("api_server", {})
api["enabled"] = True
api["port"] = 8642
api["host"] = "127.0.0.1"
api["key"] = None  # from env

cfg_path.parent.mkdir(parents=True, exist_ok=True)
cfg_path.write_text(yaml.safe_dump(data, allow_unicode=True, sort_keys=False), encoding="utf-8")
print("wrote", cfg_path)
print(yaml.safe_dump({"mcp_servers": data.get("mcp_servers")}, allow_unicode=True))
PY
"""
    )
)

# Sync bridge HERMES_API_KEY if we generated default
print(
    run(
        f"""
python3 - <<'PY'
from pathlib import Path
p = Path("{REMOTE_BRIDGE_ENV}")
key = "{api_key}"
if not p.exists():
    print("no bridge env")
    raise SystemExit
text = p.read_text(encoding="utf-8")
lines = []
found_key = False
found_base = False
for ln in text.splitlines():
    if ln.startswith("HERMES_API_KEY="):
        lines.append("HERMES_API_KEY=" + key)
        found_key = True
    elif ln.startswith("HERMES_API_BASE="):
        lines.append("HERMES_API_BASE=http://127.0.0.1:8642")
        found_base = True
    else:
        lines.append(ln)
if not found_key:
    lines.append("HERMES_API_KEY=" + key)
if not found_base:
    lines.append("HERMES_API_BASE=http://127.0.0.1:8642")
p.write_text("\\n".join(lines) + "\\n", encoding="utf-8")
print("synced bridge .env HERMES_API_KEY/BASE")
PY
"""
    )
)

print("\n=== start hermes gateway ===")
print(run("pkill -f 'hermes gateway' 2>/dev/null || true; pkill -f 'hermes_cli' 2>/dev/null || true", timeout=10))
time.sleep(1)
chan = ssh.get_transport().open_session()
chan.exec_command(
    "export PATH=\"$HOME/.local/bin:$PATH\"; "
    "cd ~ && nohup hermes gateway >> ~/.hermes/gateway.log 2>&1 < /dev/null &"
)
time.sleep(0.5)
chan.close()
time.sleep(5)

print("procs:", run("pgrep -af hermes | head -20 || echo none"))
print("port 8642:", run("ss -lntp | grep 8642 || echo free"))
print("health:", run("curl -sS -m 3 http://127.0.0.1:8642/health || echo fail"))
print("log:", run("tail -n 40 ~/.hermes/gateway.log 2>/dev/null || echo no-log"))
print("mcp test:", run("export PATH=\"$HOME/.local/bin:$PATH\"; hermes mcp list 2>&1 | head -40"))
ssh.close()
print("DONE")
