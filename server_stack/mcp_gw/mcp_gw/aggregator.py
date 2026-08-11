from __future__ import annotations

import logging
import uuid
from typing import Any, Optional

from starlette.requests import Request
from starlette.responses import JSONResponse, Response

from . import config
from .device_client import DeviceMcpClient
from .registry import DeviceRegistry

log = logging.getLogger("mcp_gw.agg")

GATEWAY_LIST_DEVICES = "gw.list_devices"


class AggregatorMcp:
    """对 Hermes 暴露的聚合 Streamable HTTP MCP（JSON 响应）。"""

    def __init__(self, registry: DeviceRegistry, client: DeviceMcpClient) -> None:
        self.registry = registry
        self.client = client

    def public_tools(self) -> list[dict[str, Any]]:
        tools: list[dict[str, Any]] = [
            {
                "name": GATEWAY_LIST_DEVICES,
                "description": (
                    "列出当前在线的传感器设备（id/name/url）。"
                    "调用具体设备工具前可用此工具确认设备 id；"
                    f"设备工具名格式为 {{id}}{config.TOOL_SEP}{{native_name}}。"
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

    async def handle_post(self, request: Request) -> Response:
        try:
            body = await request.json()
        except Exception:
            return JSONResponse(
                {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}},
                status_code=400,
            )

        if isinstance(body, list):
            # 批量：逐条处理
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
        # notification：无 id
        if "id" not in body and str(body.get("method") or "").startswith("notifications/"):
            return Response(status_code=202)

        result_msg = await self._dispatch_one(body, session)
        # initialize 时分配 session
        if body.get("method") == "initialize":
            session = session or str(uuid.uuid4())
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
                        "capabilities": {"tools": {"listChanged": False}},
                        "serverInfo": {
                            "name": config.SERVER_NAME,
                            "version": config.SERVER_VERSION,
                        },
                        "instructions": (
                            "这是多设备传感器聚合网关。先用 gw.list_devices 查看在线设备，"
                            f"再调用 {{id}}{config.TOOL_SEP}sensors.get_snapshot 等工具。"
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

    async def _call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        if name == GATEWAY_LIST_DEVICES:
            devices = [self.registry.to_public(r) for r in self.registry.list_online()]
            import json

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

    async def handle_get(self, request: Request) -> Response:
        # 最小兼容：不提供 SSE 推送
        return JSONResponse(
            {"error": "SSE not enabled; use POST JSON-RPC on /mcp"},
            status_code=405,
        )
