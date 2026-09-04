"""实体由本文件转发平台；MQTT 只订 event，与 mcp_gw 同 topic、互不转发。"""

from __future__ import annotations

import logging
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, ServiceCall, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.typing import ConfigType

from .const import CONF_DEVICE_ID, CONF_HOST, CONF_NAME, DOMAIN, MQTT_PREFIX, PLATFORMS
from .coordinator import SnapshotCoordinator

_LOGGER = logging.getLogger(__name__)


async def async_setup(hass: HomeAssistant, _config: ConfigType) -> bool:
    """注册 CoAP PUT 服务（控制面，不是 MQTT command）。"""

    async def _put(path: str, call: ServiceCall) -> None:
        hub_id = str(call.data.get("hub_id") or "").strip().lower()
        payload = call.data.get("payload")
        if not hub_id or not isinstance(payload, dict):
            raise HomeAssistantError("hub_id 与 payload 对象必填")
        for item in hass.data.get(DOMAIN, {}).values():
            coord: SnapshotCoordinator = item["coordinator"]
            if coord.entry.data.get(CONF_DEVICE_ID) == hub_id:
                await coord.coap.put_json(path, payload)
                await coord.async_request_refresh()
                return
        raise HomeAssistantError(f"未找到已配置枢纽 id={hub_id}")

    async def set_thresholds(call: ServiceCall) -> None:
        await _put("/sensors/thresholds", call)

    async def set_config(call: ServiceCall) -> None:
        await _put("/sensors/config", call)

    hass.services.async_register(DOMAIN, "set_thresholds", set_thresholds)
    hass.services.async_register(DOMAIN, "set_config", set_config)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    coordinator = SnapshotCoordinator(hass, entry)
    await coordinator.async_config_entry_first_refresh()
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = {
        "coordinator": coordinator,
        "last_event": None,
    }
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    await _subscribe_mqtt(hass, entry)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unload_ok:
        hass.data[DOMAIN].pop(entry.entry_id, None)
    return unload_ok


def device_info(entry: ConfigEntry) -> DeviceInfo:
    did = entry.data[CONF_DEVICE_ID]
    return DeviceInfo(
        identifiers={(DOMAIN, did)},
        name=entry.data.get(CONF_NAME) or f"MCP Sensors {did}",
        manufacturer="mcp_sensors",
        model="hub",
        configuration_url=f"http://{entry.data[CONF_HOST]}/",
    )


async def _subscribe_mqtt(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """HA 与脚本同订 mcp_sensors/<id>/event。无 command topic。"""
    did = entry.data[CONF_DEVICE_ID]
    topic = f"{MQTT_PREFIX}/{did}/event"
    try:
        from homeassistant.components import mqtt
    except Exception:
        _LOGGER.info("MQTT integration not loaded; events only via CoAP poll")
        return

    @callback
    def _on_event(msg: Any) -> None:
        import json

        try:
            payload = json.loads(msg.payload)
        except Exception:
            payload = {"raw": msg.payload}
        hass.data[DOMAIN][entry.entry_id]["last_event"] = payload
        hass.bus.async_fire(f"{DOMAIN}_event", {"device_id": did, "event": payload})

    try:
        await mqtt.async_subscribe(hass, topic, _on_event, 0)
        _LOGGER.info("subscribed %s", topic)
    except Exception as err:
        _LOGGER.warning("MQTT subscribe failed (install MQTT integration): %s", err)
