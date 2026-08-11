# -*- coding: utf-8 -*-
"""冒烟：假设备 + 网关 resources list/read/subscribe + SSE 推送。"""
from __future__ import annotations

import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path

import httpx

ROOT = Path(__file__).resolve().parent


async def wait_http(url: str, timeout: float = 15.0) -> None:
    deadline = time.time() + timeout
    async with httpx.AsyncClient() as client:
        while time.time() < deadline:
            try:
                r = await client.get(url, timeout=1.0)
                if r.status_code < 500:
                    return
            except Exception:
                pass
            await asyncio.sleep(0.2)
    raise RuntimeError(f"timeout waiting {url}")


async def run() -> None:
    py = sys.executable
    fake = subprocess.Popen([py, str(ROOT / "fake_device.py"), "--port", "18080"])
    gw = subprocess.Popen(
        [py, "-m", "mcp_gw"],
        cwd=str(ROOT),
        env={**dict(**{k: v for k, v in __import__("os").environ.items()}), "MCP_GW_PORT": "19080"},
    )
    try:
        await wait_http("http://127.0.0.1:19080/health")
        async with httpx.AsyncClient(timeout=30.0) as client:
            r = await client.post(
                "http://127.0.0.1:19080/api/devices/register",
                json={
                    "id": "demo01",
                    "name": "测试设备",
                    "ip": "127.0.0.1",
                    "port": 18080,
                    "path": "/mcp_server",
                },
            )
            print("register", r.status_code, r.text)
            r.raise_for_status()
            assert r.json()["ok"]

            r = await client.post(
                "http://127.0.0.1:19080/mcp",
                json={
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "protocolVersion": "2025-03-26",
                        "capabilities": {},
                        "clientInfo": {"name": "smoke", "version": "0"},
                    },
                },
                headers={"Accept": "application/json, text/event-stream"},
            )
            r.raise_for_status()
            sid = r.headers.get("mcp-session-id")
            caps = r.json()["result"]["capabilities"]
            assert caps["resources"]["subscribe"] is True
            print("initialize ok session", sid)
            headers = {
                "Accept": "application/json, text/event-stream",
                **({"MCP-Session-Id": sid} if sid else {}),
            }

            r = await client.post(
                "http://127.0.0.1:19080/mcp",
                json={"jsonrpc": "2.0", "id": 2, "method": "resources/list", "params": {}},
                headers=headers,
            )
            r.raise_for_status()
            uris = [x["uri"] for x in r.json()["result"]["resources"]]
            assert "demo01__sensor://events" in uris

            r = await client.post(
                "http://127.0.0.1:19080/mcp",
                json={
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "resources/subscribe",
                    "params": {"uri": "demo01__sensor://events"},
                },
                headers=headers,
            )
            r.raise_for_status()
            assert "error" not in r.json()
            print("subscribe ok")

            # 打开 Hermes 侧 SSE，等待桥接后的 updated
            got_updated = False
            async with client.stream(
                "GET",
                "http://127.0.0.1:19080/mcp",
                headers={
                    "Accept": "text/event-stream",
                    **({"MCP-Session-Id": sid} if sid else {}),
                },
                timeout=httpx.Timeout(20.0),
            ) as resp:
                assert resp.status_code == 200
                buf = ""
                deadline = time.time() + 12.0
                async for chunk in resp.aiter_text():
                    buf += chunk
                    while "\n\n" in buf:
                        block, buf = buf.split("\n\n", 1)
                        for line in block.splitlines():
                            if line.startswith("data:"):
                                payload = line[5:].strip()
                                if not payload:
                                    continue
                                msg = json.loads(payload)
                                print("sse note", msg)
                                if msg.get("method") == "notifications/resources/updated":
                                    assert msg["params"]["uri"] == "demo01__sensor://events"
                                    got_updated = True
                    if got_updated or time.time() > deadline:
                        break

            assert got_updated, "did not receive resources/updated over SSE"
        print("SMOKE OK")
    finally:
        gw.terminate()
        fake.terminate()
        gw.wait(timeout=5)
        fake.wait(timeout=5)


if __name__ == "__main__":
    asyncio.run(run())
