# -*- coding: utf-8 -*-
"""Upload fixed haos_app mqtt push + routes to 192.168.1.193 and bump addon version."""
from __future__ import annotations

import sys
from pathlib import Path

import paramiko

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = "192.168.1.193"
USER = "dfrobot"
PASSWORD = "123456"
APP = "/home/dfrobot/桌面/haos_app"

DEVICE_MQTT = r'''"""MQTT broker provisioning for devices (haos_app).

Primary: mDNS _mcp-gw TXT mqtt= / mqtt_user / mqtt_pass.
Fallback: HTTP POST /api/wifi action=mqtt.

Broker URI never uses the device IP. Prefer MQTT_ADVERTISE_HOST / ADVERTISE_IP.
When mqtt_broker exists but mqtt_connected=false (missing auth), re-push creds.
"""

from __future__ import annotations

import logging
from typing import Any

import httpx

from . import config
from .mdns_advertise import guess_lan_ip

log = logging.getLogger("mcp_gw.device_mqtt")

_pushed_fp: dict[str, str] = {}
_last_config_fp: str = ""


def _is_loopback(host: str) -> bool:
    h = (host or "").strip().lower()
    if h.startswith("mqtt://"):
        h = h[7:]
    elif h.startswith("mqtts://"):
        h = h[8:]
    h = h.split("/")[0]
    if h.startswith("["):
        h = h.split("]")[0].lstrip("[")
    else:
        h = h.split(":")[0]
    return (not h) or h in ("127.0.0.1", "localhost", "0.0.0.0", "::1") or h.startswith("127.")


def _advertise_host() -> str:
    for name in ("MQTT_ADVERTISE_HOST", "ADVERTISE_IP"):
        v = (getattr(config, name, "") or "").strip()
        if v and not _is_loopback(v):
            return v
    return (guess_lan_ip() or "").strip()


def broker_uri_for_device(*_args: Any, **_kwargs: Any) -> str:
    """LAN broker for devices. Extra args ignored (compat with old lan_ip signature)."""
    raw = (config.MQTT_HOST or "").strip()
    advertise = _advertise_host()
    if raw and not _is_loopback(raw):
        host = raw
        if host.startswith("mqtt://"):
            host = host[7:].split("/")[0].split(":")[0]
        elif host.startswith("mqtts://"):
            host = host[8:].split("/")[0].split(":")[0]
    else:
        host = advertise
    if not host or _is_loopback(host):
        return ""
    return f"mqtt://{host}:{int(config.MQTT_PORT or 1883)}"


def config_fingerprint() -> str:
    uri = broker_uri_for_device()
    user = (config.MQTT_USER or "").strip()
    password = (config.MQTT_PASSWORD or "").strip()
    return f"{uri}\n{user}\n{password}"


def invalidate_push_cache() -> None:
    global _last_config_fp
    _pushed_fp.clear()
    _last_config_fp = ""
    log.info("mqtt push cache cleared (will re-push on next cycle)")


def consume_config_change() -> bool:
    global _last_config_fp
    fp = config_fingerprint()
    if fp == _last_config_fp:
        return False
    if _last_config_fp:
        log.info("mqtt config changed -> force re-push to devices")
        _pushed_fp.clear()
    _last_config_fp = fp
    return True


async def push_if_missing(device_ip: str, *, force: bool = False) -> bool:
    uri = broker_uri_for_device()
    if not uri:
        log.warning("skip mqtt push: invalid broker host (set advertise_ip / MQTT_ADVERTISE_HOST)")
        return False
    user = (config.MQTT_USER or "").strip()
    password = (config.MQTT_PASSWORD or "").strip()
    fp = config_fingerprint()
    if not user:
        log.warning(
            "mqtt push without MQTT_USER: LAN devices often get Not authorized; "
            "set plugin mqtt_user/password"
        )
    wifi_url = f"http://{device_ip}/api/wifi"
    try:
        async with httpx.AsyncClient(timeout=4.0) as client:
            if not force and _pushed_fp.get(device_ip) == fp:
                r = await client.get(wifi_url)
                r.raise_for_status()
                info: Any = r.json()
                if isinstance(info, dict) and bool(info.get("mqtt_connected")):
                    return True
            elif not force:
                r = await client.get(wifi_url)
                r.raise_for_status()
                info = r.json() if r.content else {}
                if isinstance(info, dict):
                    existing = str(info.get("mqtt_broker") or "").strip()
                    connected = bool(info.get("mqtt_connected"))
                    dev_user = str(info.get("mqtt_user") or "").strip()
                    same_broker = existing == uri
                    same_user = (not user) or (not dev_user) or (dev_user == user)
                    if same_broker and connected and same_user:
                        _pushed_fp[device_ip] = fp
                        return True
                    if existing and not connected and not user:
                        return False

            body = {
                "action": "mqtt",
                "broker": uri,
                "user": user,
                "password": password,
            }
            w = await client.post(wifi_url, json=body)
            txt = (w.text or "")[:180]
            if w.status_code >= 400:
                log.warning("mqtt push failed %s status=%s body=%s", device_ip, w.status_code, txt)
                return False
            _pushed_fp[device_ip] = fp
            log.info(
                "mqtt pushed %s user=%s -> %s%s",
                uri,
                user or "(none)",
                device_ip,
                " (force)" if force else "",
            )
            return True
    except Exception as e:
        log.debug("mqtt push skip %s: %s", device_ip, e)
        return False


async def ensure_device_mqtt(device_ip: str, *, force: bool = False) -> bool:
    """Alias used by coap_poll."""
    return await push_if_missing(device_ip, force=force)


async def push_all(device_ips: list[str], *, force: bool = False) -> dict[str, bool]:
    out: dict[str, bool] = {}
    if force:
        invalidate_push_cache()
        global _last_config_fp
        _last_config_fp = config_fingerprint()
    for ip in device_ips:
        ip = (ip or "").strip()
        if not ip:
            continue
        out[ip] = await push_if_missing(ip, force=force)
    return out
'''

