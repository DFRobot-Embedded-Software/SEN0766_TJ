from __future__ import annotations

import asyncio
import json
import logging
import uuid
from typing import Any, Optional

from starlette.requests import Request
from starlette.responses import JSONResponse, Response, StreamingResponse

from . import config
from .device_bridge import DeviceNotificationBridge, split_public_uri
from .device_client import DeviceMcpClient
from .registry import DeviceRegistry
from .sessions import SessionManager

log = logging.getLogger("mcp_gw.agg")

GATEWAY_LIST_DEVICES = "gw.list_devices"


class AggregatorMcp:
    """对 Hermes 暴露的聚合 Streamable HTTP MCP（JSON + GET SSE）。"""

    def __init__(
        self,
        registry: DeviceRegistry,
        client: DeviceMcpClient,
        sessions: SessionManager,
        bridge: DeviceNotificationBridge,
    ) -> None:
        self.registry = registry
        self.client = client
        self.sessions = sessions
        self.bridge = bridge

    def public_tools(self) -> list[dict[str, Any]]:
        tools: list[dict[str, Any]] = [
            {
                "name": GATEWAY_LIST_DEVICES,
                "description": (
                    "列出当前在线的传感器设备（id/name/url）。"
                    "调用具体设备工具前可用此工具确认设备 id；"
                    f"设备工具名格式为 {{id}}{config.TOOL_SEP}{{native_name}}。"
                    f"设备资源 URI 格式为 {{id}}{config.TOOL_SEP}{{native_uri}}，"
                    "例如 {{id}}__sensor://events。订阅后经 GET /mcp SSE 接收更新通知。"
                ),
                "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
            }
        ]
        for rec in self.registry.list_online():
            label = rec.name or rec.id
            for t in rec.tools:
                native = str(t.get("name") or "")
                if not native:
                    continue
                prefixed = f"{rec.id}{config.TOOL_SEP}{native}"
                desc = str(t.get("description") or native)
                tools.append(
                    {
                        "name": prefixed,
                        "description": f"[{label}/{rec.id}] {desc}",
                        "inputSchema": t.get("inputSchema")
                        or t.get("input_schema")
                        or {"type": "object", "properties": {}},
                    }
                )
        return tools

    def public_resources(self) -> list[dict[str, Any]]:
        resources: list[dict[str, Any]] = []
        for rec in self.registry.list_online():
            label = rec.name or rec.id
            for r in rec.resources:
                native_uri = str(r.get("uri") or "")
                if not native_uri:
                    continue
                item = dict(r)
                item["uri"] = f"{rec.id}{config.TOOL_SEP}{native_uri}"
                desc = str(r.get("description") or r.get("name") or native_uri)
                item["description"] = f"[{label}/{rec.id}] {desc}"
                name = str(r.get("name") or "")
                if name and config.TOOL_SEP not in name:
                    item["name"] = f"{rec.id}{config.TOOL_SEP}{name}"
                resources.append(item)
        return resources

    async def handle_post(self, request: Request) -> Response:
        try:
            body = await request.json()
        except Exception:
            return JSONResponse(
                {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}},
                status_code=400,
            )

        if isinstance(body, list):
            out = []
            session = request.headers.get("mcp-session-id")
            for item in body:
                out.append(await self._dispatch_one(item, session))
            resp = JSONResponse(out)
            if session:
                resp.headers["MCP-Session-Id"] = session
            return resp

        if not isinstance(body, dict):
            return JSONResponse(
                {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Invalid Request"}},
                status_code=400,
            )

        session = request.headers.get("mcp-session-id")
        if "id" not in body and str(body.get("method") or "").startswith("notifications/"):
            return Response(status_code=202)

        result_msg = await self._dispatch_one(body, session)
        if body.get("method") == "initialize":
            session = session or str(uuid.uuid4())
            await self.sessions.ensure(session)
        resp = JSONResponse(result_msg)
        if session:
            resp.headers["MCP-Session-Id"] = session
        return resp

    async def _dispatch_one(self, body: dict[str, Any], session: Optional[str]) -> dict[str, Any]:
        req_id = body.get("id")
        method = body.get("method")
        params = body.get("params") or {}

        try:
            if method == "initialize":
                return {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "result": {
                        "protocolVersion": config.PROTOCOL_VERSION,
                        "capabilities": {
                            "tools": {"listChanged": False},
                            "resources": {"subscribe": True, "listChanged": True},
                        },
                        "serverInfo": {
                            "name": config.SERVER_NAME,
                            "version": config.SERVER_VERSION,
                        },
                        "instructions": (
                            "多设备传感器聚合网关。"
                            "环境读数与阈值：{id}__sensors.get_snapshot（含 sound_db、sound_warning_db、thresholds）；"
                            "仅阈值：{id}__sensors.get_thresholds（sound.warning_db 等）；"
                            "雷达区域：{id}__sensors.get_config；"
                            "事件轮询请优先：{id}__sensors.get_recent_events（limit/category，比 resources/read 更稳）；"
                            "事件资源：{id}__sensor://events（subscribe + GET /mcp SSE）。"
                        ),
                    },
                }
            if method == "tools/list":
                return {"jsonrpc": "2.0", "id": req_id, "result": {"tools": self.public_tools()}}
            if method == "tools/call":
                name = str((params or {}).get("name") or "")
                args = (params or {}).get("arguments") or {}
                if not isinstance(args, dict):
                    args = {}
                result = await self._call_tool(name, args)
                return {"jsonrpc": "2.0", "id": req_id, "result": result}
            if method == "resources/list":
                return {
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "result": {"resources": self.public_resources()},
                }
            if method == "resources/templates/list":
                return {"jsonrpc": "2.0", "id": req_id, "result": {"resourceTemplates": []}}
            if method == "resources/read":
                uri = str((params or {}).get("uri") or "")
                result = await self._read_resource(uri)
                return {"jsonrpc": "2.0", "id": req_id, "result": result}
            if method == "resources/subscribe":
                uri = str((params or {}).get("uri") or "")
                await self._subscribe(session, uri)
                return {"jsonrpc": "2.0", "id": req_id, "result": {}}
            if method == "resources/unsubscribe":
                uri = str((params or {}).get("uri") or "")
                await self._unsubscribe(session, uri)
                return {"jsonrpc": "2.0", "id": req_id, "result": {}}
            if method == "ping":
                return {"jsonrpc": "2.0", "id": req_id, "result": {}}
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32601, "message": f"Method not found: {method}"},
            }
        except Exception as e:
            log.exception("MCP method %s failed", method)
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32000, "message": str(e)},
            }

    async def _subscribe(self, session: Optional[str], public_uri: str) -> None:
        if not session:
            raise RuntimeError("resources/subscribe 需要 MCP-Session-Id（先 initialize）")
        if not public_uri:
            raise RuntimeError("uri required")
        # 校验格式与在线
        device_id, _native = split_public_uri(public_uri)
        rec = self.registry.get(device_id)
        if rec is None:
            raise RuntimeError(f"设备未注册: {device_id}")
        online_ids = {r.id for r in self.registry.list_online()}
        if rec.id not in online_ids:
            raise RuntimeError(f"设备离线或心跳超时: {device_id}")
        await self.sessions.subscribe(session, public_uri)
        await self.bridge.on_subscribe(public_uri)

    async def _unsubscribe(self, session: Optional[str], public_uri: str) -> None:
        if not session:
            return
        await self.sessions.unsubscribe(session, public_uri)
        await self.bridge.on_unsubscribe(public_uri)

    async def _call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        if name == GATEWAY_LIST_DEVICES:
            devices = [self.registry.to_public(r) for r in self.registry.list_online()]
            text = json.dumps(devices, ensure_ascii=False, indent=2)
            return {"content": [{"type": "text", "text": text}], "isError": False}

        if config.TOOL_SEP not in name:
            return {
                "content": [
                    {
                        "type": "text",
                        "text": (
                            f"未知工具 {name}。设备工具须为 id{config.TOOL_SEP}native；"
                            f"或使用 {GATEWAY_LIST_DEVICES}。"
                        ),
                    }
                ],
                "isError": True,
            }

        device_id, native = name.split(config.TOOL_SEP, 1)
        rec = self.registry.get(device_id)
        if rec is None:
            return {
                "content": [{"type": "text", "text": f"设备未注册: {device_id}"}],
                "isError": True,
            }
        online_ids = {r.id for r in self.registry.list_online()}
        if rec.id not in online_ids:
            return {
                "content": [{"type": "text", "text": f"设备离线或心跳超时: {device_id}"}],
                "isError": True,
            }

        try:
            return await self.client.call_tool(rec.mcp_url, native, arguments)
        except Exception as e:
            self.registry.mark_error(device_id, str(e))
            return {
                "content": [{"type": "text", "text": f"调用设备失败: {e}"}],
                "isError": True,
            }

    async def _read_resource(self, public_uri: str) -> dict[str, Any]:
        if config.TOOL_SEP not in public_uri:
            raise RuntimeError(
                f"资源 URI 须为 id{config.TOOL_SEP}native，例如 demo01{config.TOOL_SEP}sensor://events"
            )
        device_id, native_uri = public_uri.split(config.TOOL_SEP, 1)
        rec = self.registry.get(device_id)
        if rec is None:
            raise RuntimeError(f"设备未注册: {device_id}")
        online_ids = {r.id for r in self.registry.list_online()}
        if rec.id not in online_ids:
            raise RuntimeError(f"设备离线或心跳超时: {device_id}")

        try:
            result = await self.client.read_resource(rec.mcp_url, native_uri)
        except Exception as e:
            self.registry.mark_error(device_id, str(e))
            raise RuntimeError(f"读取设备资源失败: {e}") from e

        contents = result.get("contents")
        if isinstance(contents, list):
            rewritten = []
            for item in contents:
                if not isinstance(item, dict):
                    rewritten.append(item)
                    continue
                row = dict(item)
                uri = str(row.get("uri") or native_uri)
                if uri.startswith(f"{device_id}{config.TOOL_SEP}"):
                    row["uri"] = uri
                else:
                    row["uri"] = f"{device_id}{config.TOOL_SEP}{uri}"
                rewritten.append(row)
            result = dict(result)
            result["contents"] = rewritten
        return result

    async def handle_get(self, request: Request) -> Response:
        accept = (request.headers.get("accept") or "").lower()
        if "text/event-stream" not in accept and "*/*" not in accept:
            return JSONResponse(
                {"error": "Accept must include text/event-stream for SSE"},
                status_code=406,
            )

        session_id = request.headers.get("mcp-session-id") or request.headers.get("MCP-Session-Id")
        if not session_id:
            return JSONResponse(
                {"error": "MCP-Session-Id required (initialize via POST first)"},
                status_code=400,
            )

        sess = await self.sessions.ensure(session_id)
        sess.sse_attached = True
        log.info("Hermes SSE open session=%s", session_id)

        async def event_gen():
            # 立即发注释，确认流已建立
            yield ": connected\n\n"
            try:
                while True:
                    try:
                        note = await asyncio.wait_for(sess.queue.get(), timeout=15.0)
                        payload = json.dumps(note, ensure_ascii=False)
                        yield f"event: message\ndata: {payload}\n\n"
                    except asyncio.TimeoutError:
                        yield ": ping\n\n"
            except asyncio.CancelledError:
                raise
            finally:
                sess.sse_attached = False
                log.info("Hermes SSE closed session=%s", session_id)

        return StreamingResponse(
            event_gen(),
            media_type="text/event-stream",
            headers={
                "Cache-Control": "no-cache",
                "Connection": "keep-alive",
                "MCP-Session-Id": session_id,
                "X-Accel-Buffering": "no",
            },
        )
