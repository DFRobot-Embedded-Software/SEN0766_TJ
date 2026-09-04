from __future__ import annotations

from homeassistant import config_entries
from homeassistant.components import zeroconf
from homeassistant.data_entry_flow import FlowResult
import voluptuous as vol

from .coap import HubCoap
from .const import CONF_DEVICE_ID, CONF_HOST, CONF_NAME, CONF_PORT, DOMAIN


class ConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        self._host = ""
        self._port = 5683
        self._device_id = ""
        self._name = ""

    async def async_step_user(self, user_input: dict | None = None) -> FlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            self._host = user_input[CONF_HOST]
            self._port = int(user_input.get(CONF_PORT) or 5683)
            try:
                ident = await HubCoap(self._host, self._port).get_json("/id")
                self._device_id = str((ident or {}).get("id") or "").lower()
            except Exception:
                errors["base"] = "cannot_connect"
                self._device_id = ""
            if self._device_id:
                await self.async_set_unique_id(self._device_id)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(
                    title=self._name or f"MCP Sensors {self._device_id}",
                    data={
                        CONF_HOST: self._host,
                        CONF_PORT: self._port,
                        CONF_DEVICE_ID: self._device_id,
                        CONF_NAME: self._name or self._device_id,
                    },
                )
        schema = vol.Schema(
            {
                vol.Required(CONF_HOST, default=self._host): str,
                vol.Required(CONF_PORT, default=self._port): int,
            }
        )
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)

    async def async_step_zeroconf(self, discovery_info: zeroconf.ZeroconfServiceInfo) -> FlowResult:
        props = discovery_info.properties or {}
        self._device_id = str(props.get("id") or "").strip().lower()
        self._name = str(props.get("name") or self._device_id)
        self._host = discovery_info.host
        self._port = int(discovery_info.port or 5683)
        if not self._device_id:
            return self.async_abort(reason="no_device_id")
        await self.async_set_unique_id(self._device_id)
        self._abort_if_unique_id_configured(
            updates={CONF_HOST: self._host, CONF_PORT: self._port}
        )
        self.context["title_placeholders"] = {"name": self._name}
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(self, user_input: dict | None = None) -> FlowResult:
        if user_input is not None:
            return self.async_create_entry(
                title=self._name or self._device_id,
                data={
                    CONF_HOST: self._host,
                    CONF_PORT: self._port,
                    CONF_DEVICE_ID: self._device_id,
                    CONF_NAME: self._name,
                },
            )
        return self.async_show_form(
            step_id="zeroconf_confirm",
            description_placeholders={"name": self._name, "host": self._host},
        )
