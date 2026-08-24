"""把已注册设备的快照/事件推到 Home Assistant MQTT Discovery。音频不走此通道。"""

from __future__ import annotations

import asyncio
import json
import logging
import re
from typing import Any, Optional

from . import config
from .device_client import DeviceMcpClient
from .registry import DeviceRecord, DeviceRegistry

log = logging.getLogger("mcp_gw.mqtt")

HA_STATUS_TOPIC = "homeassistant/status"
GW_AVAIL_SUFFIX = "gw/availability"

EVENT_TYPES = (
    "range_enter",
    "range_exit",
    "room_empty",
    "area_enter",
    "area_exit",
    "approach",
    "away",
    "person_static",
    "person_moving",
    "temp_high",
    "humidity_high",
    "humidity_low",
    "co2_high",
    "lux_dark",
    "lux_bright",
    "sound_db_high",
    "sound_cry",
    "sound_glass",
    "sound_alarm",
    "other",
)

_SAFE = re.compile(r"[^a-zA-Z0-9_-]+")


def mqtt_object_id(device_id: str) -> str:
    oid = _SAFE.sub("_", (device_id or "").strip().lower()).strip("_")
    return oid or "device"


def extract_tool_json(result: dict[str, Any] | None) -> Any:
    if not isinstance(result, dict) or result.get("isError"):
        return None
    structured = result.get("structuredContent")
    if isinstance(structured, (dict, list)):
        return structured
    texts: list[str] = []
    for item in result.get("content") or []:
        if isinstance(item, dict) and item.get("type") == "text":
            texts.append(str(item.get("text") or ""))
    blob = "\n".join(texts).strip()
    if not blob or blob.lower() in ("true", "false"):
        return None
    try:
        return json.loads(blob)
    except json.JSONDecodeError:
        return None


def occupancy_from_snapshot(snap: dict[str, Any]) -> tuple[bool, int]:
    people = 0
    occupied = False
    for area in snap.get("areas") or []:
        if not isinstance(area, dict):
            continue
        if area.get("enter") or area.get("approaching"):
            occupied = True
        active = area.get("active")
        if isinstance(active, (int, float)) and active > 0:
            people += int(active)
            occupied = True
        static = area.get("static")
        if isinstance(static, (int, float)) and static > 0:
            occupied = True
    if occupied and people <= 0:
        people = 1
    return occupied, people


def flatten_state(snap: dict[str, Any]) -> dict[str, Any]:
    occupied, people = occupancy_from_snapshot(snap)
    return {
        "temperature": snap.get("temperature_c"),
        "humidity": snap.get("humidity_pct"),
        "co2": snap.get("co2_ppm"),
        "lux": snap.get("lux"),
        "sound_db": snap.get("sound_db"),
        "sound_alarm": "ON" if snap.get("sound_alarm") else "OFF",
        "occupancy": "ON" if occupied else "OFF",
        "people": people,
        "env_ready": bool(snap.get("env_ready")),
        "lux_ready": bool(snap.get("lux_ready")),
        "sound_ready": bool(snap.get("sound_ready")),
    }


def event_fingerprint(ev: dict[str, Any]) -> str:
    return "|".join(
        [
            str(ev.get("timestamp") or ""),
            str(ev.get("type") or ""),
            str(ev.get("category") or ""),
            str(ev.get("area") or ""),
            str(ev.get("message") or ""),
        ]
    )