APP_PY = r'''from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager

from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import JSONResponse, Response
from starlette.routing import Route

from . import config
from .aggregator import AggregatorMcp
from .api import RegisterApi
from .coap_client import DeviceCoapClient
from .coap_poll import CoapSnapshotPoller
from .device_mqtt import broker_uri_for_device, consume_config_change, push_if_missing
from .mdns_advertise import MdnsAdvertiser
from .mdns_devices import MdnsDeviceBrowser
from .mqtt_ha import HaMqttBridge
from .registry import DeviceRegistry
from .sessions import SessionManager

log = logging.getLogger("mcp_gw")


async def _mqtt_push_loop() -> None:
    """Periodic HTTP MQTT fallback; force when MQTT options fingerprint changes."""
    while True:
        try:
            force = consume_config_change()
            for rec in STATE.registry.list_all():
                host = (rec.coap_host or "").strip()
                if host:
                    await push_if_missing(host, force=force)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            log.debug("mqtt push loop: %s", e)
        await asyncio.sleep(30)


class AppState:
    def __init__(self) -> None:
        self.registry = DeviceRegistry(stale_s=config.STALE_S)
        self.client = DeviceCoapClient(timeout_s=config.DEVICE_TIMEOUT_S)
        self.sessions = SessionManager()
        self.api = RegisterApi(self.registry)
        self.agg = AggregatorMcp(self.registry, self.client, self.sessions)
        self.mdns = MdnsAdvertiser()
        self.mdns_dev = MdnsDeviceBrowser(self.registry)
        self.mqtt = HaMqttBridge(self.registry, self.sessions)
        self.coap_poll = CoapSnapshotPoller(self.registry, self.client, self.mqtt)


STATE = AppState()


@asynccontextmanager
async def lifespan(app: Starlette):
    log.info(
        "mcp_gw starting as device adapter host=%s port=%s mdns=%s mqtt=%s "
        "(/mcp is legacy; agents should use Home Assistant /api/mcp)",
        config.HOST,
        config.PORT,
        "yes" if config.MDNS_ENABLE else "no",
        "on" if config.MQTT_ENABLE else "off",
    )
    await STATE.mdns.start()
    await STATE.mdns_dev.start()
    await STATE.mqtt.start()
    await STATE.coap_poll.start()
    push_task = asyncio.create_task(_mqtt_push_loop(), name="mqtt-push-loop")
    try:
        yield
    finally:
        push_task.cancel()
        try:
            await push_task
        except asyncio.CancelledError:
            pass
        await STATE.coap_poll.stop()
        await STATE.mqtt.stop()
        await STATE.mdns_dev.stop()
        await STATE.mdns.stop()


async def health(_: Request) -> JSONResponse:
    online = STATE.registry.list_online()
    return JSONResponse(
        {
            "ok": True,
            "service": config.SERVER_NAME,
            "version": config.SERVER_VERSION,
            "devices_total": len(STATE.registry.list_all()),
            "devices_online": len(online),
            "role": "device-adapter",
            "mcp": "/mcp",
            "mcp_note": "legacy debug; agents use Home Assistant /api/mcp",
            "device_protocol": "coap+mqtt-event",
            "sse": True,
            "mdns": STATE.mdns.status(),
            "mqtt": {
                **STATE.mqtt.status(),
                "device_broker": broker_uri_for_device(),
                "device_auth_configured": bool((config.MQTT_USER or "").strip()),
            },
        }
    )


async def mcp_post(request: Request) -> Response:
    return await STATE.agg.handle_post(request)


async def mcp_get(request: Request) -> Response:
    return await STATE.agg.handle_get(request)


def create_app() -> Starlette:
    routes = [
        Route("/health", health, methods=["GET"]),
        Route("/api/devices", STATE.api.list_devices, methods=["GET"]),
        Route("/api/devices/register", STATE.api.register, methods=["POST"]),
        Route("/api/devices/{id}", STATE.api.delete_device, methods=["DELETE"]),
        Route("/api/mqtt/push", STATE.api.push_mqtt, methods=["POST"]),
        Route("/mcp", mcp_post, methods=["POST"]),
        Route("/mcp", mcp_get, methods=["GET"]),
    ]
    return Starlette(routes=routes, lifespan=lifespan)


app = create_app()
'''

