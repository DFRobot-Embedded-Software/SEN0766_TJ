from __future__ import annotations

import logging
from starlette.requests import Request
from starlette.responses import JSONResponse

from . import config
from .device_client import DeviceMcpClient
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
    def __init__(self, registry: DeviceRegistry, client: DeviceMcpClient) -> None:
        self.registry = registry
        self.client = client

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
        mcp_path = str(body.get("path") or body.get("mcp_path") or "/mcp_server").strip()

        base_url = str(body.get("base_url") or "").strip()
        if not base_url:
            ip = str(body.get("ip") or "").strip()
            port = int(body.get("port") or 8080)
            if not ip:
                return JSONResponse(
                    {"ok": False, "error": "ip or base_url required"},
                    status_code=400,
                )
            base_url = f"http://{ip}:{port}"

        try:
            rec = self.registry.upsert(
                device_id=device_id,
                base_url=base_url,
                mcp_path=mcp_path,
                name=name,
                fw=fw,
            )
        except ValueError as e:
            return JSONResponse({"ok": False, "error": str(e)}, status_code=400)

        try:
            tools = await self.client.list_tools(rec.mcp_url)
            self.registry.set_tools(rec.id, tools)
        except Exception as e:
            self.registry.mark_error(rec.id, str(e))
            return JSONResponse(
                {
                    "ok": False,
                    "error": f"registered but tools/list failed: {e}",
                    "device": self.registry.to_public(rec),
                },
                status_code=502,
            )

        log.info("registered device id=%s url=%s tools=%s", rec.id, rec.mcp_url, len(rec.tools))
        return JSONResponse({"ok": True, "device": self.registry.to_public(rec)})

    async def heartbeat(self, request: Request) -> JSONResponse:
        if not _check_token(request):
            return _unauthorized()
        device_id = (request.path_params.get("id") or "").strip()
        rec = self.registry.get(device_id)
        if not rec:
            return JSONResponse({"ok": False, "error": "not found"}, status_code=404)
        # 允许心跳顺带更新 IP
        try:
            body = await request.json()
        except Exception:
            body = {}
        if isinstance(body, dict):
            base_url = str(body.get("base_url") or "").strip()
            ip = str(body.get("ip") or "").strip()
            port = body.get("port")
            path = str(body.get("path") or body.get("mcp_path") or "").strip()
            if base_url or ip:
                if not base_url:
                    base_url = f"http://{ip}:{int(port or 8080)}"
                self.registry.upsert(
                    device_id=rec.id,
                    base_url=base_url,
                    mcp_path=path or rec.mcp_path,
                    name=str(body.get("name") or rec.name),
                    fw=str(body.get("fw") or rec.fw),
                )
                rec = self.registry.get(rec.id) or rec
            else:
                rec.touch()
        else:
            rec.touch()
        return JSONResponse({"ok": True, "device": self.registry.to_public(rec)})

    async def list_devices(self, request: Request) -> JSONResponse:
        if not _check_token(request):
            return _unauthorized()
        return JSONResponse(
            {
                "ok": True,
                "devices": [self.registry.to_public(r) for r in self.registry.list_all()],
            }
        )

    async def delete_device(self, request: Request) -> JSONResponse:
        if not _check_token(request):
            return _unauthorized()
        device_id = (request.path_params.get("id") or "").strip()
        ok = self.registry.remove(device_id)
        return JSONResponse({"ok": ok}, status_code=200 if ok else 404)

    async def refresh_device(self, request: Request) -> JSONResponse:
        if not _check_token(request):
            return _unauthorized()
        device_id = (request.path_params.get("id") or "").strip()
        rec = self.registry.get(device_id)
        if not rec:
            return JSONResponse({"ok": False, "error": "not found"}, status_code=404)
        try:
            tools = await self.client.list_tools(rec.mcp_url)
            self.registry.set_tools(rec.id, tools)
            return JSONResponse({"ok": True, "device": self.registry.to_public(rec)})
        except Exception as e:
            self.registry.mark_error(rec.id, str(e))
            return JSONResponse({"ok": False, "error": str(e)}, status_code=502)