def discovery_payload(rec: DeviceRecord, *, state_topic: str, avail_topic: str, event_topic: str) -> dict[str, Any]:
    oid = mqtt_object_id(rec.id)
    uid = f"mcp_{oid}"
    device = {
        "identifiers": [uid],
        "name": rec.name or rec.id,
        "manufacturer": "mcp_sensors",
        "model": "ESP sensor node",
        "sw_version": rec.fw or config.SERVER_VERSION,
    }
    if rec.base_url:
        device["configuration_url"] = rec.base_url.rstrip("/") + "/"

    cmps: dict[str, Any] = {}

    def add(key: str, body: dict[str, Any]) -> None:
        cmps[key] = body

    add(
        "temperature",
        {
            "p": "sensor",
            "name": "Temperature",
            "unique_id": f"{uid}_temperature",
            "device_class": "temperature",
            "unit_of_measurement": "°C",
            "state_class": "measurement",
            "value_template": "{{ value_json.temperature }}",
            "suggested_display_precision": 1,
        },
    )
    add(
        "humidity",
        {
            "p": "sensor",
            "name": "Humidity",
            "unique_id": f"{uid}_humidity",
            "device_class": "humidity",
            "unit_of_measurement": "%",
            "state_class": "measurement",
            "value_template": "{{ value_json.humidity }}",
            "suggested_display_precision": 1,
        },
    )
    add(
        "co2",
        {
            "p": "sensor",
            "name": "CO2",
            "unique_id": f"{uid}_co2",
            "device_class": "carbon_dioxide",
            "unit_of_measurement": "ppm",
            "state_class": "measurement",
            "value_template": "{{ value_json.co2 }}",
        },
    )
    add(
        "lux",
        {
            "p": "sensor",
            "name": "Illuminance",
            "unique_id": f"{uid}_lux",
            "device_class": "illuminance",
            "unit_of_measurement": "lx",
            "state_class": "measurement",
            "value_template": "{{ value_json.lux }}",
        },
    )
    add(
        "sound_db",
        {
            "p": "sensor",
            "name": "Sound level",
            "unique_id": f"{uid}_sound_db",
            "icon": "mdi:volume-high",
            "unit_of_measurement": "dB",
            "state_class": "measurement",
            "value_template": "{{ value_json.sound_db }}",
        },
    )
    add(
        "people",
        {
            "p": "sensor",
            "name": "People",
            "unique_id": f"{uid}_people",
            "icon": "mdi:account-group",
            "state_class": "measurement",
            "value_template": "{{ value_json.people }}",
        },
    )
    add(
        "occupancy",
        {
            "p": "binary_sensor",
            "name": "Occupancy",
            "unique_id": f"{uid}_occupancy",
            "device_class": "occupancy",
            "value_template": "{{ value_json.occupancy }}",
            "payload_on": "ON",
            "payload_off": "OFF",
        },
    )
    add(
        "sound_alarm",
        {
            "p": "binary_sensor",
            "name": "Sound alarm",
            "unique_id": f"{uid}_sound_alarm",
            "device_class": "sound",
            "value_template": "{{ value_json.sound_alarm }}",
            "payload_on": "ON",
            "payload_off": "OFF",
        },
    )
    add(
        "event",
        {
            "p": "event",
            "name": "Device event",
            "unique_id": f"{uid}_event",
            "event_types": list(EVENT_TYPES),
            "state_topic": event_topic,
            "json_attributes_topic": event_topic,
        },
    )

    return {
        "device": device,
        "origin": {
            "name": config.SERVER_NAME,
            "sw_version": config.SERVER_VERSION,
        },
        "availability_topic": avail_topic,
        "payload_available": "online",
        "payload_not_available": "offline",
        "state_topic": state_topic,
        "qos": 1,
        "components": cmps,
    }


