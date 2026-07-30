# WebSocket 音频约定（未实现）

后续用 WebSocket 替换现行 UDP [`audio_stream`](audio_stream.h) 透传；**本阶段仅约定，固件未实现**。

## 路径

- 建议：`ws://<device>/ws/audio`（与 Web Dashboard HTTP :80 同源；或独立端口，实现时再定）

## 上行（设备 → 客户端，麦克风透传）

- Binary 帧：16-bit LE PCM，16 kHz mono
- 控制（JSON text 帧），开始前发送：

```json
{"op":"uplink_start","rate":16000,"format":"s16le","channels":1}
```

- 停止：`{"op":"uplink_stop"}`

## 下行（客户端 → 设备播放）

- Binary 帧：同上 PCM 格式
- 控制：`{"op":"play_start"}` / `{"op":"play_stop"}`

## 与事件总线的关系

- WebSocket **只传媒体**，不承载分类告警
- YAMNet / 声级告警仍走 `device_event_emit(category=sound)` → `sensor://events` / `/api/events`
- 唤醒后开始上行：由现有 wake 回调打开 WS session，而不是往事件总线塞 PCM

## 迁移

1. 实现 `/ws/audio` 与上/下行会话
2. 将 `esp_sr_wake` / 业务触发从 `audio_stream_start()` 切到 WS
3. 保留 `CONFIG_AUDIO_STREAM_ENABLE` UDP 路径作回退，确认稳定后删除
