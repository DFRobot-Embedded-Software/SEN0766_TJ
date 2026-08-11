# -*- coding: utf-8 -*-
"""本地假设备 MCP：验证聚合网关 register + tools 转发。"""
from __future__ import annotations

import argparse
import json
import uuid

from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import JSONResponse, Response
from starlette.routing import Route
import uvicorn

TOOLS = [
    {
        "name": "sensors.get_snapshot",
        "description": "fake env snapshot",
        "inputSchema": {"type": "object", "properties": {}},
    }
]


async def mcp(request: Request) -> Response:
    body = await request.json()
    method = body.get("method")
    req_id = body.get("id")
    if method == "notifications/initialized" or (
        isinstance(method, str) and method.startswith("notifications/") and req_id is None
    ):
        return Response(status_code=202)

    session = request.headers.get("mcp-session-id") or str(uuid.uuid4())
    if method == "initialize":
        msg = {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "protocolVersion": "2025-03-26",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "fake-device", "version": "0.0.1"},
            },
        }
    elif method == "tools/list":
        msg = {"jsonrpc": "2.0", "id": req_id, "result": {"tools": TOOLS}}
    elif method == "tools/call":
        name = (body.get("params") or {}).get("name")
        msg = {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "content": [
                    {
                        "type": "text",
                        "text": json.dumps(
                            {"tool": name, "temperature": 25.5, "humidity": 40},
                            ensure_ascii=False,
                        ),
                    }
                ],
                "isError": False,
            },
        }
    else:
        msg = {
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {"code": -32601, "message": f"unknown {method}"},
        }
    resp = JSONResponse(msg)
    resp.headers["MCP-Session-Id"] = session
    return resp


app = Starlette(routes=[Route("/mcp_server", mcp, methods=["POST"])])


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=18080)
    args = p.parse_args()
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")


if __name__ == "__main__":
    main()
