from __future__ import annotations

import logging
from typing import Any, Optional

import httpx

from . import config

log = logging.getLogger("mcp_gw.device")


class DeviceMcpClient:
    """对 ESP32 Streamable HTTP MCP 的轻量客户端（initialize → list/call）。"""

    def __init__(self, timeout_s: float | None = None) -> None:
        self._timeout = timeout_s if timeout_s is not None else config.DEVICE_TIMEOUT_S

    async def list_tools(self, mcp_url: str) -> list[dict[str, Any]]:
        async with httpx.AsyncClient(timeout=self._timeout) as client:
            session_id = await self._initialize(client, mcp_url)
            result = await self._rpc(
                client,
                mcp_url,
                "tools/list",
                {},
                session_id=session_id,
                req_id=2,
            )
        tools = (result or {}).get("tools") or []
        if not isinstance(tools, list):
            return []
        return [t for t in tools if isinstance(t, dict) and t.get("name")]

    async def call_tool(
        self,
        mcp_url: str,
        name: str,
        arguments: Optional[dict[str, Any]] = None,
    ) -> dict[str, Any]:
        async with httpx.AsyncClient(timeout=self._timeout) as client:
            session_id = await self._initialize(client, mcp_url)
            result = await self._rpc(
                client,
                mcp_url,
                "tools/call",
                {"name": name, "arguments": arguments or {}},
                session_id=session_id,
                req_id=2,
            )
        if not isinstance(result, dict):
            return {"content": [{"type": "text", "text": str(result)}], "isError": True}
        return result

    async def probe(self, mcp_url: str) -> tuple[bool, str]:
        try:
            tools = await self.list_tools(mcp_url)
            return True, f"ok tools={len(tools)}"
        except Exception as e:
            return False, str(e)

    async def _initialize(self, client: httpx.AsyncClient, mcp_url: str) -> Optional[str]:
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }
        body = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": config.PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": config.SERVER_NAME, "version": config.SERVER_VERSION},
            },
        }
        r = await client.post(mcp_url, json=body, headers=headers)
        r.raise_for_status()
        session_id = r.headers.get("mcp-session-id") or r.headers.get("MCP-Session-Id")
        data = self._parse_body(r)
        if isinstance(data, dict) and data.get("error"):
            raise RuntimeError(f"initialize error: {data['error']}")

        # 新协议要求 initialized 通知
        note_headers = dict(headers)
        if session_id:
            note_headers["MCP-Session-Id"] = session_id
        note = {
            "jsonrpc": "2.0",
            "method": "notifications/initialized",
            "params": {},
        }
        try:
            nr = await client.post(mcp_url, json=note, headers=note_headers)
            # 部分固件对 notification 返回 202/空 body，不强制 2xx 以外失败
            if nr.status_code >= 400:
                log.debug("initialized notify status=%s", nr.status_code)
        except Exception as e:
            log.debug("initialized notify failed: %s", e)
        return session_id

    async def _rpc(
        self,
        client: httpx.AsyncClient,
        mcp_url: str,
        method: str,
        params: dict[str, Any],
        *,
        session_id: Optional[str],
        req_id: int,
    ) -> Any:
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }
        if session_id:
            headers["MCP-Session-Id"] = session_id
        body = {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params}
        r = await client.post(mcp_url, json=body, headers=headers)
        r.raise_for_status()
        data = self._parse_body(r)
        if not isinstance(data, dict):
            raise RuntimeError(f"bad RPC response for {method}")
        if data.get("error"):
            raise RuntimeError(f"{method} error: {data['error']}")
        return data.get("result")

    @staticmethod
    def _parse_body(r: httpx.Response) -> Any:
        ctype = (r.headers.get("content-type") or "").lower()
        text = r.text or ""
        if not text.strip():
            return {}
        if "text/event-stream" in ctype or text.lstrip().startswith("event:"):
            # 取最后一条 data: JSON
            import json

            last = None
            for line in text.splitlines():
                if line.startswith("data:"):
                    payload = line[5:].strip()
                    if payload and payload != "[DONE]":
                        try:
                            last = json.loads(payload)
                        except json.JSONDecodeError:
                            continue
            return last if last is not None else {}
        return r.json()
