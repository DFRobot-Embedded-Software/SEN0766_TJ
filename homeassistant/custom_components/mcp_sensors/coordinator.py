from __future__ import annotations

from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed
from datetime import timedelta

from .coap import HubCoap
from .const import CONF_HOST, CONF_PORT, DOMAIN

SCAN = timedelta(seconds=30)


class SnapshotCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """温度等靠间隔 GET /sensors/snapshot。进门/哭声走 MQTT，不要缩短本间隔当门铃。"""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        super().__init__(hass, logger=__import__("logging").getLogger(__name__), name=DOMAIN, update_interval=SCAN)
        self.entry = entry
        self.coap = HubCoap(entry.data[CONF_HOST], int(entry.data.get(CONF_PORT) or 5683))

    async def _async_update_data(self) -> dict[str, Any]:
        try:
            data = await self.coap.get_json("/sensors/snapshot")
        except Exception as err:
            raise UpdateFailed(str(err)) from err
        if not isinstance(data, dict):
            raise UpdateFailed("empty snapshot")
        return data
