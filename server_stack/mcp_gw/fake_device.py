# -*- coding: utf-8 -*-
"""本地假设备 MCP：tools/resources + GET SSE 推送。"""
from __future__ import annotations

import argparse
import asyncio
import json
import uuid

from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import JSONResponse, Response, StreamingResponse
from starlette.routing import Route
import uvicorn

TOOLS = [
    {
        "name": "sensors.get_snapshot",
        "description": "fake env snapshot",
        "inputSchema": {"type": "object", "properties": {}},
    }
]

RESOURCES = [
    {
        "uri": "sensor://events",
        "name": "events",
        "description": "fake recent events",
        "mimeType": "application/json",
    },
    {
        "uri": "sensor://temperature",
        "name": "temperature",
        "description": "fake temperature resource",
        "mimeType": "application/json",
    },
]

EVENTS_PAYLOAD = {
    "ready": True,
    "events": [
        {
            "type": "area_enter",
            "category": "mmwave",
            "message": "人员#1进入客厅，该区域有1人活动，0人静止",
            "area": "客厅",
        }
    ],
}

# session_id -> set(uris) ; session_id -> list of asyncio.Queue
SUBSCRIPTIONS: dict[str, set[str]] = {}
SSE_QUEUES: dict[str, asyncio.Queue] = {}


async def mcp_post(request: Request) -> Response:
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
                "capabilities": {
                    "tools": {},
                    "resources": {"subscribe": True, "listChanged": True},
                },
                "serverInfo": {"name": "fake-device", "version": "0.0.2"},
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
    elif method == "resources/list":
        msg = {"jsonrpc": "2.0", "id": req_id, "result": {"resources": RESOURCES}}
    elif method == "resources/read":
        uri = str((body.get("params") or {}).get("uri") or "")
        if uri == "sensor://events":
            payload = EVENTS_PAYLOAD
        elif uri == "sensor://temperature":
            payload = {"temperature": 25.5, "humidity": 40}
        else:
            msg = {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {"code": -32002, "message": f"unknown resource {uri}"},
            }
            resp = JSONResponse(msg)
            resp.headers["MCP-Session-Id"] = session
            return resp
        msg = {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "contents": [
                    {
                        "uri": uri,
                        "mimeType": "application/json",
                        "text": json.dumps(payload, ensure_ascii=False),
                    }
                ]
            },
        }
    elif method == "resources/subscribe":
        uri = str((body.get("params") or {}).get("uri") or "")
        SUBSCRIPTIONS.setdefault(session, set()).add(uri)
        msg = {"jsonrpc": "2.0", "id": req_id, "result": {}}
        # 订阅后短暂延迟推一条 updated，便于冒烟
        q = SSE_QUEUES.get(session)

        async def _emit() -> None:
            await asyncio.sleep(0.4)
            note = {
                "jsonrpc": "2.0",
                "method": "notifications/resources/updated",
                "params": {"uri": uri},
            }
            if q is not None:
                await q.put(note)

        asyncio.create_task(_emit())
    elif method == "resources/unsubscribe":
        uri = str((body.get("params") or {}).get("uri") or "")
        if session in SUBSCRIPTIONS:
            SUBSCRIPTIONS[session].discard(uri)
        msg = {"jsonrpc": "2.0", "id": req_id, "result": {}}
    else:
        msg = {
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {"code": -32601, "message": f"unknown {method}"},
        }
    resp = JSONResponse(msg)
    resp.headers["MCP-Session-Id"] = session
    return resp


async def mcp_get(request: Request) -> Response:
    accept = (request.headers.get("accept") or "").lower()
    if "text/event-stream" not in accept and "*/*" not in accept:
        return JSONResponse({"error": "need text/event-stream"}, status_code=406)
    session = request.headers.get("mcp-session-id")
    if not session:
        return JSONResponse({"error": "MCP-Session-Id required"}, status_code=400)

    q: asyncio.Queue = asyncio.Queue()
    SSE_QUEUES[session] = q

    async def gen():
        yield ": connected\n\n"
        try:
            while True:
                try:
                    note = await asyncio.wait_for(q.get(), timeout=10.0)
                    yield f"event: message\ndata: {json.dumps(note, ensure_ascii=False)}\n\n"
                except asyncio.TimeoutError:
                    yield ": ping\n\n"
        finally:
            SSE_QUEUES.pop(session, None)

    return StreamingResponse(
        gen(),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "MCP-Session-Id": session},
    )


app = Starlette(
    routes=[
        Route("/mcp_server", mcp_post, methods=["POST"]),
        Route("/mcp_server", mcp_get, methods=["GET"]),
    ]
)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=18080)
    args = p.parse_args()
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")


if __name__ == "__main__":
    main()
