# MCP Sensors

基于 ESP-IDF 的传感器监控项目，运行在 **ESP32-P4** 上。设备通过 SoftAP 配网，连接路由器后在局域网内提供 Web 监控页面。

## 功能概览

- **AP 配网**：首次启动创建热点 `MCP-Sensors-Setup`，浏览器访问 `http://192.168.4.1` 配置 WiFi
- **STA 联网**：保存 WiFi 凭据到 NVS，自动连接路由器并获取局域网 IP
- **Web 监控**：同网段设备通过 `http://<设备IP>` 访问环境监测与人员活动状态页面
- **C4004 区域检测**：通过 UART 连接 DFRobot C4004 毫米波雷达，支持人流统计、边缘门、靠近/远离等策略
- **MCP SDK**：集成 Espressif MCP C SDK，便于后续扩展 AI 工具调用能力

## 硬件要求

| 项目 | 说明 |
|------|------|
| 主控 | ESP32-P4（无内置 WiFi） |
| WiFi 协处理器 | ESP32-C6，通过 **SDIO** 与 P4 通信 |
| 推荐开发板 | ESP32-P4-Function-EV-Board（C6 已预烧录 ESP-Hosted 固件） |

> ESP32-P4 本身不支持 WiFi，必须配合 C6 及 ESP-Hosted 方案使用。若使用官方 Function EV Board，SDIO 引脚已在硬件上接好，menuconfig 中选对应板型即可。

### ESP32-P4-Function-EV-Board 默认 SDIO 引脚（P4 ↔ C6）

| 信号 | GPIO |
|------|------|
| CLK | 18 |
| CMD | 19 |
| D0 | 14 |
| D1 | 15 |
| D2 | 16 |
| D3 | 17 |
| Reset | 54 |

## 软件环境

### 1. 安装 ESP-IDF

推荐使用 **ESP-IDF v5.4 及以上**（本项目已在 v5.5.1 下验证）。

**Windows（安装器方式，推荐）：**

1. 下载 [ESP-IDF Windows 安装器](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/windows-setup.html)
2. 安装时选择 ESP32-P4 工具链
3. 从开始菜单打开 **ESP-IDF PowerShell** 或 **ESP-IDF CMD**

**手动安装：**

```bash
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32p4        # Windows: install.bat esp32p4
. ./export.sh               # Windows: export.bat
```

### 2. 激活环境（每次新开终端）

```powershell
# 路径按实际安装位置修改
D:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1
```

### 3. 设置目标芯片

```powershell
cd mcp_sensors
idf.py set-target esp32p4
```

## 项目依赖

### 组件管理器依赖（`main/idf_component.yml`）

| 组件 | 版本 | 用途 |
|------|------|------|
| `idf` | `>=4.1.0` | ESP-IDF 框架 |
| `espressif/esp_hosted` | `*` | P4 通过 SDIO 使用 C6 的 WiFi |
| `espressif/esp_wifi_remote` | `*` | 远程 WiFi API 封装，兼容 `esp_wifi` 调用 |
| `espressif/mcp-c-sdk` | `^2.0.1` | MCP 协议 C SDK |
| `espressif/mdns` | `^1.0.3` | mDNS 服务（`mcp-sensors.local`） |
| `vrxiaojie/stcc4` | `^1.0.0` | DFRobot STCC4 CO2/温湿度 I2C 驱动 |

首次编译时，组件管理器会自动下载依赖到 `managed_components/` 目录。

手动添加依赖（可选）：

```powershell
idf.py add-dependency "espressif/esp_hosted"
idf.py add-dependency "espressif/esp_wifi_remote"
idf.py add-dependency "espressif/mcp-c-sdk^2.0.1"
```

### ESP-IDF 内置组件（`main/CMakeLists.txt`）

| 组件 | 用途 |
|------|------|
| `nvs_flash` | 保存 WiFi 凭据 |
| `esp_wifi` | WiFi AP/STA（经 esp_wifi_remote 桥接） |
| `esp_event` | WiFi / IP 事件 |
| `esp_netif` | 网络接口 |
| `esp_http_server` | 配网页与监控页 HTTP 服务 |
| `driver` | I2C 总线（STCC4）、UART（C4004） |
| `json` | Web API JSON 响应 |
| `spiffs` | 静态网页分区 |
| `mdns` | 局域网域名解析 |

