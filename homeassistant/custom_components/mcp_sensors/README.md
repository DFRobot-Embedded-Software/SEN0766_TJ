# Home Assistant：MCP Sensors 自定义集成

**实体来自本集成的 CoAP，不是 MQTT Discovery。**

拷贝 `homeassistant/custom_components/mcp_sensors` 到 HA 的 `config/custom_components/mcp_sensors`，重启。

1. 设置 → 设备与服务 → 添加集成 → **MCP Sensors**，或等 zeroconf `_mcp-sensors._udp` 弹出确认。
2. 安装官方 **MQTT** 集成并连同一 Mosquitto，以便订 `mcp_sensors/<id>/event`。
3. 控制：开发者工具 → 服务 `mcp_sensors.set_thresholds` / `mcp_sensors.set_config`（CoAP PUT，填 `hub_id` 与 JSON）。不要用 MQTT command topic。
4. 语音：设备仍是 WS **客户端**。不要上 Wyoming。用 `homeassistant/addons/mcp_sensors_voice`（内含与 voice-bridge 相同的 `bridge.py`）。设备 browse `_voice-ws` 或把 `AUDIO_WS_URI` 指到 HA。
