# MCP Sensors Gateway (`mcp_gw`)

独立进程：把局域网多台设备 MCP 聚合成 **一个** Streamable HTTP 入口，供爱马仕固定配置。

```text
设备 ──mDNS browse _mcp-gw._tcp──► 发现注册 URL
     ──POST /api/devices/register──► mcp_gw :9080
                                        │
爱马仕 ────────── POST /mcp ─────────────┘（只配这一条）
HA MQTT  ◄── discovery + state/event ──┘（可选，音频不走这里）
```

## 安装

```bash
cd mcp_gw
python -m venv venv
# Windows: venv\Scripts\pip install -r requirements.txt && venv\Scripts\python -m mcp_gw
# Linux:   source venv/bin/activate && pip install -r requirements.txt && python -m mcp_gw
```

可选：复制 `.env.example` → `.env`。

健康检查：`curl http://127.0.0.1:9080/health`（含 `mdns` 字段）。

## mDNS 自动发现

| 端 | 行为 |
|----|------|
| **网关** | 启动后广播 `_mcp-gw._tcp`，端口=`MCP_GW_PORT`，TXT `path=/api/devices/register` |
| **设备** | `mdns_query_ptr("_mcp-gw","_tcp")` → `http://{ip}:{port}{path}` → register |
| **回退** | 发现失败则用 Kconfig `MCP_GW_REGISTER_URL` |

环境变量：

- `MCP_GW_MDNS=1`（默认开）
- `MCP_GW_ADVERTISE_IP=192.168.0.73`（多网卡时建议写死）
- `MCP_GW_REGISTER_PATH=/api/devices/register`

固件：menuconfig → **5. MCP 聚合网关** → `MCP_GW_MDNS_DISCOVER`。

## Home Assistant MQTT

网关把已注册设备的传感器/事件推到 HA，用官方 [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) **自动创建设备和实体**。轮询设备工具 `sensors.get_snapshot`、`sensors.get_recent_events`（默认 15 秒）。**语音 WebSocket 不走 MQTT**，仍连 voice-bridge。

自动：发现实体、刷新数值、新事件、掉线 `unavailable`、HA 重启后重发 Discovery。  
需手配一次：Mosquitto、MQTT 用户、网关 `.env`。仪表盘卡片和手机通知要在 HA 里自己加。

### 网络前提

网关机与 HAOS **必须能互相访问**（同一网段或已打通路由）。在跑 `mcp_gw` 的机器上：

```bash
ping <HAOS局域网IP>
nc -vz <HAOS局域网IP> 1883
```

不通时 `health` 里会是 `mqtt.connected: false`，错误常见 `No route to host`。HA 网页能开、不代表网关也能连 1883（有线/Wi‑Fi 隔离时经常这样）。

HA 网页不一定是 `:8123`。有的 HAOS 只开 **80**（如 `http://192.168.1.210/`），8123 会拒绝。MQTT 端口仍是 **1883**。

局域网里可能有多台 Home Assistant。`MQTT_HOST` 必须填**你能登录的那台**的 IP，不要填碰巧也开着 8123/1883 的别的机器。

### 1. HAOS：Mosquitto + MQTT 集成

1. 用浏览器打开你的 HA（示例：`http://192.168.1.210/`）。
2. **设置 → 应用**（旧界面叫加载项 / Add-ons）→ 安装 **Mosquitto broker**，启动它。
3. Mosquitto → **配置 / Configuration** → **Logins** 里给网关单独加一条（自己设用户名和密码），保存并重启 Mosquitto。
4. **设置 → 设备与服务 → 添加集成 → MQTT**。Broker 一般填本机（HA 连自己的 Mosquitto）。  
   HA 集成会自动生成一组密钥，**那组不要抄到网关**。网关只用第 3 步 Logins 里新建的账号。
5. MQTT 集成默认开启 Discovery，前缀 `homeassistant`。不要关 Discovery，也不要改前缀（除非同步改网关 `MQTT_DISCOVERY_PREFIX`）。

网页登录用的是 **HA 用户**，和 `MQTT_USER` 不是一回事。

### 2. 网关 `.env`

在 **跑 mcp_gw 的那台机器**上改 `mcp_gw/.env`（仓库里的 `.env.example` 只是模板）。  
`scripts/_deploy_server_stack.py` **不会覆盖**远端已有 `.env`，同步代码后仍要在服务器上改这一份。

```env
MQTT_ENABLE=1
MQTT_HOST=192.168.1.210          # HAOS 局域网 IP，不是 127.0.0.1
MQTT_PORT=1883
MQTT_USER=mosquitto里Logins的用户名
MQTT_PASSWORD=对应密码
MQTT_PREFIX=mcp_sensors          # 状态/事件主题前缀
MQTT_DISCOVERY_PREFIX=homeassistant
MQTT_POLL_S=15                   # 拉快照间隔（秒）
MQTT_CLIENT_ID=mcp-sensors-gw    # 连 broker 的客户端 ID，同一 broker 上不要重复
```

`MQTT_ENABLE` 默认在设置了 `MQTT_HOST` 时为开。关掉：`MQTT_ENABLE=0` 或清空 `MQTT_HOST`。

