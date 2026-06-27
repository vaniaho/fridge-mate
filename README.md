# 小鲜智能冰箱 (Smart Fridge)

基于 **ESP32-P4 + ESP32-C6 (ESP-Hosted)** 的智能冰箱助手，搭载 7 英寸 MIPI-DSI 触摸屏，支持食材管理、语音/拍照录入、云端大模型推荐、Web 控制面板等功能。

## 硬件平台

- **开发板**：ESP32-P4-WIFI6-Touch-LCD-7B
- **主控**：RISC-V HP 双核 @360MHz + LP @40MHz
- **无线**：ESP32-C6 协处理器通过 SDIO 提供 Wi-Fi 6 / Bluetooth 5
- **屏幕**：7 英寸 1024×600 MIPI-DSI 触摸屏
- **摄像头**：MIPI-CSI 2-lane（支持 RPi Camera）
- **音频**：ES8311 + ES7210 + 双麦克风 + 扬声器
- **存储**：32MB PSRAM + 32MB Nor Flash + Micro SD 卡槽

## 已完成功能

| 模块 | 状态 | 说明 |
|------|------|------|
| 系统事件总线 | ✅ | FreeRTOS 队列，支持语音/图像/LLM/库存/PIR/休眠事件 |
| WiFi / SD 卡 / NVS 凭证 | ✅ | ESP-Hosted 联网，SD 卡 FATFS，NVS 存储 WiFi/LLM/天气凭证 |
| GUI 框架 | ✅ | LVGL + 自定义 Launcher；库存、食谱、设置、待机页面可用 |
| 食材数据库 | ✅ | 批次/FIFO 管理、临期检测、历史记录、消耗速率、本地食谱匹配 |
| 云端大模型 | ✅ | 异步 worker 调用 LLM，Prompt 注入库存/消耗/食谱上下文 |
| Web 控制面板 | ✅ | HTTP/HTTPS + WebSocket，支持库存/食谱/留言板/天气/设置/语音配置管理 |
| 桌面看板 | ✅ | 天气卡片 + 留言板，30 分钟定时刷新 |
| RTC / NTP | ✅ | 北京时间对时，RTC 离线保持 |
| 音频云端链路 | ⚠️ | ES7210/ES8311、ASR、TTS、端到端实时语音、浏览器 PCM 已接入；本地唤醒词待实现 |

## 待接入/待完善

| 模块 | 状态 | 说明 |
|------|------|------|
| 视觉模块 (`vision_hal`) | ❌ | MIPI-CSI 摄像头 + 本地图像分类模型 |
| 本地唤醒词检测 | ❌ | `audio_hal` 采集链路已就绪，唤醒词检测任务仍是占位消费者 |
| 低功耗管理 | ❌ | PIR 红外唤醒、自动息屏/Light Sleep，仅事件入口预留 |
| 拍照预览页 | ⚠️ | GUI 占位，待接入 `vision_hal` |
| 运行时音量控制 | ⚠️ | 启动时会应用音量到 `audio_hal`，设置页滑块尚未实时调用硬件音量接口 |
| 设置页日期时间 Tab | ✅ | GUI 与 Web 设置页均支持时间状态查看、手动设置和立即 NTP 同步 |
| Web 面板认证 | ⚠️ | 当前局域网内可直接访问和修改凭证，实机部署前需补本地认证 |

## 快速开始

### 1. 环境准备

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/)（建议 v5.x 或更新）
- Python 3、CMake、Ninja
- 一张 Micro SD 卡（FAT32），用于持久化库存、食谱、留言板数据

### 2. 配置 WiFi 与 LLM 凭证

```bash
idf.py menuconfig
# 进入 Smart Fridge Configuration -> WiFi Settings / LLM API Settings
```

> 也可在开发板启动后，通过触摸屏设置页或 Web 面板配置。

### 3. 编译与烧录

```bash
idf.py build
idf.py -p COMx flash monitor
```

> 请将 `COMx` 替换为实际的串口号。首次烧录时建议同时烧录 `partitions.csv` 对应的分区表。

### 4. 访问 Web 面板

开发板联网后，在手机/电脑浏览器中访问设备 IP（串口日志会输出），即可管理库存、查看食谱、留言板等。

## 项目结构

```text
smart_fridge/
├── main/                    # app_main、系统事件总线、任务调度
├── components/
│   ├── system_services/     # WiFi、SD 卡、NVS 凭证
│   ├── system_manager/      # 运行时 WiFi/亮度/音量/设备信息
│   ├── gui_port/            # LCD/Touch/LVGL 任务底座
│   ├── gui_bridge/          # 系统事件 → LVGL 桥接层
│   ├── gui_app/             # Launcher、库存、食谱、设置等 UI 页面
│   ├── inventory_system/    # 食材 CRUD、历史、食谱匹配
│   ├── ai_agent/            # 云端 LLM 客户端 + Prompt 构建
│   ├── audio_hal/           # ES7210/ES8311、ASR/TTS/实时语音
│   ├── web_panel/           # HTTP Server + WebSocket + REST API
│   ├── dashboard/           # 天气 + 留言板
│   └── env_sensors/         # RTC/NTP（低功耗待实现）
├── web_frontend/            # 手机 Web 面板前端源码
├── internal_fs_data/        # 打包进 Flash 的资源（中文字体等）
└── doc/                     # 设计文档
```

## 注意事项

- **不支持 5G WiFi**：ESP32-C6 仅支持 2.4GHz 频段。
- **SD 卡**：部分数据持久化依赖 SD 卡；SD 卡不可用时自动降级为纯内存模式，重启后数据丢失。
- **大模型 API**：需要配置有效的 API URL、Key 和模型名称；默认端点为豆包/火山方舟，可替换为任意 OpenAI 兼容 API。
- **语音功能**：ASR/TTS/端到端实时语音需要配置云端凭证，并依赖 WiFi 与有效系统时间；本地唤醒词尚未接入。
- **拍照功能**：需等待 `vision_hal` 摄像头与本地模型组件接入后才能使用。
- **当前进展**：详见 [doc/项目进展与问题.md](doc/项目进展与问题.md)。

## 团队分工

- **A**：GUI 维护 + 图像识别模块 (`components/gui_app/`、`components/vision_hal/`)
- **B**：语音音频模块 (`components/audio_hal/`，重点补本地唤醒词与实机调优)
- **C**：系统总线、本地逻辑、云端大模型、Web 面板、环境监测 (`main/`、`components/system_*`、`inventory_system`、`ai_agent`、`web_panel`、`dashboard`、`env_sensors`)

## 许可证

本项目为团队内部开发，具体开源协议待定。
