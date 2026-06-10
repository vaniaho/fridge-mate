# 开发者 C 开发方案

根据《功能设计》与《开发方案》文档，开发者 C 在团队中扮演**系统集成者**和**业务逻辑核心**的角色。负责云端大模型中枢、本地逻辑与系统总线、环境监测及数据持久化。

## 1. 核心模块与职责划分 (C 的工作区)

### 1.1 系统总线与调度 (`main/`)

- **`system_events.h`**: 定义全局事件流，如 `EVT_WAKE_WORD_DETECTED`, `EVT_VISION_INFER_DONE`, `EVT_VOICE_CMD_RCVD`, `EVT_LLM_RESPONSE_READY` 等。
- **`task_manager.cpp`**: 创建 FreeRTOS 任务（队列容量 20，栈 8192），集成 `gui_bridge` 桥接调用，将 A（视觉）和 B（语音）的数据分发给 C 的业务逻辑。

### 1.2 系统级公共服务 (`components/system_services/`) ★ 新增

- **`wifi_manager.c`**: WiFi 连接管理（ESP-Hosted via C6 协处理器），从 `ai_agent` 中迁出。
- **`sd_storage.c`**: SD 卡 SPI 模式驱动 + FATFS 挂载，从 `inventory_system` 中迁出。
- **`credentials_manager.c`**: 基于 NVS 的凭证管理器，支持运行时修改（为 GUI 设置页面预留）。
- **`Kconfig`**: 在 `idf.py menuconfig` 中提供默认 WiFi 和 LLM API 配置。

### 1.3 GUI 事件桥接层 (`components/gui_bridge/`) ★ 新增

- **`gui_bridge.c`**: 系统事件与 GUI 框架的抽象桥接层。
- 当前为 Stub 实现（仅日志输出），待 GUI 框架（LVGL 或 ESP-Brookesia）确定后接入。
- 接口：屏幕亮灭、库存刷新、通知弹窗、语音监听指示、TTS 文本显示、设置页面导航。

### 1.4 存储与本地数据库 (`components/inventory_system/`)

- **`inventory_manager.cpp`**:
  - 封装食材 CRUD 接口（当前使用 JSON 存储，后续可迁移至 SQLite）。
  - **核心算法**：保质期倒计时（配合 RTC）、消耗速率计算、本地食谱匹配算法。
  - SD 卡初始化已迁移至 `system_services`，通过 `#include "sd_storage.h"` 引用。

### 1.5 云端大模型中枢 (`components/ai_agent/`)

- **`llm_client.cpp`**: 基于 `esp_http_client` 封装，支持 HTTPS。
  - API URL 和 Key 通过 `credentials_manager` 获取（NVS 存储，支持运行时修改）。
  - 不再硬编码任何敏感信息。
- **`prompt_builder.cpp`**:
  - 构建 System Prompt，模型名称通过 `credentials_get_llm_model()` 动态获取。
  - 使用 cJSON 解析大模型返回的结构化指令。

### 1.6 环境与低功耗 (`components/env_sensors/`)

- **`rtc_time.c`**: 结合 SNTP 服务，在连网时同步网络时间，并写入外置 RTC 芯片。
- **`power_manage.c`**: 配置 PIR 传感器的 GPIO 中断。处理 5 分钟超时逻辑，进入 Light Sleep 模式。

### 1.7 Web 控制面板 (`components/web_panel/` - 可选)

- 基于 `esp_http_server` 提供 RESTful API，供手机端查看和管理。

***

## 2. 接口设计与协作契约

### 2.1 全局事件定义 (`system_events.h`)

```cpp
typedef enum {
    EVT_SYS_INIT_DONE,
    EVT_WAKE_WORD_DETECTED, // 语音唤醒 (B -> C)
    EVT_VISION_INFER_DONE,  // 图像识别完成 (A -> C)
    EVT_VOICE_CMD_RCVD,     // 语音指令收到 (B -> C)
    EVT_LLM_RESPONSE_READY, // LLM 处理完成 (C -> B, C -> A)
    EVT_INVENTORY_UPDATED,  // 食材库存已更新 (C -> A)
    EVT_PIR_TRIGGERED,      // 人体靠近 (C -> A 亮屏)
    EVT_GOTO_SLEEP          // 进入休眠 (C -> 全局)
} sys_event_type_t;
```

