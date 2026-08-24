from __future__ import annotations

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
from .device_bridge import DeviceNotificationBridge
from .device_client import DeviceMcpClient
from .mdns_advertise import MdnsAdvertiser
from .mqtt_ha import HaMqttBridge
from .registry import DeviceRegistry
from .sessions import SessionManager

log = logging.getLogger("mcp_gw")


class AppState:
    def __init__(self) -> None:
        self.registry = DeviceRegistry(stale_s=config.STALE_S)
        self.client = DeviceMcpClient(timeout_s=config.DEVICE_TIMEOUT_S)
        self.sessions = SessionManager()
        self.bridge = DeviceNotificationBridge(self.registry, self.sessions)
        self.api = RegisterApi(self.registry, self.client)
        self.agg = AggregatorMcp(self.registry, self.client, self.sessions, self.bridge)
        self.mdns = MdnsAdvertiser()
        self.mqtt = HaMqttBridge(self.registry, self.client)
        self._refresh_task: asyncio.Task | None = None
        self._prev_resource_keys: dict[str, frozenset[str]] = {}


STATE = AppState()


async def _refresh_loop() -> None:
    while True:
        try:
            for rec in STATE.registry.list_all():
                try:
                    tools, resources = await STATE.client.fetch_catalog(rec.mcp_url)
                    prev = STATE._prev_resource_keys.get(rec.id, frozenset())
                    new_keys = frozenset(str(r.get("uri") or "") for r in resources if r.get("uri"))
                    STATE.registry.set_catalog(rec.id, tools=tools, resources=resources)
                    if prev and new_keys != prev:
                        await STATE.sessions.publish_resources_list_changed()
                    STATE._prev_resource_keys[rec.id] = new_keys
                except Exception as e:
                    STATE.registry.mark_error(rec.id, str(e))
                    log.warning("refresh failed id=%s err=%s", rec.id, e)
            # 同步 watcher（设备掉线等）
            await STATE.bridge.sync_from_sessions()
        except Exception:
            log.exception("refresh loop error")
        await asyncio.sleep(config.REFRESH_S)


@asynccontextmanager
async def lifespan(app: Starlette):
    log.info(
        "mcp_gw starting host=%s port=%s stale=%.0fs refresh=%.0fs token=%s mdns=%s mqtt=%s sse=yes",
        config.HOST,
        config.PORT,
        config.STALE_S,
        config.REFRESH_S,
        "yes" if config.TOKEN else "no",
        "yes" if config.MDNS_ENABLE else "no",
        "on" if config.MQTT_ENABLE else "off",
    )
    await STATE.mdns.start()
    await STATE.mqtt.start()
    STATE._refresh_task = asyncio.create_task(_refresh_loop())
    try:
        yield
    finally:
        if STATE._refresh_task:
            STATE._refresh_task.cancel()
            try:
                await STATE._refresh_task
            except asyncio.CancelledError:
                pass
        await STATE.mqtt.stop()
        await STATE.bridge.stop()
        await STATE.mdns.stop()
        await STATE.client.close()


async def health(_: Request) -> JSONResponse:
    online = STATE.registry.list_online()
    return JSONResponse(
        {
            "ok": True,
            "service": config.SERVER_NAME,
            "version": config.SERVER_VERSION,
            "devices_total": len(STATE.registry.list_all()),
            "devices_online": len(online),
            "mcp": "/mcp",
            "sse": True,
            "mdns": STATE.mdns.status(),
            "mqtt": STATE.mqtt.status(),
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
        Route("/api/devices/{id}/heartbeat", STATE.api.heartbeat, methods=["POST"]),
        Route("/api/devices/{id}/refresh", STATE.api.refresh_device, methods=["POST"]),
        Route("/api/devices/{id}", STATE.api.delete_device, methods=["DELETE"]),
        Route("/mcp", mcp_post, methods=["POST"]),
        Route("/mcp", mcp_get, methods=["GET"]),
    ]
    return Starlette(routes=routes, lifespan=lifespan)


app = create_app()
