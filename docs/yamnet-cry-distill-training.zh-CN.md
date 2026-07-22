# YAMNet 哭声蒸馏模型训练说明（中文）

本文说明如何理解并复现类似 **[yamnet-cry-distill-int8](https://huggingface.co/chayuto/yamnet-cry-distill-int8)** 的训练流程：用大模型 YAMNet 作教师，蒸馏出可在 ESP32 上实时跑的小 INT8 学生网络。

| 项目 | 链接 |
|------|------|
| 训练源码 | [chayuto/yamnet-cry-distill-int8](https://github.com/chayuto/yamnet-cry-distill-int8) |
| 成品模型（HF） | [chayuto/yamnet-cry-distill-int8](https://huggingface.co/chayuto/yamnet-cry-distill-int8) |
| 教师 INT8（板端大模型） | [chayuto/yamnet-mel-int8-tflm](https://huggingface.co/chayuto/yamnet-mel-int8-tflm) |
| 参考固件 | [ws-ESP32-S3-CAM / cry-detect-01](https://github.com/chayuto/ws-ESP32-S3-CAM) |

> 本仓库当前默认跑的是全量 YAMNet INT8（约 4 MB）。蒸馏学生约 **110 KB**，输入仍是 **96×64 log-mel**，可与现有 mel 前端兼容后替换推理模型。

---

## 1. 训练在解决什么问题？

| | 教师 YAMNet | 学生（蒸馏后） |
|--|-------------|----------------|
| 参数量 | ~400 万 | ~8 万 |
| INT8 体积 | ~4.0 MB | ~110 KB |
| ESP32-S3 推理（参考） | ~500 ms / patch | ~28 ms / patch |
| Tensor arena | 数百 KB～1 MB+ | 约数十 KB |
| 目标 | AudioSet 521 类「参考答案」 | 近似教师行为，尤其哭声相关类 |

蒸馏的目标不是「比教师更准」，而是：**在资源受限设备上，用尽量少的算力复现教师对哭声等事件的判断**。

---

## 2. 整体流水线

```text
[公开 AudioSet 片段]  (+ 可选：私有现场录音)
        │
        ▼
  16 kHz 单声道 → log-mel（96 帧 × 64 mel）
        │
        ├──────────────────┐
        ▼                  ▼
  教师 YAMNet (FP32)    学生 DS-CNN
  输出 521 维 soft logits
        │                  │
        └──── KL 损失 ──────┘
                │
                ▼
        评估（F1 / AUC）
                │
                ▼
        全整型 PTQ → INT8 .tflite
```

### 2.1 特征（必须与板端一致）

与 Google YAMNet / 本工程 `yamnet_mel` 对齐：

- 采样率：16 kHz，单声道  
- 窗长 25 ms（400 点）、hop 10 ms（160 点）  
- 512 点 FFT，幅度谱 `|X|`  
- 64 个 mel 带，约 125–7500 Hz  
- `log(mel + 0.001)`  
- 拼成 **0.96 s** 的 patch：`[96, 64]`

板端与训练端 mel 不一致时，蒸馏再准也会在设备上失效。

### 2.2 学生网络结构（发表版）

- **DS-CNN**：普通卷积 + 若干 depthwise-separable 块 + Global Average Pool + Dense(521)  
- 参数约 **80,713**  
- 输出：**521 维 logits**（INT8 导出后仍为 521 维）  
- 哭声分数（部署常用）：

```text
cry_score = softmax(logits)[19] + softmax(logits)[20]
```

对应 AudioSet / YAMNet 类别：

- 19：`Crying, sobbing`  
- 20：`Baby cry, infant cry`

### 2.3 损失函数

对学生与教师在 **521 类 softmax** 上做：

```text
KL( teacher_softmax  ‖  student_softmax )
```

即让学生整表分布贴近教师，而不是只学一个二分类标签。

---

## 3. 数据准备

### 3.1 公开数据（可复现主结果）

仓库已提交 AudioSet 片段 ID（`data/ids/audioset_*.csv`），例如：

- 训练 / 验证 / 测试分段 ID  
- 正类：哭声、婴儿哭  
- 难负类：Speech、Babbling、Silence、室内环境、呻吟、尖叫等  

用脚本按 ID 从 YouTube / AudioSet 拉取（部分视频可能下架，属正常损耗）。

### 3.2 私有数据（可选增强）

作者用家庭设备录音做 **无监督增强**（只匹配教师 logits，不上传、不进公开产物）。  
**仅用公开 AudioSet 也能跑通蒸馏**；加私有数据一般能提升域内表现。

### 3.3 Teacher-as-filter（重要技巧）

不要把整段音频随机切窗都拿来训。先用教师扫窗：

| 类型 | 条件（发表流程量级） | 用途 |
|------|----------------------|------|
| 高置信正样本 | `p_cry > 0.30` | 进入训练 |
| 高置信负样本 | `p_cry < 0.05` | 进入训练 |
| 中间模糊窗 | 其余 | 丢弃 |

这样学生学的是「教师很有把握」的片段，比随机窗更稳。

### 3.4 正负比例与采样（EXP-007/008）

- 训练时正负比约 **1:3**（更接近真实部署，负样本远多于哭声）  
- 每个 epoch **重采样负样本**，避免总盯着同一批负例  
- 学习率：前期 `1e-3`，约 epoch 30 降到 `1e-4`

发表模型 **v0.2.0 = EXP-008**。

---

## 4. 环境与冒烟训练（官方复现）

建议 Linux / macOS 或 WSL；需要 Python 3，最好有 GPU。

```bash
git clone https://github.com/chayuto/yamnet-cry-distill-int8
cd yamnet-cry-distill-int8

python -m venv .venv
# Windows PowerShell:
#   .\.venv\Scripts\Activate.ps1
source .venv/bin/activate

pip install -e ".[dev,audioset]"
pytest

# 下载小规模 AudioSet 冒烟集（约 50 段，几分钟音频）
python scripts/download_audioset.py --ids data/ids/audioset_smoke.csv

# Phase-1：验证蒸馏环路能跑通
bash scripts/run_exp001_smoke.sh
```

全量训练请按仓库 `docs/experiments/` 中 EXP-006 → EXP-008 的说明，使用完整 `audioset_*.csv` ID 列表与对应训练脚本/配置（仓库仍在迭代，以最新 `README` 与 experiment 文档为准）。

### Windows 提示

- `bash scripts/run_exp001_smoke.sh` 需 Git Bash / WSL。  
- 下载 AudioSet 依赖网络与视频可用性，失败 ID 可跳过并记录存活率。

---

## 5. INT8 量化导出

1. 得到 FP32 学生权重。  
2. 用 **验证集** 上的 log-mel patch 作 representative dataset（发表版约 **200** 个 patch，且 **只用 val、不用 train/test/私有录音**）。  
3. `TFLiteConverter`：权重与激活全 INT8，输入为 mel patch，输出为 521 维。  
4. 对比 FP32 / INT8 的 AUC、best-F1；发表版量化损失很小（约 ΔF1 ≤ 0.012）。

产物示例：单个 `*.tflite`（约 110 KB），适合 SPIFFS / 嵌入固件，**不是** Edge Impulse 那种多文件 SDK。

---

## 6. 评估与部署阈值

蒸馏学生会继承教师「概率偏软」的特点：即使明显哭声，单类概率也可能 **远低于 0.5**。

| 指标（发表 INT8 量级） | 含义 |
|------------------------|------|
| AudioSet 测试 AUC ~0.87 | 排序能力 |
| best-F1 阈值约 **0.03–0.05** | 不要用默认 0.5 |
| 时间投票（如 5-of-9） | 降低夜间误报 |

**必须按现场标定**：录一段环境底噪，看分数上限，再设  
`threshold ≈ max(ambient_score) + 余量`，并配合连续帧投票 / 保持时间。

---

## 7. 改成「自己的事件」（报警 / 玻璃碎 / 说话等）

思路相同，换数据与关注的 class（或改学生头）：

1. 定教师：仍可用 YAMNet，或你自己的大模型。  
2. 定正负：AudioSet 标签 + 自采 WAV（16 kHz）。  
3. Teacher-as-filter：用教师对该事件的概率筛窗。  
4. 蒸馏：KL 到 521 维，或改成少类学生（需改网络与损失）。  
5. 量化导出 INT8 `.tflite`。  
6. 板端：复用 mel；加载新文件；盯对应 class id + 现场阈值。

若事件很少、数据很好标，也可不蒸馏，直接训小 DS-CNN 二分类/少类——流程更简单，但没有 521 类教师的「软知识」。

---

## 8. 与本工程（mcp_sensors）的关系

| 组件 | 说明 |
|------|------|
| `yamnet_mel.*` | 可继续用（需与训练 mel 一致） |
| `yamnet.tflite`（约 4 MB） | 教师级 INT8，慢但类全 |
| cry-distill `.tflite`（约 110 KB） | 学生，快；输出仍为 521，哭声看 19+20 |
| esp-tflite-micro + ESP-NN | 学生更小，更可能在 Optimized 下实时跑通 |

接入时注意：

- 核对输入 quant `scale` / `zero_point`（以学生模型为准）。  
- 输出若是 **logits**，板端需 **softmax** 再取 19+20；若导出已是概率，则按模型卡说明处理。  
- 发表学生与教师 INT8 的后处理不完全相同，以 [模型卡](https://huggingface.co/chayuto/yamnet-cry-distill-int8) 为准。

---

## 9. 常见问题

**Q: 必须有 GPU 吗？**  
冒烟可以 CPU；全量蒸馏强烈建议 GPU。

**Q: 和 Edge Impulse 比？**  
EI 偏「网页标注 → 导出 C 库」；本流程偏「教师蒸馏 → 单文件 tflite」，更贴当前 esp-tflite-micro 架构。

**Q: 训练要多久？**  
取决于数据量与 GPU；先跑 `run_exp001_smoke.sh` 验证环境，再开全量。

**Q: YouTube 下架怎么办？**  
按存活片段训练/评估即可，并在日志中记录存活率；公开复现允许一定 attrition。

**Q: 只有自己的哭声录音行不行？**  
可以作增强，但建议仍保留 AudioSet 等公开负样本/难例，否则易过拟合单一房间与麦克风。

---

## 10. 参考命令速查

```bash
# 克隆与安装
git clone https://github.com/chayuto/yamnet-cry-distill-int8
cd yamnet-cry-distill-int8
python -m venv .venv && source .venv/bin/activate   # 或 Windows Activate.ps1
pip install -e ".[dev,audioset]"

# 冒烟数据与训练
python scripts/download_audioset.py --ids data/ids/audioset_smoke.csv
bash scripts/run_exp001_smoke.sh

# 测试
pytest
```

更细的实验记录见上游：

- `docs/experiments/`  
- `docs/research/methodology-teacher-as-filter.md`  
- `docs/model_cards/yamnet-cry-distill-int8.md`

---

## 11. 许可与引用

- 蒸馏仓库与发表学生模型：以上游 **MIT** 及模型卡为准。  
- 教师权重来自 Google YAMNet（AudioSet），使用时请遵守其许可与引用要求。

```bibtex
@misc{yamnet-cry-distill-int8,
  author = {Orapinpatipat, Chayut},
  title  = {yamnet-cry-distill-int8: a distilled INT8 baby-cry detector for ESP32-S3},
  year   = {2026},
  howpublished = {\url{https://huggingface.co/chayuto/yamnet-cry-distill-int8}}
}
```

---

*文档根据上游 README / 模型卡整理，便于本仓库中文阅读；若与上游最新脚本冲突，以上游仓库为准。*
