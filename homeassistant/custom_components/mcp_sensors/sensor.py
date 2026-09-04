from __future__ import annotations

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorStateClass
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import (
    CONCENTRATION_PARTS_PER_MILLION,
    LIGHT_LUX,
    PERCENTAGE,
    UnitOfTemperature,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import device_info
from .const import CONF_DEVICE_ID, DOMAIN
from .coordinator import SnapshotCoordinator


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    coordinator: SnapshotCoordinator = hass.data[DOMAIN][entry.entry_id]["coordinator"]
    did = entry.data[CONF_DEVICE_ID]
    async_add_entities(
        [
            HubNum(coordinator, entry, did, "temperature_c", "Temperature", SensorDeviceClass.TEMPERATURE, UnitOfTemperature.CELSIUS),
            HubNum(coordinator, entry, did, "humidity_pct", "Humidity", SensorDeviceClass.HUMIDITY, PERCENTAGE),
            HubNum(coordinator, entry, did, "co2_ppm", "CO2", SensorDeviceClass.CO2, CONCENTRATION_PARTS_PER_MILLION),
            HubNum(coordinator, entry, did, "lux", "Illuminance", SensorDeviceClass.ILLUMINANCE, LIGHT_LUX),
            HubNum(coordinator, entry, did, "sound_db", "Sound", None, "dB"),
        ]
    )


class HubNum(CoordinatorEntity[SnapshotCoordinator], SensorEntity):
    _attr_has_entity_name = True
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, coordinator, entry, did, key, name, device_class, unit) -> None:
        super().__init__(coordinator)
        self._key = key
        self._attr_name = name
        self._attr_unique_id = f"{did}_{key}"
        self._attr_device_class = device_class
        self._attr_native_unit_of_measurement = unit
        self._attr_device_info = device_info(entry)

    @property
    def native_value(self):
        return (self.coordinator.data or {}).get(self._key)
