# -*- coding: utf-8 -*-
"""冒烟：假设备 + 网关 register + 聚合 tools/list/call。"""
from __future__ import annotations

import asyncio
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
        async with httpx.AsyncClient(timeout=20.0) as client:
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

            init = {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-03-26",
                    "capabilities": {},
                    "clientInfo": {"name": "smoke", "version": "0"},
                },
            }
            r = await client.post(
                "http://127.0.0.1:19080/mcp",
                json=init,
                headers={"Accept": "application/json, text/event-stream"},
            )
            r.raise_for_status()
            sid = r.headers.get("mcp-session-id")
            print("initialize ok session", sid)

            r = await client.post(
                "http://127.0.0.1:19080/mcp",
                json={"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
                headers={
                    "Accept": "application/json, text/event-stream",
                    **({"MCP-Session-Id": sid} if sid else {}),
                },
            )
            r.raise_for_status()
            tools = r.json()["result"]["tools"]
            names = [t["name"] for t in tools]
            print("tools", names)
            assert "gw.list_devices" in names
            assert "demo01__sensors.get_snapshot" in names

            r = await client.post(
                "http://127.0.0.1:19080/mcp",
                json={
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "tools/call",
                    "params": {"name": "demo01__sensors.get_snapshot", "arguments": {}},
                },
                headers={
                    "Accept": "application/json, text/event-stream",
                    **({"MCP-Session-Id": sid} if sid else {}),
                },
            )
            r.raise_for_status()
            print("call", r.json())
            assert r.json()["result"]["isError"] is False
        print("SMOKE OK")
    finally:
        gw.terminate()
        fake.terminate()
        gw.wait(timeout=5)
        fake.wait(timeout=5)


if __name__ == "__main__":
    asyncio.run(run())
