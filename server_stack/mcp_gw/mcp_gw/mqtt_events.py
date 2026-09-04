"""订设备 MQTT 事件（契约见固件 mqtt_hub.h）。

订阅（前缀写死为固件 mcp_sensors）：
  mcp_sensors/+/event          JSON，不 retain；写入 event_cache 并 SSE
  mcp_sensors/+/availability   online/offline

网关与 HA 互不依赖：只是可各自订同一 topic。本模块不发 Discovery / state / command。
"""

from __future__ import annotations

import asyncio
import json
import logging
from typing import Any

from . import config
from .registry import DeviceRegistry
from .sessions import SessionManager

log = logging.getLogger("mcp_gw.mqtt")


class MqttEventBridge:
    """设备事件订阅器：MQTT → registry.event_cache → MCP resources/updated。"""

    def __init__(self, registry: DeviceRegistry, sessions: SessionManager) -> None:
        self.registry = registry
        self.sessions = sessions
        self._task: asyncio.Task[Any] | None = None
        self._connected = False
        self.events_received = 0
        self.last_error = ""

    def status(self) -> dict[str, Any]:
        return {
            "enabled": config.MQTT_ENABLE,
            "connected": self._connected,
            "events_received": self.events_received,
            "filter": f"{config.MQTT_PREFIX}/+/event",
            "prefix": config.MQTT_PREFIX,
            "last_error": self.last_error,
        }

    async def start(self) -> None:
        if not config.MQTT_ENABLE:
            log.info("MQTT event subscribe disabled (set MQTT_HOST)")
            return
        self._task = asyncio.create_task(self._run(), name="mqtt-events")

    async def stop(self) -> None:
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass

    async def _run(self) -> None:
        try:
            import aiomqtt
        except ImportError:
            self.last_error = "aiomqtt not installed"
            log.warning(self.last_error)
            return

        event_filter = f"{config.MQTT_PREFIX}/+/event"
        avail_filter = f"{config.MQTT_PREFIX}/+/availability"
        while True:
            try:
                async with aiomqtt.Client(
                    hostname=config.MQTT_HOST,
                    port=config.MQTT_PORT,
                    username=config.MQTT_USER or None,
                    password=config.MQTT_PASSWORD or None,
                    identifier=f"{config.MQTT_CLIENT_ID}-events",
                ) as client:
                    self._connected = True
                    self.last_error = ""
                    await client.subscribe(event_filter)
                    await client.subscribe(avail_filter)
                    log.info("MQTT subscribed %s %s", event_filter, avail_filter)
                    async for message in client.messages:
                        await self._on_message(str(message.topic), message.payload)
            except asyncio.CancelledError:
                self._connected = False
                raise
            except Exception as e:
                self._connected = False
                self.last_error = str(e)
                log.warning("MQTT reconnect in 5s: %s", e)
                await asyncio.sleep(5)

    async def _on_message(self, topic: str, payload: bytes | bytearray | str | None) -> None:
        parts = topic.split("/")
        if len(parts) < 3:
            return
        device_id = parts[1].strip().lower()
        kind = parts[2]
        rec = self.registry.get(device_id)
        if rec is None:
            rec = self.registry.ensure_mqtt_device(device_id)
        raw = payload.decode("utf-8", "replace") if isinstance(payload, (bytes, bytearray)) else str(payload or "")
        if kind == "availability":
            rec.online = raw.strip().lower() not in ("offline", "0", "false")
            if rec.online:
                rec.touch()
            return
        if kind != "event":
            return
        try:
            ev = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            ev = {"raw": raw}
        if not isinstance(ev, dict):
            ev = {"value": ev}
        self.registry.append_event(device_id, ev)
        self.events_received += 1
        log.info("event #%s %s type=%s", self.events_received, topic, ev.get("type"))
        public = f"{device_id}{config.TOOL_SEP}sensor://events"
        await self.sessions.publish_resource_updated(public)
