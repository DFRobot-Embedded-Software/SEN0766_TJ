from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager

from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import JSONResponse
from starlette.routing import Route

from . import config
from .aggregator import AggregatorMcp
from .api import RegisterApi
from .device_client import DeviceMcpClient
from .mdns_advertise import MdnsAdvertiser
from .registry import DeviceRegistry

log = logging.getLogger("mcp_gw")


class AppState:
    def __init__(self) -> None:
        self.registry = DeviceRegistry(stale_s=config.STALE_S)
        self.client = DeviceMcpClient(timeout_s=config.DEVICE_TIMEOUT_S)
        self.api = RegisterApi(self.registry, self.client)
        self.agg = AggregatorMcp(self.registry, self.client)
        self.mdns = MdnsAdvertiser()
        self._refresh_task: asyncio.Task | None = None


STATE = AppState()


async def _refresh_loop() -> None:
    while True:
        try:
            for rec in STATE.registry.list_all():
                try:
                    tools = await STATE.client.list_tools(rec.mcp_url)
                    STATE.registry.set_tools(rec.id, tools)
                except Exception as e:
                    STATE.registry.mark_error(rec.id, str(e))
                    log.warning("refresh failed id=%s err=%s", rec.id, e)
        except Exception:
            log.exception("refresh loop error")
        await asyncio.sleep(config.REFRESH_S)


@asynccontextmanager
async def lifespan(app: Starlette):
    log.info(
        "mcp_gw starting host=%s port=%s stale=%.0fs refresh=%.0fs token=%s mdns=%s",
        config.HOST,
        config.PORT,
        config.STALE_S,
        config.REFRESH_S,
        "yes" if config.TOKEN else "no",
        "yes" if config.MDNS_ENABLE else "no",
    )
    await STATE.mdns.start()
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
            "mcp": "/mcp",
            "mdns": STATE.mdns.status(),
        }
    )


async def mcp_post(request: Request) -> JSONResponse:
    return await STATE.agg.handle_post(request)  # type: ignore[return-value]


async def mcp_get(request: Request) -> JSONResponse:
    return await STATE.agg.handle_get(request)  # type: ignore[return-value]


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
