# 开发者 A 开发指南 — GUI 界面 + 视觉模块

> **文档版本**：v1.0 · 2026-06-18
> **项目**：智能冰箱 · 小鲜 · Smart Fridge（ESP32-P4 平台）
> **适用对象**：开发者 A（负责 GUI 界面实现 + 图像识别模块）

---

## 1. 你的职责范围

| 组件 | 路径 | 状态 | 说明 |
|------|------|------|------|
| **GUI 界面层** | `components/gui_app/` | ✅ 框架已搭建，需按 UI 设计方案完善 | Launcher、库存、食谱、设置页等 |
| **图像识别模块** | `components/vision_hal/` | ❌ 尚未创建 | 摄像头驱动 + 本地图像分类模型 |

### C 已经帮你搭好的基础：

| 模块 | 说明 | 状态 |
|------|------|------|
| `gui_bridge/` | 所有跨线程 GUI 调用的安全桥接层（`lv_async_call`） | ✅ 已实现 |
| `gui_port/` | LCD 驱动 + Touch 驱动 + LVGL 任务创建（栈 32KB） | ✅ 已实现 |
| `inventory_system/` | 食材 CRUD + 食谱匹配 + 历史记录 + 品类表（mutex 保护） | ✅ 已实现 |
| `system_manager/` | WiFi 扫描连接 + 亮度 + 音量 + 系统信息 | ✅ 已实现 |
| `system_events.h` | 全局事件类型 + payload 结构体 | ✅ 已定义 |
| 中文字体 | NotoSansSC-Regular.ttf → FreeType 动态加载到 PSRAM | ✅ 已集成 |
| `web_panel/` | 手机 Web 控制面板 + WebSocket 实时推送 | ✅ 已实现 |
| `ai_agent/` | 云端 LLM 调用 + Prompt 动态注入库存上下文 | ✅ 已实现 |

### 模块间协作关系

```
后台事件/HTTP 请求                你的代码
     │                            │
     ▼                            ▼
gui_bridge_xxx()     ─────>   gui_app_xxx()
  (C/B 调用)        lv_async_call    (LVGL 线程)
                                │
                                ▼
                     inventory_system / system_manager
                          (后端数据 API)
```

---

## 2. 硬件资源速览

| 资源 | 规格 |
|------|------|
| 处理器 | RISC-V 双核 HP@360MHz + LP@40MHz |
| 内存 | 32MB PSRAM + 32MB Flash + 768KB L2MEM |
| 屏幕 | **7 英寸 1024×600 MIPI-DSI + 触摸** |
| 摄像头 | MIPI-CSI 2-lane（支持 RPi Camera） |
| SD 卡 | SDIO 3.0 |
| 音频 | ES8311 + ES7210 + 双麦 + 扬声器 |

---

## 3. `gui_app` 组件现状

### 3.1 已有文件

```
components/gui_app/
├── include/
│   ├── gui_app.h           # 主入口 + 所有 gui_app_* 函数声明
│   └── gui_styles.h        # 颜色常量 + 字体变量 + 全局样式
├── gui_app.cpp             # ✅ 屏幕管理器 + FreeType 字体加载 + Splash
├── gui_launcher.cpp        # ✅ 桌面启动器 (左右分栏)
├── gui_standby.cpp         # ✅ 待机/锁屏界面
├── gui_overlays.cpp        # ✅ 通知弹窗 + 语音指示器 + TTS 文本条
├── app_inventory.cpp       # ✅ 库存管理页 (侧边栏 + 食材卡片)
├── app_recipes.cpp         # ✅ 智能食谱页 (侧边栏 + 食谱卡片)
├── app_settings.cpp        # ✅ 系统设置页 (WiFi/显示/声音/设备信息)
└── CMakeLists.txt
```

### 3.2 `gui_app.h` 接口清单

以下是 `gui_bridge.c` 会调用到的所有函数：