改完必须重启 mcp_gw（只保存 `.env` 不会生效）：

```bash
cd ~/桌面/server_stack   # 或你的实际目录
./stop_services.sh
./start_services.sh
```

`start_services.sh` 若发现缺 `aiomqtt` 会重装依赖。已在跑的进程不会读新 `.env`。

### 3. 确认连上

在网关本机：

```bash
curl -s http://127.0.0.1:9080/health
```

`mqtt` 字段应为：`"host":"<HAOS IP>","connected":true`。  
`devices_online` ≥ 1 后才会持续发状态（设备要先注册到网关）。

日志关键字（`logs/mcp_gw.log`）：

- `MQTT connected broker=...` — 已登录
- `MQTT discovery id=... topic=homeassistant/device/.../config` — 已向 HA 注册实体
- `MQTT 断开` / `No route to host` — 网络
- 认证失败 — 用户名密码不是 Mosquitto Logins 那一组，或未重启 Mosquitto

### 4. 在 HA 里看数据

1. **设置 → 设备与服务 → MQTT → 配置**
2. **监听主题**：`mcp_sensors/#`（状态/事件）或 `homeassistant/device/#`（Discovery）
3. **MQTT → 设备**：每个已注册传感器节点一台设备（id 如 `f20e08`）

| 实体 | 来源 |
|------|------|
| 温度 / 湿度 / CO2 / 光照 / 声级 | `sensors.get_snapshot` |
| 人数、占用、声报警 | 由快照里的区域/声级推算 |
| Device event | 新的 `sensors.get_recent_events`（哭声、闯入、阈值等）；首次轮询只记水位，不刷历史 |

主题：

| 主题 | 作用 |
|------|------|
| `homeassistant/device/<id>/config` | Discovery（retain） |
| `mcp_sensors/<id>/state` | JSON 状态（retain） |
| `mcp_sensors/<id>/availability` | `online` / `offline` |
| `mcp_sensors/<id>/event` | 事件（不 retain，避免 HA 重启重放） |
| `homeassistant/status` | 网关订阅 HA birth，收到 `online` 后重发 Discovery |

仪表盘：把上述实体加到 Lovelace。自动化示例：`event` 的 `event_type` 为 `sound_cry` 时 `notify.mobile_app_...`。

## 爱马仕配置（只需一次）

```yaml
mcp_servers:
  mcp_sensors_gw:
    url: "http://127.0.0.1:9080/mcp"
    enabled: true
    connect_timeout: 30
    timeout: 120
```

`/reload-mcp` 或重启爱马仕。

## 日志看哪里

### 网关（随 `server_stack` 部署）

| 内容 | 位置 |
|------|------|
| 主日志 | `~/桌面/mcp-sensors-stack/logs/mcp_gw.log` |
| 实时尾部 | `tail -f ~/桌面/mcp-sensors-stack/logs/mcp_gw.log` |
| 健康/mDNS | `curl -s http://127.0.0.1:9080/health` |
| 已注册设备 | `curl -s http://127.0.0.1:9080/api/devices` |

关注关键字：`mDNS advertised`、`registered device`、`refresh failed`。

本地 Windows 前台跑时：日志直接打在终端（`python -m mcp_gw`）。

### 设备（串口）

TAG：`mcp_gw`

| 日志 | 含义 |
|------|------|
| `mDNS found gateway ... url=` | 发现成功 |
| `mDNS browse _mcp-gw failed` / `discover miss` | 发现失败，将用回退 URL |
| `registered id=... -> http://...` | 注册成功 |
| `register failed ...` | 注册失败（网关未起 / 跨网段 / token） |
| `heartbeat failed` | 心跳失败，会重试 register |

### 爱马仕

MCP 连接问题看爱马仕自身日志。

| 类型 | 命名 |
|------|------|
| 工具 | `{id}__sensors.get_snapshot` |
| 资源 | `{id}__sensor://events`、`{id}__sensor://temperature`、`{id}__sensor://area/0/events` |

爱马仕侧：

1. `resources/list` / `resources/read`
2. `resources/subscribe` 后对 `GET /mcp` 打开 SSE（带 `MCP-Session-Id`），接收 `notifications/resources/updated`

设备 SSE 不可用时，网关会回退为短周期 `resources/read` 轮询并在内容变化时推送。

## 手动注册（调试）

```bash
curl -sS -X POST http://127.0.0.1:9080/api/devices/register \
  -H "Content-Type: application/json" \
  -d '{"id":"a1b2c3","name":"客厅","ip":"192.168.0.50","port":8080,"path":"/mcp_server"}'
```

## 聚合名

| 网关入口 | 说明 |
|----------|------|
| `gw.list_devices` | 在线设备列表 |
| `{id}__sensors.get_snapshot` 等 | 转发设备工具 |
| `{id}__sensor://events` 等 | 转发设备资源（list/read/subscribe+SSE） |

## 部署到语音机

```powershell
.\tools\bin\python.cmd scripts\_deploy_server_stack.py
```

远端目录：`~/桌面/mcp-sensors-stack/`（与 voice-bridge 同目录，便于整体移植）。

## 后续（未做）

- `tools/list_changed` SSE 推送
