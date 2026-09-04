#!/usr/bin/env python3
"""STA dashboard: same-LAN IP must answer HTTP on :80.

Classifies: no ICMP, TCP/80 closed, HTTP error, vs 200 page/API.

Usage:
  python scripts/diagnose_sta_web.py --offline
  python scripts/diagnose_sta_web.py 192.168.0.67

Exit 1 = red (this host cannot load the dashboard).
"""

from __future__ import annotations

import re
import socket
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "main" / "web" / "web_server.c"
APP = ROOT / "main" / "app" / "mcp_sensors.c"
DEFAULT_IP = "192.168.0.67"


def count_uri_slots() -> tuple[int, int]:
    text = WEB.read_text(encoding="utf-8")
    max_h = int(re.search(r"config\.max_uri_handlers\s*=\s*(\d+)", text).group(1))
    api = len(re.findall(r"httpd_register_uri_handler\(s_server,\s*&", text))
    return api, max_h


def ping(ip: str, timeout_s: float = 2.0) -> bool:
    try:
        param = "-n" if sys.platform == "win32" else "-c"
        import subprocess

        r = subprocess.run(
            ["ping", param, "1", "-w", "2000" if sys.platform == "win32" else "2", ip],
            capture_output=True,
            text=True,
            timeout=timeout_s + 3,
        )
        return r.returncode == 0
    except OSError:
        return False


def lan_bind_ip() -> str | None:
    """Prefer a 192.168/10.x NIC, never the Mihomo 198.18 TUN."""
    try:
        import subprocess

        out = subprocess.check_output(["ipconfig"], text=True, errors="replace")
    except OSError:
        return None
    current_ip = None
    skip = False
    for line in out.splitlines():
        s = line.strip()
        if not s:
            continue
        if "适配器" in s or s.startswith("adapter") or s.endswith(":"):
            skip = "Mihomo" in s or "Meta" in s or "Clash" in s
            current_ip = None
            continue
        if skip:
            continue
        if "IPv4" in s and ":" in s:
            current_ip = s.split(":")[-1].strip()
            if current_ip.startswith(("192.168.", "10.", "172.")):
                return current_ip
    return current_ip


def tcp80(ip: str, timeout_s: float = 3.0, bind_ip: str | None = None) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout_s)
    try:
        if bind_ip:
            s.bind((bind_ip, 0))
        s.connect((ip, 80))
        return "open"
    except socket.timeout:
        return "timeout"
    except OSError as e:
        return f"err:{e.errno}"
    finally:
        s.close()


def http_get(url: str, timeout_s: float = 5.0) -> tuple[int | None, str]:
    req = urllib.request.Request(url, headers={"User-Agent": "diagnose_sta_web"})
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as r:
            body = r.read(800)
            return r.status, body.decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, str(e.reason)
    except Exception as e:
        return None, type(e).__name__ + ":" + str(e)


def main() -> int:
    args = [a for a in sys.argv[1:] if a]
    offline = "--offline" in args
    args = [a for a in args if a != "--offline"]
    ip = args[0] if args else DEFAULT_IP
    used, cap = count_uri_slots()
    print(f"uri_handlers used~={used} max={cap}")
    if used > cap:
        print("FAIL handlers_full: static /* will not register, GET / 404")
        return 1
    print("OK  uri handler slots")
    if offline:
        return 0
    bind_ip = lan_bind_ip()
    print(f"bind_lan={bind_ip or '(default)'}")
    print(f"icmp ping {ip}:", "ok" if ping(ip) else "FAIL")
    t = tcp80(ip, bind_ip=bind_ip)
    print(f"tcp :80 {ip}: {t}")
    if t != "open":
        print("FAIL no_http_port (httpd down, wrong L2, AP isolation, or TUN fake-open)")
        return 1
    st, body = http_get(f"http://{ip}/")
    print(f"GET / -> {st} {body[:120]!r}")
    st2, body2 = http_get(f"http://{ip}/api/capture")
    print(f"GET /api/capture -> {st2} {body2[:160]!r}")
    if st == 200 and ("html" in body.lower() or "<!doctype" in body.lower()):
        print("PASS dashboard html")
        return 0
    if st2 == 200 and "time_ok" in body2:
        print("FAIL html_missing_api_ok (SPIFFS / static route)")
        return 1
    print("FAIL http_not_dashboard")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