API_PY = r'''from __future__ import annotations

import asyncio
import logging

from starlette.requests import Request
from starlette.responses import JSONResponse

from . import config
from .device_mqtt import broker_uri_for_device, config_fingerprint, push_all, push_if_missing
from .registry import DeviceRegistry

log = logging.getLogger("mcp_gw.api")


def _unauthorized() -> JSONResponse:
    return JSONResponse({"ok": False, "error": "unauthorized"}, status_code=401)


def _check_token(request: Request) -> bool:
    if not config.TOKEN:
        return True
    got = request.headers.get("x-mcp-gw-token") or ""
    q = request.query_params.get("token") or ""
    return got == config.TOKEN or q == config.TOKEN


class RegisterApi:
    """Optional manual register; primary path is browse _mcp-sensors._udp."""

    def __init__(self, registry: DeviceRegistry) -> None:
        self.registry = registry

    async def register(self, request: Request) -> JSONResponse:
        if not _check_token(request):
            return _unauthorized()
        try:
            body = await request.json()
        except Exception:
            return JSONResponse({"ok": False, "error": "invalid json"}, status_code=400)
        if not isinstance(body, dict):
            return JSONResponse({"ok": False, "error": "object required"}, status_code=400)

        device_id = str(body.get("id") or "").strip()
        name = str(body.get("name") or "").strip()
        fw = str(body.get("fw") or "").strip()
        base_url = str(body.get("base_url") or "").strip()
        ip = str(body.get("ip") or "").strip()
        port = int(body.get("port") or body.get("coap_port") or 5683)
        if not base_url:
            if not ip:
                return JSONResponse({"ok": False, "error": "ip or base_url required"}, status_code=400)
            base_url = f"http://{ip}"
        if not ip:
            ip = base_url.replace("http://", "").replace("https://", "").split("/")[0].split(":")[0]
        try:
            rec = self.registry.upsert(
                device_id=device_id,
                base_url=base_url,
                name=name,
                fw=fw,
                coap_host=ip,
                coap_port=port,
            )
        except ValueError as e:
            return JSONResponse({"ok": False, "error": str(e)}, status_code=400)
        log.info("registered device id=%s coap=%s", rec.id, f"{rec.coap_host}:{rec.coap_port}")
        if rec.coap_host:
            asyncio.create_task(push_if_missing(rec.coap_host), name=f"mqtt-push-reg-{rec.id}")
        return JSONResponse({"ok": True, "device": self.registry.to_public(rec)})

    async def list_devices(self, request: Request) -> JSONResponse:
        if not _check_token(request):
            return _unauthorized()
        return JSONResponse(
            {"ok": True, "devices": [self.registry.to_public(r) for r in self.registry.list_all()]}
        )

    async def delete_device(self, request: Request) -> JSONResponse:
        if not _check_token(request):
            return _unauthorized()
        device_id = (request.path_params.get("id") or "").strip()
        ok = self.registry.remove(device_id)
        return JSONResponse({"ok": ok}, status_code=200 if ok else 404)

    async def push_mqtt(self, request: Request) -> JSONResponse:
        """Force-push broker+creds after user changed MQTT options.

        POST /api/mqtt/push  body: {"force": true, "id": "f20e08"}
        """
        if not _check_token(request):
            return _unauthorized()
        force = True
        only_id = ""
        try:
            body = await request.json()
            if isinstance(body, dict):
                if "force" in body:
                    force = bool(body.get("force"))
                only_id = str(body.get("id") or body.get("device_id") or "").strip().lower()
        except Exception:
            pass

        recs = self.registry.list_all()
        if only_id:
            recs = [r for r in recs if r.id == only_id]
            if not recs:
                return JSONResponse({"ok": False, "error": "device not found"}, status_code=404)

        ips = [(r.coap_host or "").strip() for r in recs if (r.coap_host or "").strip()]
        results = await push_all(ips, force=force)
        ok_n = sum(1 for v in results.values() if v)
        log.info("mqtt push api force=%s ok=%s/%s uri=%s", force, ok_n, len(results), broker_uri_for_device())
        return JSONResponse(
            {
                "ok": ok_n == len(results) and len(results) > 0,
                "broker": broker_uri_for_device(),
                "user": (config.MQTT_USER or "").strip() or None,
                "fingerprint": config_fingerprint().split("\n")[0],
                "pushed": results,
                "ok_count": ok_n,
                "total": len(results),
            }
        )
'''