### 2.2 凭证管理接口 (`credentials_manager.h`) ★ 新增

```cpp
// 初始化（读取 NVS，回退到 Kconfig 默认值）
esp_err_t credentials_init(void);

// WiFi 凭证
const char* credentials_get_wifi_ssid(void);
const char* credentials_get_wifi_pass(void);
esp_err_t credentials_set_wifi(const char* ssid, const char* pass);

// LLM API 凭证
const char* credentials_get_llm_api_url(void);
const char* credentials_get_llm_api_key(void);
const char* credentials_get_llm_model(void);
esp_err_t credentials_set_llm_api(const char* url, const char* key);
esp_err_t credentials_set_llm_model(const char* model);
```

### 2.3 核心数据结构 (`inventory.hpp`)

```cpp
struct IngredientItem {
    int id;
    std::string name;
    std::string category;
    int quantity;
    time_t entry_time;
    int expire_days;
    time_t expire_time;
};
```

***

## 3. 分阶段实施路径 (开发方案)

### Phase 1: 基础设施与全局调度 ✅ 已完成

- **成果**：
  1. `main/system_events.h` 全局事件定义
  2. `main/task_manager.cpp` 事件总线（队列 20，栈 8192，集成 gui\_bridge）
  3. `system_services/sd_storage.c` SD 卡 SPI 挂载
  4. `system_services/wifi_manager.c` ESP-Hosted WiFi 连接
  5. `system_services/credentials_manager.c` NVS 凭证管理

### Phase 2: 本地业务与存储引擎 (部分完成)

- **已完成**：
  - 食材 CRUD API（基于 JSON + 内存缓存）
  - 临期检测功能（阈值可配置）
  - SD 卡回退机制（纯内存模式）
- **待完成**：
  - RTC 时钟 & NTP 对时
  - 食材历史记录与消耗速率计算
  - 本地食谱匹配算法

### Phase 3: 云端大模型打通 ✅ 已完成

- **成果**：
  1. `llm_client.cpp` 大模型 REST API 接入（支持豆包、Moonshot 等）
  2. `prompt_builder.cpp` System Prompt 构建 + JSON 解析
  3. API 凭证通过 `credentials_manager` 安全管理，不再硬编码

### Phase 3.5: GUI 集成 ★ 新增阶段

- **目标**：将 GUI 框架集成到主工程，让业务逻辑有前端展示载体。
- **任务**：
  1. 确定 GUI 框架（LVGL 或 ESP-Brookesia）
  2. 将 `esp_brookesia_phone/` 参考代码集成到主工程
  3. 实现 `gui_bridge.c` 中的真实 GUI 操作
  4. 首次三方联调（A+B+C）

### Phase 4: 低功耗与环境控制

- **目标**：系统功耗优化，提升体验。
- **任务**：
  1. 编写 `power_manage.c`，配置 PIR 传感器引脚为中断模式。
  2. 实现定时器机制，超过 5 分钟无交互/无人靠近，发送 `EVT_GOTO_SLEEP`。
  3. 配置 ESP32-P4 的 Light Sleep 唤醒源。

### Phase 5: Web 面板与联调优化

- **目标**：收尾与可视化管理。
- **任务**：
  1. 初始化 `esp_http_server`。
  2. 暴露 `GET /inventory` 和 `POST /update` 接口。
  3. 整机联调，处理边界情况（如弱网下的 LLM 超时回退机制）。

## Verification Plan

### 单元验证 (C 的本地测试)

- **存储验证**：断电重启后，SD 卡中的食材数据和 RTC 时间无丢失。
- **凭证验证**：通过 `credentials_set_*` 修改凭证后重启，验证 NVS 持久化。
- **云端验证**：使用 Mock 数据触发 LLM 接口，验证 JSON 解析是否准确。
- **通信验证**：向 System Queue 发送模拟事件，验证 gui\_bridge 响应。

### 集成验证 (团队联调)

- 触发 PIR -> gui\_bridge\_screen\_on() -> 屏幕亮起。
- 语音唤醒 -> 语音输入 "存入两个苹果" -> B 发送给 C -> C 传给云端 -> 更新库存 -> C 返回播报文本 -> B 播报。

