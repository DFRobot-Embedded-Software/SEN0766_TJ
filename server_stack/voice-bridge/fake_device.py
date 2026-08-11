#!/usr/bin/env python3
"""模拟设备（对齐 ESP32 协议）。

用法：
  python fake_device.py
  python fake_device.py /path/to/16k_mono_s16le.pcm
"""

from __future__ import annotations

import asyncio
import json
import sys

import websockets


async def main(pcm_path: str | None = None) -> None:
    uri = "ws://127.0.0.1:8765/ws"
    async with websockets.connect(uri) as ws:
        print("connected")

        async def reader():
            try:
                async for msg in ws:
                    if isinstance(msg, bytes):
                        print(f"<-- PCM binary {len(msg)} bytes")
                    else:
                        print(f"<-- {msg}")
            except websockets.ConnectionClosed as e:
                print(f"<-- closed: {e}")

        reader_task = asyncio.create_task(reader())

        await ws.send(
            json.dumps(
                {"op": "uplink_start", "rate": 16000, "format": "s16le", "channels": 1}
            )
        )

        if pcm_path:
            with open(pcm_path, "rb") as f:
                data = f.read()
            for i in range(0, len(data), 3200):
                await ws.send(data[i : i + 3200])
                await asyncio.sleep(0.1)
            await ws.send(json.dumps({"op": "uplink_stop", "reason": "vad"}))
            print("pcm uploaded + uplink_stop(vad), waiting reply/close...")
            await asyncio.sleep(60)
        else:
            print("已 uplink_start，2 秒后 uplink_stop(vad)（未推 PCM）")
            await asyncio.sleep(2)
            await ws.send(json.dumps({"op": "uplink_stop", "reason": "vad"}))
            await asyncio.sleep(3)

        reader_task.cancel()


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else None
    asyncio.run(main(path))
