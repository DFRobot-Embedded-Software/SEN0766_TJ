from __future__ import annotations

from homeassistant.components.event import EventEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback, Event
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import device_info
from .const import CONF_DEVICE_ID, DOMAIN

EVENT_TYPES = (
    "range_enter",
    "range_exit",
    "room_empty",
    "area_enter",
    "area_exit",
    "approach",
    "away",
    "temp_high",
    "humidity_high",
    "co2_high",
    "sound_db_high",
    "sound_cry",
    "sound_glass",
    "sound_alarm",
    "other",
)


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    async_add_entities([HubDeviceEvent(entry)])


class HubDeviceEvent(EventEntity):
    """MQTT mcp_sensors/<id>/event → HA Event。event_type 取 JSON 的 type 字段。不是 Discovery。"""

    _attr_has_entity_name = True
    _attr_name = "Device event"
    _attr_event_types = list(EVENT_TYPES)

    def __init__(self, entry: ConfigEntry) -> None:
        self._entry = entry
        did = entry.data[CONF_DEVICE_ID]
        self._attr_unique_id = f"{did}_event"
        self._attr_device_info = device_info(entry)

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(self.hass.bus.async_listen(f"{DOMAIN}_event", self._on_bus))

    @callback
    def _on_bus(self, event: Event) -> None:
        data = event.data or {}
        if data.get("device_id") != self._entry.data[CONF_DEVICE_ID]:
            return
        ev = data.get("event") or {}
        etype = str(ev.get("type") or "other")
        if etype not in self._attr_event_types:
            etype = "other"
        self._trigger_event(etype, ev)
        self.async_write_ha_state()