```c
// -------- 已实现 --------
void gui_app_init(void);                    // ✅ Splash → Launcher 流程
void gui_app_navigate_to(gui_app_id_t id);  // ✅ 导航栈 + 屏幕切换动画
void gui_app_go_back(void);                 // ✅ 出栈 + 右滑动画
void gui_app_wake_from_standby(void);       // ✅ 待机 → Launcher
void gui_app_show_standby(void);            // ✅ 进入待机
void gui_app_refresh_inventory(void);       // ✅ 刷新库存+食谱页
void gui_app_show_notification(...);        // ✅ 通知弹窗
void gui_app_show_listening_indicator(...); // ✅ 语音指示器
void gui_app_show_tts_text(...);            // ✅ TTS 文本显示
void gui_app_set_wifi_status(bool);         // ✅ WiFi 图标颜色

// -------- 需要你完善 --------
void gui_app_show_camera_preview(void);     // ⬜ 拍照预览页面
void gui_app_show_voice_assist(void);       // ⬜ 全屏语音助手对话页

// -------- 各应用页面刷新钩子 --------
void app_inventory_refresh(void);           // ✅ 刷新库存列表
void app_recipes_refresh(void);             // ✅ 刷新食谱列表
```

### 3.3 调用链路

```
后台事件 / HTTP 请求
    │
    ▼
gui_bridge_xxx()          ← C/B 的代码调用这些函数
    │ strdup + malloc
    ▼
lv_async_call(callback)   ← 自动切换到 LVGL 线程
    │
    ▼
gui_app_xxx()             ← 你的代码在这里执行 LVGL 控件操作
```

> **线程安全规则**：永远不要从 `gui_app` 以外的地方直接操作 LVGL 控件。所有外部触发的 GUI 更新必须通过 `gui_bridge` 的 `lv_async_call` 路径。你在 `gui_app` 内部写的代码天然运行在 LVGL 线程中，可以直接操作控件。

### 3.4 应用 ID 枚举

```c
typedef enum {
    GUI_APP_LAUNCHER,      // 桌面
    GUI_APP_INVENTORY,     // 库存管理
    GUI_APP_RECIPES,       // 智能食谱
    GUI_APP_SETTINGS,      // 系统设置
    GUI_APP_VOICE_ASSIST,  // 语音助手
    GUI_APP_SHOPPING       // 购物清单 (预留)
} gui_app_id_t;
```

---

## 4. 后端 API 速查手册

以下是你在 GUI 页面中需要调用的数据接口。**所有接口都是线程安全的**（已加 mutex），可以在 LVGL 线程中直接调用。

### 4.1 食材库存 (`#include "inventory.hpp"`)

```cpp
using namespace smart_fridge::inventory;

// 获取所有食材（用于库存列表渲染）
std::vector<IngredientItem> items = get_all_ingredients();

// IngredientItem 结构体字段
struct IngredientItem {
    int id;                // 唯一 ID
    std::string name;      // "苹果"
    std::string category;  // "水果"
    int quantity;          // 数量
    time_t entry_time;     // 最近存入时间
    int expire_days;       // 保质期天数
    time_t expire_time;    // 预计过期时间
};

// 存入食材（拍照/语音识别后调用）
bool ok = add_ingredient("苹果", "水果", 3, 7);

// 取出食材（卡片上的 [-] 按钮）
bool ok = remove_ingredient("苹果", 1);

// 检查临期食材（"临期预警"视图用）
auto expiring = check_expiring_ingredients(3);  // 3天内过期的

// SD 卡状态（设备信息页用）
bool sd_ok = is_sd_card_available();
```

### 4.2 食谱匹配 (`#include "recipe_matcher.hpp"`)

```cpp
using namespace smart_fridge::inventory;

// 食材齐全的食谱
auto full_match = recipe_match_available();

// 差 2 样以内的食谱（食谱页默认视图）
auto near_match = recipe_match_near(2);

// RecipeMatch 结构体
struct RecipeMatch {
    Recipe recipe;                          // 食谱详情
    float coverage;                         // 0.0~1.0 覆盖率
    std::vector<std::string> missing_items; // 缺失食材名称
    int missing_count;                      // 缺失数量
};

// Recipe 结构体
struct Recipe {
    std::string name;      // "西红柿炒蛋"
    std::string category;  // "家常菜"
    std::vector<RecipeIngredient> ingredients;
    std::string brief;     // 简要做法
};

// 获取全部食谱 / 新增 / 删除
auto all = recipe_get_all();
recipe_add(new_recipe);
recipe_remove("西红柿炒蛋");
```