## menuconfig 配置（ESP32-P4 WiFi 必做）

```powershell
idf.py menuconfig
```

按以下路径检查配置：

| 配置项 | 路径 | 建议值 |
|--------|------|--------|
| 关闭 Host WiFi | `Component config → Wi-Fi` | **不要**勾选 `Host WiFi Enable` |
| 协处理器 | `Component config → Wi-Fi Remote → WiFi configuration` | 选择 **ESP32-C6** |
| 传输层 | `Component config → ESP-Hosted config → Transport layer` | **SDIO** |
| 开发板 | `Component config → ESP-Hosted config` | 勾选 `ESP32-P4 Function EV Board`（若使用官方板） |
| SDIO 引脚 | `Component config → ESP-Hosted config → Hosted SDIO Configuration` | 使用默认值或按硬件修改 |
| C4004 UART | `Component config → C4004 毫米波雷达` | TX=50, RX=51, 115200（见下文接线） |
| STCC4 I2C | `Component config → STCC4 环境传感器` | SDA=7, SCL=8（见下文接线） |

> 若 `Wi-Fi Remote → Wi-Fi configuration` 菜单不可见，请确认 IDF 版本 ≥ 5.4，且 `Host WiFi Enable` 已关闭。

可选：在项目根目录创建 `sdkconfig.defaults.esp32p4` 固化配置：

```ini
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y
```

## 编译与烧录

```powershell
idf.py build
idf.py -p COMx flash monitor
```

将 `COMx` 替换为实际串口号（设备管理器中查看）。

常用命令：

```powershell
idf.py fullclean          # 完全清理构建
idf.py menuconfig         # 修改配置
idf.py erase-flash        # 擦除 Flash（含 NVS 配网信息）
idf.py size               # 查看固件大小
```

## 使用说明

### 首次配网

1. 设备上电，串口日志出现 `AP started: SSID=MCP-Sensors-Setup`
2. 手机/电脑连接热点 **MCP-Sensors-Setup**（开放网络，无密码）
3. 浏览器打开 **http://192.168.4.1**
4. 填写路由器 WiFi 名称和密码，点击保存
5. 设备连接路由器，串口打印 `got IP: 192.168.x.x`

### 访问监控页

在同一局域网内，浏览器访问：

```
http://192.168.x.x
```

页面包含环境监测数据（温湿度、CO2）和人员活动状态，数据通过 `/api/data` 接口每 **1 秒** 刷新。STCC4 上电后约需 20 秒预热，预热期间页面显示「预热中…」。

### STCC4 传感器接线

默认引脚适用于 **ESP32-P4-Function-EV-Board**（可在 `idf.py menuconfig` → **STCC4 环境传感器** 中修改）：

| STCC4 引脚 | ESP32-P4 |
|------------|----------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO7 |
| SCL | GPIO8 |

- I2C 地址默认 **0x64**（模块跳线改为 0x65 时需同步修改 menuconfig，且当前 `vrxiaojie/stcc4` 驱动内部固定为 0x64）
- 串口日志出现 `CO2=xxx ppm, T=xx.x°C, RH=xx.x%` 表示采样正常
- 若启动报 `STCC4 not found`，检查接线、上拉与供电

### C4004 毫米波雷达接线

C4004 通过 **UART** 与 ESP32-P4 通信（可在 `idf.py menuconfig` → **C4004 毫米波雷达** 中修改引脚与波特率）：

| C4004 引脚 | ESP32-P4 | 说明 |
|------------|----------|------|
| VCC | 5V | 按模块标注供电（与 P4 共地） |
| GND | GND | 共地 |
| TX | **GPIO51** | 雷达发送 → P4 接收 |
| RX | **GPIO50** | 雷达接收 ← P4 发送 |

> **注意交叉连接**：P4 的 TX（GPIO50）接 C4004 的 RX，P4 的 RX（GPIO51）接 C4004 的 TX。

默认通信参数：