def put(sftp: paramiko.SFTPClient, remote: str, text: str) -> None:
    bak = remote + ".bak_sync"
    try:
        sftp.stat(remote)
        try:
            sftp.remove(bak)
        except OSError:
            pass
        sftp.rename(remote, bak)
    except OSError:
        pass
    with sftp.file(remote, "w") as f:
        f.write(text)
    print(f"wrote {remote} ({len(text)} bytes)")


def main() -> None:
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(HOST, username=USER, password=PASSWORD, timeout=20)
    sftp = ssh.open_sftp()
    put(sftp, f"{APP}/mcp_gw/device_mqtt.py", DEVICE_MQTT)
    put(sftp, f"{APP}/mcp_gw/app.py", APP_PY)
    put(sftp, f"{APP}/mcp_gw/api.py", API_PY)

    # bump version + default advertise_ip hint in config.yaml
    _, o, _ = ssh.exec_command(
        f"python3 - <<'PY'\n"
        f"from pathlib import Path\n"
        f"p=Path('{APP}/config.yaml')\n"
        f"t=p.read_text(encoding='utf-8')\n"
        f"t2=t.replace('version: \"0.2.17\"', 'version: \"0.2.18\"')\n"
        f"t2=t2.replace('version: \"0.2.18\"', 'version: \"0.2.18\"')\n"
        f"if 'version: \"0.2.18\"' not in t2:\n"
        f"    import re\n"
        f"    t2=re.sub(r'version:\\s*\"[^\"]+\"', 'version: \"0.2.18\"', t, count=1)\n"
        f"p.write_text(t2, encoding='utf-8')\n"
        f"print('config.yaml', [ln for ln in t2.splitlines() if ln.startswith('version')][:1])\n"
        f"PY"
    )
    print(o.read().decode())

    # ensure MQTT_ADVERTISE_HOST in config.py if missing
    _, o, e = ssh.exec_command(
        f"grep -n MQTT_ADVERTISE_HOST '{APP}/mcp_gw/config.py' || true"
    )
    print("MQTT_ADVERTISE_HOST in config.py:", o.read().decode().strip() or "MISSING")

    _, o, e = ssh.exec_command(
        f"python3 - <<'PY'\n"
        f"from pathlib import Path\n"
        f"p=Path('{APP}/mcp_gw/config.py')\n"
        f"t=p.read_text(encoding='utf-8')\n"
        f"if 'MQTT_ADVERTISE_HOST' not in t:\n"
        f"    needle='MQTT_ENABLE = bool(MQTT_HOST)'\n"
        f"    add=\"MQTT_ADVERTISE_HOST = (os.environ.get('MQTT_ADVERTISE_HOST') or '').strip()\\n\"\n"
        f"    if needle in t:\n"
        f"        t=t.replace(needle, add+needle, 1)\n"
        f"        p.write_text(t, encoding='utf-8')\n"
        f"        print('added MQTT_ADVERTISE_HOST')\n"
        f"    else:\n"
        f"        p.write_text(t.rstrip()+\"\\n\"+add, encoding='utf-8')\n"
        f"        print('appended MQTT_ADVERTISE_HOST')\n"
        f"else:\n"
        f"    print('MQTT_ADVERTISE_HOST already present')\n"
        f"PY"
    )
    print(o.read().decode(), e.read().decode())

    # patch mdns_devices push call to allow force=False explicitly (already ok)
    _, o, _ = ssh.exec_command(
        f"grep -n push_if_missing '{APP}/mcp_gw/mdns_devices.py'"
    )
    print("mdns_devices:", o.read().decode())

    # sync_sources / how to rebuild
    _, o, _ = ssh.exec_command(f"cat '{APP}/sync_sources.sh'; echo '===='; head -5 '{APP}/README.md'")
    print(o.read().decode())

    sftp.close()
    ssh.close()
    print("DONE sync to", APP)


if __name__ == "__main__":
    main()