### 4.3 历史记录 (`#include "inventory_history.hpp"`)

```cpp
using namespace smart_fridge::inventory;

// 最近 30 天记录（存取记录视图用）
auto records = history_get_recent(30);

// 某种食材的记录（食材详情页用）
auto apple_records = history_get_by_item("苹果", 30);

// 消耗速率（食材详情页展示）
ConsumptionRate rate = compute_consumption_rate("苹果", 7);
// rate.daily_rate = 0.3, rate.total_consumed = 2, rate.window_days = 7
```

### 4.4 品类查询 (`#include "category_table.h"`)

```c
const char* cat = category_lookup("西红柿");  // 返回 "蔬菜"
int days = default_expire_days("水果");       // 返回 7
int days = expire_days_for_item("牛奶");      // 返回 14 (一步到位)
```

**11 个分类**：水果、蔬菜、肉禽、海鲜、蛋奶、豆制品、主食、调味品、饮品、零食、冻品、其他

### 4.5 系统管理 (`#include "system_manager.hpp"`)

设置页面需要调用的 API：

```cpp
using namespace smart_fridge::system;

// WiFi
SystemManager::scan_wifi_async();                        // 触发异步扫描
auto networks = SystemManager::get_scanned_networks();   // 获取扫描结果
SystemManager::connect_wifi("MyWiFi", "password123");    // 连接
SystemManager::disconnect_wifi();                        // 断开
WifiStatus status = SystemManager::get_wifi_status();    // DISCONNECTED/CONNECTING/CONNECTED
std::string ip = SystemManager::get_wifi_ip();           // "192.168.1.100"

// 显示
SystemManager::set_brightness(80);     // 亮度 0~100
int b = SystemManager::get_brightness();

// 声音
SystemManager::set_volume(70);         // 音量 0~100
int v = SystemManager::get_volume();

// 系统信息
std::string ver = SystemManager::get_firmware_version();
uint32_t free_heap = SystemManager::get_free_heap();
uint32_t total_heap = SystemManager::get_total_heap();
```

---

## 5. 事件系统

### 5.1 你需要投递的事件

图像识别完成后，投递事件通知 C 的事件总线：

```cpp
#include "system_events.h"

vision_infer_payload_t* p = (vision_infer_payload_t*)malloc(sizeof(vision_infer_payload_t));
p->top1_id = 15;         // 识别出的最高置信度类别 ID
p->confidence = 0.92f;   // 置信度
send_system_event(EVT_VISION_INFER_DONE, p, sizeof(vision_infer_payload_t));
// payload 由事件总线处理后自动 free，不要手动释放
```

### 5.2 系统自动为你派发的事件

以下事件由 C 的 `task_manager` 接收后，通过 `gui_bridge` → `lv_async_call` 自动调用你的 `gui_app_*` 函数，**你不需要手动监听**：

| 事件 | 触发方 | 自动调用的 gui_app 函数 |
|------|--------|------------------------|
| `EVT_PIR_TRIGGERED` | 红外传感器 | `gui_app_wake_from_standby()` ✅ |
| `EVT_INVENTORY_UPDATED` | add/remove 操作 | `app_inventory_refresh()` + `app_recipes_refresh()` ✅ |
| `EVT_LLM_RESPONSE_READY` | LLM 返回 | `gui_app_show_tts_text()` / `gui_app_show_notification()` ✅ |
| `EVT_WAKE_WORD_DETECTED` | B 的语音模块 | `gui_app_show_listening_indicator(true)` ✅ |
| `EVT_GOTO_SLEEP` | 5分钟无活动 | `gui_app_show_standby()` ✅ |

---

## 6. `vision_hal` 组件（需要新建）

### 6.1 目录结构建议

```
components/vision_hal/
├── include/
│   └── vision_hal.h       # 公共接口
├── vision_hal.cpp          # 摄像头初始化 + 推断入口
├── model/                  # 本地分类模型文件
│   └── food_classifier.tflite
└── CMakeLists.txt
```

### 6.2 需要实现的接口