| 参数 | 默认值 |
|------|--------|
| 波特率 | 115200 |
| UART 端口 | UART1 |
| TX GPIO | 50 |
| RX GPIO | 51 |

- 串口日志出现 `C4004 ready (UART1 TX=50 RX=51 115200bps)` 表示通信正常
- 在 Web 页面 **设备数据** 中启用区域配置并添加标签后，配置会自动同步到雷达
- 若启动报 `C4004 not responding`，检查 TX/RX 是否接反、供电与 GND 是否可靠

### YAMNet 声音事件分类（方案 A）

使用 MCU 专用 INT8 模型（[yamnet-mel-int8-tflm](https://huggingface.co/chayuto/yamnet-mel-int8-tflm)），**不是** Kaggle 官方带图内 FFT 的 `yamnet.tflite`。

1. 下载模型到 `models/yamnet/`：

```powershell
.\scripts\fetch_yamnet_model.ps1
```

2. 打开功能并确认 PSRAM：

```powershell
idf.py menuconfig
```

- `Component config → ESP PSRAM`：启用 SPIRAM（`sdkconfig.defaults.esp32p4` 已默认打开）
- `YAMNet 声音事件分类`：勾选 **启用 YAMNet**
- 音频来源：默认 **ES8388**；无麦验证可选 Fake
- ES8388 默认与 STCC4 **共用 I2C0**（SDA/SCL 见 STCC4 配置），I2C 地址 `0x10`
- I2S 默认：BCLK=12 / WS=10 / DIN=9 / MCLK=13（请按你的接线改 menuconfig）

3. 全量烧录（含 `yamnet` 模型分区）：

```powershell
idf.py build flash monitor
```

串口应看到 `ES8388 record ready` 与周期性 `infer xxx ms | #1 ...`。

> 若 ES8388 接在独立 I2C，关闭「与 STCC4 共用 I2C0」并设置 SDA/SCL。ADDR 接高时地址改为 `0x11`。

### 修改网页（无需改 C 代码）

前端文件位于 `spiffs/` 目录，可直接编辑：

```
spiffs/
├── index.html          # 监控与配置页（含设备数据 / 区域 / WiFi 三个 Tab）
├── css/style.css       # 样式
├── js/dashboard.js     # 页面逻辑与 API 调用
└── icons/*.svg         # 本地图标（离线可用）
```

修改后重新编译烧录（SPIFFS 镜像会一并打包）：

```powershell
idf.py build flash
```

仅更新网页分区（不重新烧录 app）：

```powershell
idf.py spiffs-flash
```

本地预览网页（API 不可用）：

```powershell
python -m http.server 8080 --directory spiffs
```

### 访问地址

| 场景 | 地址 |
|------|------|
| 首次配网（AP 模式） | `http://192.168.4.1` |
| 已联网（推荐） | `http://mcp-sensors.local` |
| 已联网（IP） | 串口日志中的 `got IP: x.x.x.x` |

### 重新配网

擦除 NVS 后重新烧录或调用代码中的 `wifi_manager_clear_config()` 并重启：

```powershell
idf.py erase-flash
idf.py flash
```

## 项目结构

```
mcp_sensors/
├── main/                       # 固件源码（ESP-IDF main 组件）
│   ├── app/                    # 程序入口
│   │   └── mcp_sensors.c
│   ├── wifi/                   # WiFi 配网、STA 连接、Captive Portal
│   │   ├── wifi_manager.c/h
│   │   └── captive_portal.c/h
│   ├── web/                    # HTTP 服务与 SPIFFS 文件系统
│   │   ├── web_server.c/h
│   │   └── web_fs.c/h
│   ├── sensor/                 # 传感器数据与区域配置
│   │   ├── sensor_data.c/h
│   │   ├── stcc4_sensor.c/h    # STCC4 I2C 驱动封装
│   │   └── c4004_sensor.c/h    # C4004 UART 驱动封装
│   ├── CMakeLists.txt
│   └── idf_component.yml       # 组件管理器依赖
├── spiffs/                     # Web 静态资源（打包进 SPIFFS 分区）
│   ├── index.html
│   ├── css/style.css
│   ├── js/dashboard.js
│   └── icons/
├── config/
│   └── partitions.csv          # Flash 分区表
├── sdkconfig.defaults          # 默认 Kconfig（分区表、ESP-Hosted 等）
├── sdkconfig.defaults.esp32p4  # ESP32-P4 目标专用默认配置
├── CMakeLists.txt              # 顶层构建 + SPIFFS 镜像
└── README.md
```

## Web API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 监控与配置页（HTML） |
| GET | `/css/*` `/js/*` `/icons/*` | 静态资源 |
| GET | `/api/data` | 传感器数据 JSON |
| GET | `/api/config` | 区域配置 JSON |
| POST | `/api/config` | 保存区域配置 |
| GET | `/api/wifi` | 当前 WiFi 配置与连接状态 |
| POST | `/api/wifi` | 保存 WiFi 并连接 |

## 常见问题

**Q: 启动崩溃 `esp_wifi_init` / `Failed to deinit Wi-Fi driver (0x3001)`？**

ESP32-P4 没有内置 WiFi。请确认 menuconfig 中 **未开启** `Component config → Wi-Fi → Host WiFi Enable`，并已启用 `ESP-Hosted` + `Wi-Fi Remote`。然后执行：

```powershell
idf.py fullclean reconfigure build flash
```

**Q: 编译报 `unknown type name 'wifi_twt_config_t'`？**

这是 `esp_hosted` 开启了 iTWT 功能，但当前 WiFi 头文件未包含对应类型。本项目已在 `sdkconfig.defaults` 中关闭 iTWT。若仍报错，执行：

```powershell
idf.py fullclean reconfigure build
```

或在 menuconfig 中关闭：`Component config → ESP-Hosted config → Enable iTWT`。

**Q: 编译报 WiFi 相关 undefined reference？**

确认 `Host WiFi Enable` 已关闭，且 `Wi-Fi Remote → Wi-Fi configuration` 中已配置缓冲区参数。

**Q: WiFi 初始化成功但无法连接？**

- 确认 C6 协处理器固件正常（官方 EV Board 出厂已烧录）
- 检查 SDIO 引脚是否与硬件一致
- 调试时可降低 SDIO 时钟频率（menuconfig → Hosted SDIO Configuration）

**Q: 配网成功后断电，无法再连上路由器？**

WiFi 凭据已写入 NVS，串口应出现 `restored WiFi from NVS: ssid=xxx`。若仍连不上，常见原因：

- **C6 协处理器冷启动慢**：P4 上电后需等待 C6 就绪才能连 WiFi，固件已增加重试与 2 秒启动延迟
- **连接超时回退 AP**：重试 20 次（约 40 秒）失败后会开热点 `MCP-Sensors-Setup`，**NVS 中凭据仍保留**，连热点访问 `http://192.168.4.1` 可看到已保存的 SSID
- **NVS 被擦除**：若串口出现 `NVS needs erase`，说明 Flash 分区异常，需重新配网；避免频繁 `erase-flash` 或更换分区表后不擦除旧数据

查看串口关键日志：

```
I wifi_mgr: WiFi credentials saved to NVS: ssid=xxx    # 配网保存成功
I wifi_mgr: restored WiFi from NVS: ssid=xxx           # 断电重启后读取成功
I wifi_mgr: got IP: 192.168.x.x                        # 连接成功
```

**Q: 配网后找不到设备 IP？**

查看串口日志中的 `got IP:` 行，或通过路由器管理页面查看已连接设备列表。

**Q: 网页修改后不生效？**

确认已重新烧录 SPIFFS：`idf.py build flash` 或 `idf.py spiffs-flash`。

**Q: SPIFFS 挂载失败？**

首次烧录需完整 flash（含分区表）：`idf.py erase-flash flash`。若 Flash 大于 2MB，可在 menuconfig 中调整 Flash 大小并相应修改 `partitions.csv`。

## 参考文档

- [ESP-IDF 入门指南](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html)
- [ESP-Hosted MCU 文档](https://github.com/espressif/esp-hosted-mcu)
- [ESP32-P4 Function EV Board 说明](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/esp32_p4_function_ev_board.md)
- [MCP C SDK 组件](https://components.espressif.com/components/espressif/mcp-c-sdk)