class HaMqttBridge:
    def __init__(self, registry: DeviceRegistry, client: DeviceMcpClient) -> None:
        self.registry = registry
        self.mcp = client
        self._task: asyncio.Task | None = None
        self._stop = asyncio.Event()
        self._connected = False
        self._last_error = ""
        self._seen_events: dict[str, set[str]] = {}
        self._last_state: dict[str, str] = {}
        self._discovery_sent: set[str] = set()
        self._force_discovery = asyncio.Event()

    def status(self) -> dict[str, Any]:
        return {
            "enabled": config.MQTT_ENABLE,
            "host": config.MQTT_HOST or None,
            "port": config.MQTT_PORT,
            "connected": self._connected,
            "last_error": self._last_error,
            "prefix": config.MQTT_PREFIX,
        }

    async def start(self) -> None:
        if not config.MQTT_ENABLE:
            log.info("MQTT 未启用（设 MQTT_HOST 且 MQTT_ENABLE=1）")
            return
        self._stop.clear()
        self._task = asyncio.create_task(self._run(), name="ha-mqtt")

    async def stop(self) -> None:
        self._stop.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        self._connected = False

    def request_discovery(self) -> None:
        self._force_discovery.set()

    def _topics(self, rec: DeviceRecord) -> tuple[str, str, str, str]:
        oid = mqtt_object_id(rec.id)
        base = f"{config.MQTT_PREFIX}/{oid}"
        state = f"{base}/state"
        avail = f"{base}/availability"
        event = f"{base}/event"
        disc = f"{config.MQTT_DISCOVERY_PREFIX}/device/{oid}/config"
        return disc, state, avail, event

    async def _run(self) -> None:
        try:
            import aiomqtt
            from paho.mqtt.enums import MQTTProtocolVersion
        except ImportError:
            self._last_error = "aiomqtt 未安装（pip install aiomqtt）"
            log.error("%s", self._last_error)
            return

        backoff = 2.0
        while not self._stop.is_set():
            try:
                will = aiomqtt.Will(
                    topic=f"{config.MQTT_PREFIX}/{GW_AVAIL_SUFFIX}",
                    payload="offline",
                    qos=1,
                    retain=True,
                )
                kwargs: dict[str, Any] = {
                    "hostname": config.MQTT_HOST,
                    "port": config.MQTT_PORT,
                    "identifier": config.MQTT_CLIENT_ID,
                    "protocol": MQTTProtocolVersion.MQTTv5,
                    "will": will,
                }
                if config.MQTT_USER:
                    kwargs["username"] = config.MQTT_USER
                if config.MQTT_PASSWORD:
                    kwargs["password"] = config.MQTT_PASSWORD
                async with aiomqtt.Client(**kwargs) as mqtt:
                    self._connected = True
                    self._last_error = ""
                    backoff = 2.0
                    log.info(
                        "MQTT connected broker=%s:%s discovery=%s/#",
                        config.MQTT_HOST,
                        config.MQTT_PORT,
                        config.MQTT_DISCOVERY_PREFIX,
                    )
                    await mqtt.publish(
                        f"{config.MQTT_PREFIX}/{GW_AVAIL_SUFFIX}",
                        payload="online",
                        qos=1,
                        retain=True,
                    )
                    await mqtt.subscribe(HA_STATUS_TOPIC)
                    self._force_discovery.set()
                    reader = asyncio.create_task(self._on_ha_status(mqtt), name="mqtt-ha-status")
                    try:
                        await self._poll_loop(mqtt)
                    finally:
                        reader.cancel()
                        try:
                            await reader
                        except asyncio.CancelledError:
                            pass
            except asyncio.CancelledError:
                raise
            except Exception as e:
                self._connected = False
                self._last_error = str(e)
                log.warning("MQTT 断开，%.0fs 后重连: %s", backoff, e)
                try:
                    await asyncio.wait_for(self._stop.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60.0)

    async def _on_ha_status(self, mqtt: Any) -> None:
        try:
            async for message in mqtt.messages:
                payload = message.payload
                text = payload.decode("utf-8", errors="replace") if isinstance(payload, (bytes, bytearray)) else str(payload)
                if text.strip().lower() == "online":
                    log.info("HA MQTT birth online — 重新发送 discovery")
                    self._discovery_sent.clear()
                    self._force_discovery.set()
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("HA status 订阅异常")

    async def _poll_loop(self, mqtt: Any) -> None:
        while not self._stop.is_set():
            force = self._force_discovery.is_set()
            if force:
                self._force_discovery.clear()
            online_ids = {r.id for r in self.registry.list_online()}
            for rec in self.registry.list_all():
                try:
                    if rec.id in online_ids:
                        await self._publish_device(mqtt, rec, force_discovery=force)
                    else:
                        await self._publish_offline(mqtt, rec)
                except Exception as e:
                    err = str(e)
                    if "session state alloc failed" in err:
                        log.warning(
                            "MQTT skip id=%s: 设备 MCP 会话槽已满，请重启该设备并确保网关已更新为会 DELETE 会话的版本",
                            rec.id,
                        )
                        try:
                            await self._publish_offline(mqtt, rec)
                        except Exception:
                            pass
                    else:
                        log.exception("MQTT 发布失败 id=%s", rec.id)
            try:
                await asyncio.wait_for(self._stop.wait(), timeout=max(2.0, config.MQTT_POLL_S))
            except asyncio.TimeoutError:
                pass

    async def _publish_offline(self, mqtt: Any, rec: DeviceRecord) -> None:
        _disc, _state, avail, _event = self._topics(rec)
        await mqtt.publish(avail, payload="offline", qos=1, retain=True)

    async def _publish_device(self, mqtt: Any, rec: DeviceRecord, *, force_discovery: bool) -> None:
        disc, state_t, avail, event_t = self._topics(rec)
        snap_raw = extract_tool_json(await self.mcp.call_tool(rec.mcp_url, "sensors.get_snapshot", {}))
        if not isinstance(snap_raw, dict):
            await mqtt.publish(avail, payload="offline", qos=1, retain=True)
            return

        if force_discovery or rec.id not in self._discovery_sent:
            payload = discovery_payload(rec, state_topic=state_t, avail_topic=avail, event_topic=event_t)
            await mqtt.publish(
                disc,
                payload=json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
                qos=1,
                retain=True,
            )
            self._discovery_sent.add(rec.id)
            log.info("MQTT discovery id=%s topic=%s", rec.id, disc)

        state = flatten_state(snap_raw)
        blob = json.dumps(state, ensure_ascii=False, separators=(",", ":"))
        if self._last_state.get(rec.id) != blob:
            await mqtt.publish(state_t, payload=blob, qos=1, retain=True)
            self._last_state[rec.id] = blob

        await mqtt.publish(avail, payload="online", qos=1, retain=True)
        await self._publish_new_events(mqtt, rec, event_t)

    async def _publish_new_events(self, mqtt: Any, rec: DeviceRecord, event_topic: str) -> None:
        raw = extract_tool_json(
            await self.mcp.call_tool(rec.mcp_url, "sensors.get_recent_events", {"limit": 12})
        )
        events: list[dict[str, Any]] = []
        if isinstance(raw, dict):
            events = [e for e in (raw.get("events") or []) if isinstance(e, dict)]
        seen = self._seen_events.setdefault(rec.id, set())
        if not seen:
            for ev in events:
                seen.add(event_fingerprint(ev))
            return
        for ev in events:
            fp = event_fingerprint(ev)
            if fp in seen:
                continue
            seen.add(fp)
            etype = str(ev.get("type") or "other")
            if etype not in EVENT_TYPES:
                etype = "other"
            body = {
                "event_type": etype,
                "category": ev.get("category"),
                "timestamp": ev.get("timestamp"),
                "area": ev.get("area"),
                "area_id": ev.get("area_id"),
                "message": ev.get("message"),
            }
            await mqtt.publish(
                event_topic,
                payload=json.dumps(body, ensure_ascii=False, separators=(",", ":")),
                qos=1,
                retain=False,
            )
            log.info("MQTT event id=%s type=%s", rec.id, etype)
        if len(seen) > 200:
            self._seen_events[rec.id] = set(list(seen)[-80:])
