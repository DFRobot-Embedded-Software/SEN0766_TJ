# WebSocket 音频客户端（设备 → 网关）

设备作为 **WebSocket 客户端** 连接网关 URI（`CONFIG_AUDIO_WS_URI`），唤醒后上行麦克风 PCM，接收下行 PCM 经 NS4168 播放。

实现：[`audio_ws_client.c`](audio_ws_client.c)（`CONFIG_AUDIO_WS_ENABLE`）。

## 连接

- 优先：入网后通过 mDNS `_mcp-gw._tcp` 的 TXT `ws=` 自动发现（与 MCP 注册同一广播）
- 回退：`CONFIG_AUDIO_WS_URI`（默认 `ws://192.168.1.27:8765/ws`）
- 串口关键字：`audio_ws: using gateway URI:` / `mcp_gw: mDNS voice WS:` / `mcp_gw: voice WS URI:`
- 可选鉴权：`CONFIG_AUDIO_WS_AUTH_HEADER`（完整头行，如 `Authorization: Bearer <token>`）

## 上行（设备 → 网关，麦克风）

- Binary 帧：16-bit LE PCM，16 kHz mono
- 开始（JSON text）：

```json
{"op":"uplink_start","rate":16000,"format":"s16le","channels":1}
```

- 结束：

```json
{"op":"uplink_stop","reason":"vad"|"max"|"error"|"manual"}
```

- 停说：AFE VAD 连续静音 ≥ `CONFIG_AUDIO_WS_EOS_MS`（默认 1000ms）→ `reason=vad`
- 兜底：单次上行最长 `CONFIG_AUDIO_WS_MAX_MS`（默认 15s）→ `reason=max`
- **`uplink_stop` 后保持连接**，等待下行 TTS；`play_stop` 播完后**仍保持连接**
- **禁止每轮 destroy/重连**（避免下行 RST 截断、二次建任务失败）
- **禁止自动重连**；上行/等下行/播放期间忽略再次唤醒，避免反复开 ASR 被限流
- VAD：须先检测到语音，再累计静音才 `uplink_stop`

## 续听窗口与多轮上下文

- `CONFIG_AUDIO_WS_FOLLOWUP_MS`（默认 8000；`0` = 关闭）
- TTS 排空后：
  - `FOLLOWUP_MS > 0`：进入续听窗口（LED 听状态）。窗口内 AFE 检测到语音或再次唤醒 → 直接 `uplink_start`，**无需唤醒词**。
  - 窗口超时仍无语音 → 发 `{"op":"dialog_idle"}`，回到需唤醒；网关丢弃爱马仕 `X-Hermes-Session-Id`。
  - `FOLLOWUP_MS == 0`：播完立即 `dialog_idle`（每轮需唤醒；无跨轮会话）。
- 续听窗口**不算 busy**；播放/等待下行仍算 busy（本阶段无 barge-in）。
- 开上行后若长时间无有效语音，用 `FOLLOWUP_MS` 短超时 abort，避免空挂 ASR。
- **退出对话**：由爱马仕判定自然语言退出意图（告别/先这样/可以了等）；口令列表仅作兜底。判定退出后播告别语并下发 `{"op":"dialog_end"}`，设备不再续听。

网关侧：多轮上下文由爱马仕 SessionDB 维护（请求头 `X-Hermes-Session-Id`）；bridge 只保存 session id，不本地拼 `messages`。`dialog_idle` / `dialog_end` / 断线时清空 id；漏发时有 `DIALOG_IDLE_FALLBACK_S` 兜底。

## 下行（网关 → 设备播放）

- Binary 帧：同上 PCM → `ns4168_amp_write`
- 控制：`{"op":"play_start"}` / `{"op":"play_stop"}`
- 播放期间不因唤醒重开上行
- 网关应在 TTS 播完后发 `play_stop`，否则设备按下行等待超时处理

## 与事件总线的关系

- WebSocket **只传媒体**，不承载分类告警
- YAMNet / 声级告警仍走 `device_event_emit(category=sound)` → `sensor://events` / `/api/events`
- 唤醒后开始上行：`ESP_SR_WAKE_START_STREAM` 优先 WS；未开 `AUDIO_WS` 时回退 UDP `audio_stream`

## 配置

menuconfig → 音频算法 → **WebSocket PCM 客户端**：

| 选项 | 说明 |
|------|------|
| `AUDIO_WS_ENABLE` | 启用客户端 |
| `AUDIO_WS_URI` | 网关地址 |
| `AUDIO_WS_AUTH_HEADER` | 可选 Authorization |
| `AUDIO_WS_EOS_MS` | VAD 静音判停说 |
| `AUDIO_WS_MAX_MS` | 单次上行上限 |
| `AUDIO_WS_DOWNLINK_WAIT_MS` | 下行**静默看门狗**：无 `processing`/`play`/`PCM` 超过此时长才放弃等待；网关处理中会心跳续命 |
| `AUDIO_WS_FOLLOWUP_MS` | 播完后续听窗口；`0` 关闭自动续听 |

UDP 路径（`AUDIO_STREAM_ENABLE`）仍保留作回退。
