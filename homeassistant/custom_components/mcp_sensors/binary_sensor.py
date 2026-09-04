from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import device_info
from .const import CONF_DEVICE_ID, DOMAIN
from .coordinator import SnapshotCoordinator


def _occupied(snap: dict) -> bool:
    for area in snap.get("areas") or []:
        if not isinstance(area, dict):
            continue
        if area.get("enter") or area.get("approaching"):
            return True
        if (area.get("active") or 0) > 0 or (area.get("static") or 0) > 0:
            return True
    return False


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: SnapshotCoordinator = hass.data[DOMAIN][entry.entry_id]["coordinator"]
    did = entry.data[CONF_DEVICE_ID]
    async_add_entities(
        [
            HubBin(coordinator, entry, did, "occupancy", "Occupancy", BinarySensorDeviceClass.OCCUPANCY, _occupied_fn),
            HubBin(coordinator, entry, did, "sound_alarm", "Sound alarm", BinarySensorDeviceClass.SOUND, lambda s: bool(s.get("sound_alarm"))),
        ]
    )


def _occupied_fn(s: dict) -> bool:
    return _occupied(s)


class HubBin(CoordinatorEntity[SnapshotCoordinator], BinarySensorEntity):
    _attr_has_entity_name = True

    def __init__(self, coordinator, entry, did, key, name, device_class, getter) -> None:
        super().__init__(coordinator)
        self._getter = getter
        self._attr_name = name
        self._attr_unique_id = f"{did}_{key}"
        self._attr_device_class = device_class
        self._attr_device_info = device_info(entry)

    @property
    def is_on(self) -> bool:
        return bool(self._getter(self.coordinator.data or {}))
