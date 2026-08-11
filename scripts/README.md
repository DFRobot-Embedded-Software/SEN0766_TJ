# 运维脚本（主机默认 192.168.1.27 / haha）

可移植服务栈在仓库根目录 **`server_stack/`**（mcp_gw + voice-bridge + 启停脚本），整体拷贝即可。

| 脚本 | 用途 |
|------|------|
| `_deploy_server_stack.py` | 同步整个 `server_stack/` → 远端 `~/桌面/mcp-sensors-stack/` |
| `_restart_server_stack.py` | 远端停止旧进程并从统一目录重新启动 |
| `_deploy_mcp_gw.py` | 同上（兼容入口） |
| `_deploy_bridge.py` | 仅上传并重启 voice-bridge |
| `_tail_mcp_gw.py` | 查看远端 mcp_gw 日志 |
| `_setup_bridge.py` | 重建远端 venv 后启动 bridge |
| `_install_hermes.py` | 安装 Hermes + 配置 MCP/API Server |
| `_bridge_remote/` | 语音 bridge 源码镜像（开发用；正式源在 `server_stack/voice-bridge/`） |
| `fetch_yamnet_model.ps1` | 下载 YAMNet 权重 |
| `verify_yamnet_model.ps1` | 校验 YAMNet |
| `patch_esp_sr_movemodel_utf8.ps1` | ESP-SR 补丁 |

运行示例：

```powershell
.\tools\bin\python.cmd scripts\_deploy_server_stack.py
.\tools\bin\python.cmd scripts\_tail_mcp_gw.py
```

服务器上：

```bash
cd ~/桌面/mcp-sensors-stack
./start_services.sh
./status_services.sh
./stop_services.sh
```