```c
// vision_hal.h
esp_err_t vision_hal_init(void);               // 初始化摄像头 + 加载模型
esp_err_t vision_hal_capture_and_infer(void);   // 拍照 + 本地分类推断

typedef struct {
    int class_id;        // 类别 ID
    char class_name[64]; // 食材名称
    float confidence;    // 置信度 0~1.0
} vision_result_t;

esp_err_t vision_hal_get_result(vision_result_t* result);

esp_err_t vision_hal_start_preview(void);  // 实时预览 (给拍照页用)
esp_err_t vision_hal_stop_preview(void);
```

### 6.3 参考代码

`esp_brookesia_phone/components/apps/camera/` 目录下有完整的摄像头 pipeline 参考：
- `app_camera_pipeline.cpp` — CSI 摄像头初始化 + 帧获取
- `app_video.cpp` — 视频流处理
- `Camera.cpp` — 相机应用完整实现

---

## 7. GUI 开发规范

### 7.1 颜色常量 (`gui_styles.h`)

| 常量 | 色值 | 用途 |
|------|------|------|
| `COLOR_BG` | `#F4F6F8` | 页面背景 |
| `COLOR_CARD` | `#FFFFFF` | 卡片背景 |
| `COLOR_PRIMARY` | `#4A90E2` | 蓝色主色调、交互高亮 |
| `COLOR_TEXT_MAIN` | `#333333` | 主文字 |
| `COLOR_TEXT_SUB` | `#888888` | 辅助文字 |
| `COLOR_SUCCESS` | `#2ECC71` | 绿色（食材齐全/成功） |
| `COLOR_WARNING` | `#F1C40F` | 黄色（临期/警告） |
| `COLOR_DANGER` | `#E74C3C` | 红色（过期/错误） |
| `COLOR_DIVIDER` | `#E0E0E0` | 分割线 |
| `COLOR_TAG_BG` | `#EBF2FC` | 分类标签背景 |

### 7.2 字体变量

| 变量 | 字号 | 用途 |
|------|------|------|
| `font_cn_16` | 16px | 辅助文字、标签 |
| `font_cn_18` | 18px | 正文、卡片内容、按钮 |
| `font_cn_24` | 24px | 页面标题、卡片标题 |
| `font_cn_36` | 36px | 大时钟、关键数值 |
| `font_icon_24` | 24px | LVGL 内置符号图标 |

### 7.3 页面创建模式

每个应用页面遵循统一的 `create` + `refresh` 模式：

```cpp
// 创建新屏幕（由 gui_app_navigate_to 调用）
lv_obj_t* app_xxx_create(void) {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_add_style(screen, &style_screen_bg, 0);
    // ... 创建控件 ...
    return screen;
}

// 刷新内容（由 gui_app_refresh_inventory 调用）
void app_xxx_refresh(void) {
    if (!content_area || !lv_obj_is_valid(content_area)) return;
    // ... 重新渲染 ...
}
```

### 7.4 屏幕生命周期

- **Launcher 和 Standby**：常驻内存，不销毁
- **应用页面**（库存/食谱/设置等）：切出时 `auto_del=true` 自动销毁
- 确保你创建的控件挂在 screen 下，而非全局变量

### 7.5 如何添加新的应用页面

1. 创建 `app_xxx.cpp`，实现 `lv_obj_t* app_xxx_create(void)` 函数
2. 在 `gui_app.h` 中声明该函数
3. 在 `gui_app.cpp` 的 `gui_app_navigate_to()` switch 中添加 case
4. 如需新 ID，在 `gui_bridge.h` 的 `gui_app_id_t` 枚举中添加
5. 在 `CMakeLists.txt` 的 SRCS 中添加新文件

---

## 8. 需要你完善的事项（按优先级）

### P0 — 影响基础功能

- [ ] **拍照录入页面** (`gui_app_show_camera_preview`)
  - 调用 `vision_hal_start_preview()` 显示实时画面
  - 用户点击拍照 → `vision_hal_capture_and_infer()`
  - 显示识别结果弹窗（名称 + 置信度 + 分类 + 保质期）
  - 用户确认 → `add_ingredient()` 存入
  - 参考 **UI 设计方案 §2.6.1**

- [ ] **`vision_hal` 摄像头模块**（全新组件）
  - 初始化 MIPI-CSI 摄像头
  - 集成本地食材分类模型（置信度 ≥60% 用 Top-3，<60% 提示语音修正）
  - 推断完成后投递 `EVT_VISION_INFER_DONE`
  - 参考 `esp_brookesia_phone/components/apps/camera/`

