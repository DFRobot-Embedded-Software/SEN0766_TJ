# -*- coding: utf-8 -*-
"""冒烟：手工登记 + 聚合 MCP initialize / tools/list（CoAP 工具目录，不连设备 HTTP MCP）。"""
from __future__ import annotations

import asyncio
import os
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
    env = {**os.environ, "MCP_GW_PORT": "19080", "MCP_GW_MDNS": "0", "MQTT_ENABLE": "0", "MQTT_HOST": ""}
    gw = subprocess.Popen([py, "-m", "mcp_gw"], cwd=str(ROOT), env=env)
    try:
        await wait_http("http://127.0.0.1:19080/health")
        async with httpx.AsyncClient(timeout=30.0) as client:
            r = await client.post(
                "http://127.0.0.1:19080/api/devices/register",
                json={"id": "demo01", "name": "测试设备", "ip": "127.0.0.1", "port": 5683},
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
            names = []
            r = await client.post(
                "http://127.0.0.1:19080/mcp",
                json={"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
                headers={"Accept": "application/json", "MCP-Session-Id": r.headers.get("mcp-session-id") or ""},
            )
            r.raise_for_status()
            names = [t["name"] for t in r.json()["result"]["tools"]]
            print("tools", names)
            assert "gw.list_devices" in names
            assert "demo01__sensors.get_snapshot" in names
            print("smoke ok")
    finally:
        gw.terminate()
        try:
            gw.wait(timeout=5)
        except subprocess.TimeoutExpired:
            gw.kill()


if __name__ == "__main__":
    asyncio.run(run())
