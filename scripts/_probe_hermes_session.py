# -*- coding: utf-8 -*-
"""Probe whether Hermes on 192.168.0.73 supports X-Hermes-Session-Id."""
import sys

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REMOTE = r"""
set +e
cd /home/dfrobot/桌面/hermes-voice-bridge 2>/dev/null || cd "$HOME/桌面/hermes-voice-bridge" || true
if [ -f .env ]; then set -a; . ./.env; set +a; fi
BASE="${HERMES_API_BASE:-http://127.0.0.1:8642}"
KEY="${HERMES_API_KEY:-}"
echo "HERMES_API_BASE=$BASE"
echo "HERMES_KEY_LEN=${#KEY}"

probe_get() {
  p="$1"
  echo "===== GET $p ====="
  if [ -n "$KEY" ]; then
    curl -sS -m 5 -D - -o /tmp/hermes_body.txt -H "Authorization: Bearer $KEY" "${BASE}${p}" | head -n 25
  else
    curl -sS -m 5 -D - -o /tmp/hermes_body.txt "${BASE}${p}" | head -n 25
  fi
  echo "BODY:"
  head -c 500 /tmp/hermes_body.txt; echo
}

probe_get /health
probe_get /v1/capabilities
probe_get /v1/models

echo "===== SESSION PROBE turn1 ====="
curl -sS -m 90 -D /tmp/hermes_hdr1.txt -o /tmp/hermes_body1.txt \
  "${BASE}/v1/chat/completions" \
  -H "Authorization: Bearer ${KEY}" \
  -H "Content-Type: application/json" \
  -d '{"messages":[{"role":"user","content":"只回复四个字：探测成功"}],"stream":false}'
echo "HDR1:"
head -n 40 /tmp/hermes_hdr1.txt
echo "BODY1:"
head -c 500 /tmp/hermes_body1.txt; echo
SID=$(awk 'BEGIN{IGNORECASE=1} /^X-Hermes-Session-Id:/{print $2}' /tmp/hermes_hdr1.txt | tr -d '\r')
echo "CAPTURED_SID=$SID"

if [ -n "$SID" ]; then
  echo "===== SESSION PROBE turn2 ====="
  curl -sS -m 90 -D /tmp/hermes_hdr2.txt -o /tmp/hermes_body2.txt \
    "${BASE}/v1/chat/completions" \
    -H "Authorization: Bearer ${KEY}" \
    -H "Content-Type: application/json" \
    -H "X-Hermes-Session-Id: ${SID}" \
    -d '{"messages":[{"role":"user","content":"我上一句让你回复什么？只说那四个字"}],"stream":false}'
  echo "HDR2:"
  head -n 40 /tmp/hermes_hdr2.txt
  echo "BODY2:"
  head -c 800 /tmp/hermes_body2.txt; echo
else
  echo "NO_SESSION_ID — trying code presence on disk"
fi

echo "===== CODE SEARCH ====="
python3 - <<'PY'
import os
hits = []
roots = [
    os.path.expanduser("~"),
    "/home/dfrobot",
]
for root0 in roots:
    for root, dirs, files in os.walk(root0):
        dirs[:] = [d for d in dirs if d not in {".git", "node_modules", "__pycache__", ".cache"}]
        if "api_server.py" in files and ("hermes" in root.lower() or "Nous" in root):
            hits.append(os.path.join(root, "api_server.py"))
        if len(hits) >= 8:
            break
    if len(hits) >= 8:
        break
print("candidates:", hits)
for p in hits[:5]:
    try:
        with open(p, "r", errors="replace") as f:
            txt = f.read()
        print(p)
        print("  has X-Hermes-Session-Id:", "X-Hermes-Session-Id" in txt)
        print("  has get_messages_as_conversation:", "get_messages_as_conversation" in txt)
    except Exception as e:
        print(p, e)
PY
"""

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect("192.168.0.73", username="dfrobot", password="123456", timeout=15)
_stdin, stdout, stderr = ssh.exec_command(REMOTE, timeout=180)
print(stdout.read().decode("utf-8", errors="replace"))
err = stderr.read().decode("utf-8", errors="replace")
if err.strip():
    print("STDERR:", err)
ssh.close()