### P1 — 完善用户体验

- [ ] **全屏语音助手页面** (`gui_app_show_voice_assist`)
  - 中央区域：序列帧波形动画（用 `lv_anim_img`，不要用 Lottie）
  - 底部区域：对话历史气泡
  - 参考 **UI 设计方案 §2.6.3**

- [ ] **Launcher 天气组件**（需新增天气 API 后端，可与 C 协商）
- [ ] **Launcher 留言板组件**（需新增留言存储后端，可与 C 协商）

### P2 — 优化细节

- [ ] 库存页：食材详情页消耗速率图表
- [ ] 食谱页：云端推荐加载动画
- [ ] 设置页：WiFi 扫描列表实时刷新
- [ ] 待机页：临期食材数量实时更新

---

## 9. 初始化时序

`main.cpp` 中的 GUI 初始化顺序：

```
Phase 5: GUI 初始化
  ① gui_port_init()          // LCD + Touch + LVGL 任务
  ② SystemManager::init()    // WiFi/亮度/音量管理器
  ③ lvgl_port_lock(0)        // 获取 LVGL 锁
  ④ gui_app_init()           // ★ 你的入口！Splash → Launcher
  ⑤ lvgl_port_unlock()       // 释放锁
  ⑥ gui_bridge_init()        // 桥接层就绪
  ⑦ gui_app_set_wifi_status() // 初始 WiFi 状态
```

`gui_app_init()` 在 `lvgl_port_lock` 保护下调用，内部可以安全操作 LVGL。此后外部更新 GUI 均通过 `gui_bridge` 的 `lv_async_call`。

---

## 10. 常见问题

**Q: 如何安全地调用后端数据接口？**
直接调用。后端的 `inventory_cache`、`history_cache`、`recipe_cache` 均已加 mutex 保护。

**Q: 操作食材后如何刷新 GUI？**
不需要手动刷新。`add_ingredient` / `remove_ingredient` 内部投递 `EVT_INVENTORY_UPDATED`，事件总线自动调用 `gui_bridge_refresh_inventory()` → `app_inventory_refresh()`。

**Q: 字体显示为方块？**
检查：① `/internal/NotoSansSC-Regular.ttf` 是否存在 ② `menuconfig` 中 `LV_USE_FREETYPE` 是否启用 ③ PSRAM 是否有足够空间（约 3~8MB）。

**Q: Splash 屏幕显示多久？**
约 1 秒（`lv_scr_load_anim` 的 1000ms delay），然后渐变到 Launcher。

---

## 11. 联调接口约定

### 与 C（系统集成者）

| 方向 | 接口 | 说明 |
|------|------|------|
| A → C | `send_system_event(EVT_VISION_INFER_DONE, ...)` | 图像识别完成 |
| C → A | `gui_bridge_refresh_inventory()` | 库存变化自动刷新 |
| C → A | `gui_bridge_show_notification(title, msg)` | 临期提醒等通知 |
| C → A | `gui_bridge_wake()` / `show_standby()` | 屏幕唤醒/休眠 |
| A ↔ C | `inventory_system` 所有 API | 直接调用读写数据 |
| A ↔ C | `SystemManager` 所有 API | 设置页联动 |

### 与 B（语音模块）

| 方向 | 接口 | 说明 |
|------|------|------|
| B → A | `gui_bridge_show_listening_indicator(bool)` | 语音监听指示 |
| B → A | `gui_bridge_show_tts_text(text)` | TTS 播报文本 |

你和 B 之间**不需要直接通信**，所有交互通过 C 的事件总线 + gui_bridge 中转。

---

## 12. 参考文档

| 文档 | 路径 | 用途 |
|------|------|------|
| **UI 设计方案** | `doc/UI 设计方案.md` | **最重要** — 每个页面的详细布局和交互规格 |
| 功能设计 | `doc/功能设计.md` | 完整功能需求 |
| 工程结构 | `doc/工程结构.md` | 项目目录说明 |
| 开发板硬件 | `doc/ESP32-P4-WIFI6-Touch-LCD-7B 开发板.md` | 硬件引脚和规格 |
