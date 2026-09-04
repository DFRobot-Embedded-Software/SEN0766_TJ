# MCP Sensors 服务器栈（可整体拷贝移植）

```text
server_stack/
  start_services.sh      # 一键启动
  stop_services.sh       # 一键停止
  status_services.sh     # 看端口
  mcp_gw/                # MCP 聚合网关
  voice-bridge/          # 语音 WebSocket 中转
  logs/                  # 运行日志（自动创建）
```

> **网络要求：** 设备与运行本服务器栈的机器**必须在同一网段**，否则无法 mDNS / CoAP / 语音 WS。

## 移植到新机器

1. 拷贝整个 `server_stack` 目录（不要拷本地 `venv/`，到新机再装）
2. 启动脚本会在缺 `python3-venv` 时**自动 `sudo apt install`**（需可 sudo；可能提示输入密码）。也可预先手动安装：
   ```bash
   sudo apt update && sudo apt install -y python3-venv python3-pip
   ```
3. 安装爱马仕（只需一次）：
   ```bash
   curl -fsSL https://hermes-agent.nousresearch.com/install.sh | bash
   source ~/.bashrc
   hermes model          # 配置模型
   ```
4. 配置语音 bridge：复制 `voice-bridge/.env.example` 为 `voice-bridge/.env`：
   ```bash
   DASHSCOPE_API_KEY=sk-...     # 阿里云控制台申请
   HERMES_API_KEY=...           # 见下方说明（不是去官网申请）
   HERMES_API_BASE=http://127.0.0.1:8642
   ```

   **`HERMES_API_KEY` 怎么来？**  
   它就是你自己给爱马仕 API Server 设的密钥，必须与 `~/.hermes/.env` 里的 `API_SERVER_KEY` **完全相同**：
   ```bash
   # 若还没有，先给爱马仕设一把（任意字符串即可）
   grep -q '^API_SERVER_KEY=' ~/.hermes/.env 2>/dev/null || \
     echo 'API_SERVER_KEY=hermes-local-dev' >> ~/.hermes/.env
   # 确保 API Server 已开
   grep -q '^API_SERVER_ENABLED=true' ~/.hermes/.env 2>/dev/null || \
     echo 'API_SERVER_ENABLED=true' >> ~/.hermes/.env
   # 把同一把钥匙抄进 bridge
   grep '^API_SERVER_KEY=' ~/.hermes/.env
   # → 写入 voice-bridge/.env 的 HERMES_API_KEY=那一行
   ```
   `./start_services.sh` 若发现 bridge 的 `HERMES_API_KEY` 为空，会尝试自动从 `API_SERVER_KEY` 同步。
5. 启动：
   ```bash
   cd server_stack
   chmod +x *.sh
   ./start_services.sh
   ```

爱马仕 MCP 写入 `~/.hermes/config.yaml`（改完后重启 `hermes gateway` 或新开对话）：

```yaml
mcp_servers:
  mcp_sensors_gw:
    url: http://127.0.0.1:9080/mcp
    enabled: true
    connect_timeout: 30
    timeout: 120
    skip_preflight: true
```

- `mcp_sensors_gw`：本机聚合网关（Streamable HTTP）。`skip_preflight: true` 必填，否则爱马仕预检容易失败。
- 爱马仕里工具名形如 `mcp__mcp_sensors_gw__gw.list_devices`、`mcp__mcp_sensors_gw__{id}__sensors.get_snapshot`。没有名为 `read_resource` 的工具。

资源 URI 经网关后为 `{设备id}__sensor://events`。枢纽工具走 CoAP；仅 MQTT 的叶子由网关按 `mcp_sensors/<id>/event` 自动建档，只提供事件工具/资源。

- `resources/list` / `resources/read`：拉取
- `resources/subscribe` + `GET /mcp`（`Accept: text/event-stream` + `MCP-Session-Id`）：接收 `notifications/resources/updated` SSE 推送

## 端口

| 服务 | 端口 |
|------|------|
| mcp_gw | 9080 |
| voice-bridge | 8765 |
| 爱马仕 API | 8642 |
| MQTT broker（可选） | 本机或局域网 :1883 |

网关与 Home Assistant **互不依赖**。若要传感器进 HA，另拷 `homeassistant/custom_components/mcp_sensors`（可选）。音频只走 voice-bridge。

`start_services.sh` 顺序：mcp_gw → 爱马仕 → bridge（避免爱马仕启动时 MCP 尚未监听）。

## 从本仓库同步到服务器

```powershell
.\tools\bin\python.cmd scripts\_deploy_server_stack.py
```
