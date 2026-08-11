# MCP Sensors Gateway (`mcp_gw`)

独立进程：把局域网多台设备 MCP 聚合成 **一个** Streamable HTTP 入口，供爱马仕固定配置。

```text
设备 ──mDNS browse _mcp-gw._tcp──► 发现注册 URL
     ──POST /api/devices/register──► mcp_gw :9080
                                        │
爱马仕 ────────── POST /mcp ─────────────┘（只配这一条）
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
