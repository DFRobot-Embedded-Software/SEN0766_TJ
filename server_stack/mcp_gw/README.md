# MCP Sensors Gateway (`mcp_gw`)

脚本侧 **MCP 只在本进程**。设备侧没有 HTTP MCP。

与 Home Assistant **互不依赖**：两边可各自订同一 MQTT topic，也可只跑其中一侧。

```text
枢纽 ESP32  ──mDNS _mcp-sensors._udp:5683──►  mcp_gw 浏览
            ──CoAP GET/PUT─────────────────►  工具内部（snapshot/config/thresholds/events）
            ──MQTT mcp_sensors/<id>/event──►  网关订阅 → MCP SSE
爱马仕 ──── POST /mcp ─────────────────────►  mcp_gw :9080
音频 ────── WebSocket PCM ─────────────────►  voice-bridge :8765
```

固件契约：`main/comm/coap_hub.h`、`mqtt_hub.h`、`mdns_hub` / `voice_discover`。

## 安装

```bash
cd mcp_gw
python -m venv venv
# Windows: venv\Scripts\pip install -r requirements.txt && venv\Scripts\python -m mcp_gw
# Linux:   source venv/bin/activate && pip install -r requirements.txt && python -m mcp_gw
```

复制 `.env.example` → `.env`。健康检查：`curl http://127.0.0.1:9080/health`。

## mDNS（对齐固件）

| 端 | 行为 |
|----|------|
| **设备** | 广播 `_mcp-sensors._udp` :5683，TXT `id`/`ver`/`name`；另 `_http._tcp` :80 |
| **网关** | 浏览 `_mcp-sensors`；广播 `_mcp-gw`（TXT `mqtt=` / 可选 `mqtt_user`/`mqtt_pass` / `ws=`）与 `_voice-ws` |

环境变量：`MCP_GW_MDNS=1`、`MCP_GW_ADVERTISE_IP`、`MCP_GW_VOICE_WS_PORT`（默认 8765）。

## MQTT（仅事件，对齐固件）

设备发布（前缀写死，不可改）：

- `mcp_sensors/<id>/event`（QoS1，retain=0）
- `mcp_sensors/<id>/availability`（online/offline，retain=1）

网关订阅后写入缓存并推 MCP `notifications/resources/updated`。

设备拿 broker：

1. 主路径：browse `_mcp-gw._tcp` TXT `mqtt=`（及可选 `mqtt_user`/`mqtt_pass`）
2. 兜底：网关 `POST http://设备/api/wifi`，`action=mqtt`，字段 `broker`/`user`/`password`

改 `.env` 里 broker/账号后：重启网关，或  
`curl -X POST http://网关:9080/api/mqtt/push -H 'Content-Type: application/json' -d '{"force":true}'`

`.env` 示例（独立网关，broker 可匿名）：

```env
MQTT_ENABLE=1
MQTT_HOST=127.0.0.1
MCP_GW_ADVERTISE_IP=192.168.1.193
MQTT_PORT=1883
MQTT_PREFIX=mcp_sensors
MQTT_CLIENT_ID=mcp-sensors-gw
```

`MQTT_PREFIX` 会被强制为 `mcp_sensors`（与固件一致）。broker 要登录时再自行设置环境变量 `MQTT_USER` / `MQTT_PASSWORD`（不必写进默认 `.env`）。

叶子可无 CoAP：按 event topic 自动建档，只暴露 `{id}__sensors.get_recent_events` 与事件资源。

## CoAP 工具（枢纽）

| MCP 工具 | 固件 |
|----------|------|
| `{id}__sensors.get_snapshot` | GET `/sensors/snapshot` |
| `{id}__sensors.get_thresholds` | GET `/sensors/thresholds` |
| `{id}__sensors.set_thresholds` | PUT `/sensors/thresholds` |
| `{id}__sensors.get_config` | GET `/sensors/config` |
| `{id}__sensors.set_config` | PUT `/sensors/config` |
| `{id}__sensors.get_recent_events` | MQTT 缓存，否则 GET `/sensors/events` |

资源：`{id}__sensor://events`。

## 爱马仕（`~/.hermes/config.yaml`）

```yaml
mcp_servers:
  mcp_sensors_gw:
    url: http://127.0.0.1:9080/mcp
    enabled: true
    connect_timeout: 30
    timeout: 120
    skip_preflight: true
```

只配本网关即可。不要配旧 HTTP MCP / `insensor` 桥。

## 排障

| 检查 | 命令 / 日志 |
|------|-------------|
| 健康 | `curl -s http://127.0.0.1:9080/health` |
| 已发现设备 | `curl -s http://127.0.0.1:9080/api/devices` |
| 网关日志 | `mDNS`、`MQTT subscribed`、`mqtt pushed`、`event #` |

Home Assistant 自定义集成见仓库 `homeassistant/custom_components/mcp_sensors/`（可选、独立部署）。
