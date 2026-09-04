# MCP Sensors Voice（HA Add-on）

设备仍是 WebSocket **客户端**；本 Add-on 跑与 `server_stack/voice-bridge` **同一份** `app/bridge.py`（PCM + `uplink_start` / `play_stop`）。不要装 Wyoming。传感器走 CoAP + MQTT。

`app/bridge.py` 已从仓库拷入。改协议时请同时改 `server_stack/voice-bridge/bridge.py` 再同步拷贝。

## 安装

1. 拷贝 `homeassistant/addons/mcp_sensors_voice` 到 HAOS 本地 addons。
2. 环境变量与独立 voice-bridge 相同（`DASHSCOPE_API_KEY`、`HERMES_API_BASE` 等），用 Add-on 配置或 `.env`。
3. `host_network: true`，设备连 `ws://<HA局域网IP>:8765/ws`。
4. 若同时跑 `mcp_gw`，设备可 mDNS 发现 `_voice-ws`；否则把固件 `AUDIO_WS_URI` 写成上式。
