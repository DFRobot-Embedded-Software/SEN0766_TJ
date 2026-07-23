# YAMNet cry-distill（MCU INT8）+ 通用事件框架

板端只使用 **一个** 模型文件：

| 文件 | 说明 |
|------|------|
| `yamnet_cry_distill.tflite` | 蒸馏学生 ~110 KB |
| `yamnet_class_map.csv` | 521 类显示名 |

## 架构

目录按职责隔离（可单独替换某一层）：

```text
main/audio/
  capture/   audio_capture     ← 唯一碰 ES7210 / 数字增益 / 多 sink
  stream/    audio_stream      ← UDP 透传（独立订阅 capture）
  wake/      esp_sr_wake       ← esp-sr AFE + WakeNet
  yamnet/    pcm_in / mel / events / classifier
```

```text
audio_capture
     │ multi-sink
     ├─ yamnet_pcm_in → mel → TFLite → events
     ├─ esp_sr_wake (AFE feed/fetch) → 唤醒后 audio_stream_start()
     └─ audio_stream（可选 UDP PCM）
```

关注哪些声音、用哪些 class id、阈值与投票：**只改** `main/audio/yamnet/yamnet_events.c` 里的 `s_event_table`。

远场：`ES7210_MIC_GAIN_DB` + `AUDIO_CAPTURE_DIGITAL_GAIN`。

### 语音唤醒（esp-sr）

1. `idf.py add-dependency` 已含 `espressif/esp-sr`；`config/partitions.csv` 有 `model` 分区（4M）。
2. menuconfig：
   - **ESP-SR 语音唤醒** → 启用 `ESP_SR_WAKE_ENABLE`
   - **ESP Speech Recognition** → 选 WakeNet，例如 `wn9_hilexin`（说「Hi 乐鑫」）
   - 可选：启用 `AUDIO_STREAM_ENABLE` + `ESP_SR_WAKE_START_STREAM`（唤醒后 UDP 透传）
3. 全量烧录：`idf.py build flash`（会把选中的 SR 模型写入 `model` 分区）
4. 串口出现 `WAKEWORD DETECTED` 即成功；若开了透传则自动 `audio_stream_start()`。

## 添加新事件示例

在 `yamnet/yamnet_events.c` 的表中追加一行（class id 见 `yamnet_class_map.csv`）：

```c
{
    .id = "glass",
    .log_name = "GLASS",
    .class_ids = {437, 463, 464, 435},  /* Shatter / Smash / Breaking / Glass */
    .class_count = 4,
    .score_thr_milli = 120,             /* 0.120 */
    .vote_window = 3,
    .vote_need = 2,
    .hold_ms = 3000,
    .suppress_if_speech = false,
},
```

应用层可注册：

```c
yamnet_classifier_set_event_cb(my_cb, NULL);
// my_cb(event_id, score, votes, ctx)  — event_id 为 "cry" / "glass" …
```

## 下载 / 更新模型

```powershell
.\scripts\fetch_yamnet_model.ps1
# 或覆盖 models/yamnet/yamnet_cry_distill.tflite 后:
idf.py build flash
```

## 替换模型约定

- 输入：`[1,96,64]` INT8 log-mel  
- 输出：`[1,521]` INT8 logits（固件 softmax）  
- 若类别语义变了：更新 `yamnet_events.c` 的 class_ids，不必改框架代码  

## 哭声 / 玻璃碎（默认已启用）

| id | 日志 | 默认阈值 | 投票 |
|----|------|----------|------|
| cry | CRY_ALERT | 0.18 | **2/5**（Speech 仅在明显更高时压制） |
| glass | GLASS_ALERT | 0.12 | 2/3 |
| alarm | ALARM_ALERT | 0.15 | 3/5 |

现场微调阈值：改表中 `score_thr_milli`，或 menuconfig 里默认 vote/hold。

训练说明见 [docs/yamnet-cry-distill-training.zh-CN.md](../../docs/yamnet-cry-distill-training.zh-CN.md)。
