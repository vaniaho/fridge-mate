#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "inventory.hpp"
#include "recipe_matcher.hpp"
#include "inventory_history.hpp"
#include "ai_agent.hpp"
#include "credentials_manager.h"
#include "wifi_manager.h"
#include "web_panel.h"
#include "rtc_time.h"
#include "system_events.h"
#include "gui_bridge.h"
#include "system_manager.hpp"
#include "gui_port.h"
#include "gui_app.h"
#include "audio_api.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "vfs_fat_internal.h"
#include "dashboard.hpp"
#include "notes_board.hpp"
#include "weather.hpp"

// 声明 task_manager 暴露出来的初始化函数
extern "C" void task_manager_init(void);

static const char *TAG = "SmartFridge";

// 系统状态标志
static bool s_network_ok = false;
static bool s_db_ok = false;

// ============================================================
// 周期性临期检查任务
// 每隔一段时间扫描库存中的临期/过期食材，
// 通过日志告警、GUI 通知和 WebSocket 推送提醒用户
// ============================================================
static void expiry_check_task(void *pvParameters) {
    const int CHECK_INTERVAL_S = 3600;  // 每小时检查一次
    const int WARNING_DAYS = 3;         // 3 天内过期视为临期

    // 初始延迟 60 秒，等待系统完全就绪（NTP 同步、SD 卡加载等）
    vTaskDelay(pdMS_TO_TICKS(60000));

    while (1) {
        if (!rtc_time_is_synced()) {
            ESP_LOGW(TAG, "[ExpiryCheck] 时间未同步，跳过本轮检查");
            vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_S * 1000));
            continue;
        }

        auto expiring = smart_fridge::inventory::check_expiring_ingredients(WARNING_DAYS);
        if (!expiring.empty()) {
            ESP_LOGW(TAG, "========== 临期预警: %d 项食材需要关注 ==========",
                     (int)expiring.size());

            time_t now;
            time(&now);

            for (const auto &item : expiring) {
                for (const auto &batch : item.batches) {
                    double remaining = difftime(batch.expire_time, now) / (24.0 * 3600.0);
                    if (remaining < 0) {
                        ESP_LOGE(TAG, "  [已过期] %s (批次 %d) — 过期 %.0f 天",
                                 item.name.c_str(), batch.batch_id, -remaining);
                    } else {
                        ESP_LOGW(TAG, "  [即将过期] %s (批次 %d) — 剩余 %.1f 天",
                                 item.name.c_str(), batch.batch_id, remaining);
                    }
                }
            }

            // 通知 GUI（当前为 Stub，接入 LVGL 后生效）
            gui_bridge_show_notification("临期提醒",
                "有食材即将过期，请尽快处理！");

            // 通知 Web 面板实时刷新
            web_panel_broadcast_ws("update");
        } else {
            ESP_LOGI(TAG, "[ExpiryCheck] 所有食材状态良好，无临期预警");
        }

        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_S * 1000));
    }
}

// ============================================================
// 打印系统状态摘要
// ============================================================
static void print_system_status(void) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "============ 系统状态摘要 ============");

    // 时间
    char time_buf[32];
    if (rtc_time_is_synced()) {
        ESP_LOGI(TAG, "  系统时间 : %s (已同步)",
                 rtc_time_get_formatted(time_buf, sizeof(time_buf)));
    } else {
        ESP_LOGW(TAG, "  系统时间 : 未同步 (保质期计算可能不准确)");
    }

    // 库存概况
    auto items = smart_fridge::inventory::get_all_ingredients();
    ESP_LOGI(TAG, "  库存食材 : %d 种", (int)items.size());
    for (const auto &item : items) {
        time_t now;
        time(&now);
        double remaining = 9999.0;
        // 取最早过期的批次计算剩余天数（batches 按存入时间排序，但最早存入 ≠ 最早过期）
        for (const auto& batch : item.batches) {
            double r = difftime(batch.expire_time, now) / (24.0 * 3600.0);
            if (r < remaining) remaining = r;
        }
        const char *status = (item.batches.empty()) ? "正常" : (remaining < 0) ? "已过期" :
                             (remaining <= 3) ? "即将过期" : "正常";
        ESP_LOGI(TAG, "    - %s: %d 个 (%s, 分类: %s, 最早剩余 %.0f 天)",
                 item.name.c_str(), item.total_quantity, status,
                 item.category.c_str(), remaining);
    }

    // 临期食材
    auto expiring = smart_fridge::inventory::check_expiring_ingredients(3);
    if (!expiring.empty()) {
        ESP_LOGW(TAG, "  临期预警 : %d 项需要关注", (int)expiring.size());
    }

    // 可做菜品
    auto recipes = smart_fridge::inventory::recipe_match_near(2);
    if (!recipes.empty()) {
        ESP_LOGI(TAG, "  可做菜品 : %d 个 (差2样以内)", (int)recipes.size());
        int count = 0;
        for (const auto &m : recipes) {
            if (count >= 5) break;
            if (m.missing_count == 0) {
                ESP_LOGI(TAG, "    ★ %s (食材齐全)", m.recipe.name.c_str());
            } else {
                ESP_LOGI(TAG, "    ☆ %s (差 %d 样)", m.recipe.name.c_str(), m.missing_count);
            }
            count++;
        }
    }

    // 网络 & LLM
    if (s_network_ok) {
        ESP_LOGI(TAG, "  WiFi     : %s (已连接)", credentials_get_wifi_ssid());
        ESP_LOGI(TAG, "  LLM 模型 : %s", credentials_get_llm_model());
        ESP_LOGI(TAG, "  Web 管理 : http://<device-ip>:80/");
        ESP_LOGI(TAG, "  Web 语音 : https://<device-ip>:443/");
    } else {
        ESP_LOGW(TAG, "  网络状态 : 离线模式");
    }

    // 内存
    ESP_LOGI(TAG, "  可用内存 : %lu KB", (unsigned long)(esp_get_free_heap_size() / 1024));
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "");
}

// ============================================================
// 主入口
// ============================================================
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "   智能冰箱 · 小鲜 · Smart Fridge    ");
    ESP_LOGI(TAG, "   System Starting...                 ");
    ESP_LOGI(TAG, "======================================");

    // --------------------------------------------------------
    // Phase 0: NVS 初始化 & 凭证加载
    // --------------------------------------------------------
    ESP_LOGI(TAG, "[Phase 0] 初始化 NVS 和凭证管理器...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区损坏，正在擦除重建...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(credentials_init());
    ESP_LOGI(TAG, "  凭证已加载 — WiFi: [%s], LLM: [%s]",
             credentials_get_wifi_ssid(), credentials_get_llm_model());

    // --------------------------------------------------------
    // Phase 1: 系统事件总线
    // --------------------------------------------------------
    ESP_LOGI(TAG, "[Phase 1] 启动系统事件总线...");
    task_manager_init();
    vTaskDelay(pdMS_TO_TICKS(100)); // 等待总线任务启动就绪

    // --------------------------------------------------------
    // Phase 2: 初始化内部存储与本地食材数据库
    //   包含内部 Flash FATFS 挂载（用于字体资源）、SD 卡驱动、
    //   库存数据、历史记录、食谱匹配。
    //   SD 卡失败时自动降级为纯内存模式
    // --------------------------------------------------------
    ESP_LOGI(TAG, "[Phase 2] 挂载内部资源分区 (storage) ...");
    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 10;
    mount_config.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;
    wl_handle_t s_wl_handle;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/internal", "storage", &mount_config, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  内部资源分区挂载失败 (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "  内部资源分区挂载成功 (/internal)");
    }

    ESP_LOGI(TAG, "[Phase 2] 初始化食材数据库...");
    s_db_ok = smart_fridge::inventory::init_database();
    if (s_db_ok) {
        auto items = smart_fridge::inventory::get_all_ingredients();
        ESP_LOGI(TAG, "  食材数据库就绪 — 当前库存 %d 种", (int)items.size());
    } else {
        ESP_LOGE(TAG, "  食材数据库初始化失败！系统将以降级模式运行");
    }

    // 留言板初始化（不依赖网络，SD 卡可用时持久化）
    ESP_LOGI(TAG, "[Phase 2] 初始化留言板...");
    smart_fridge::dashboard::notes_init();

    // 天气模块初始化（仅读取 NVS 配置，不主动拉取；拉取在 dashboard 任务中触发）
    smart_fridge::dashboard::weather_init();

    // --------------------------------------------------------
    // Phase 3: 网络连接 & 时间同步
    //   WiFi 未配置或连接失败时进入离线模式
    //   离线模式下 AI 助手和 Web 面板不可用，但本地功能正常
    // --------------------------------------------------------
    const char *wifi_ssid = credentials_get_wifi_ssid();
    const char *wifi_pass = credentials_get_wifi_pass();

    ESP_LOGI(TAG, "[Phase 3] 初始化 WiFi 驱动...");
    s_network_ok = wifi_init_sta(wifi_ssid, wifi_pass);

    if (strlen(wifi_ssid) == 0) {
        ESP_LOGW(TAG, "  WiFi 凭证未配置，保持离线模式，可进入设置界面进行扫描配置");
    } else if (s_network_ok) {
        ESP_LOGI(TAG, "  WiFi 已连接");

        // NTP 时间同步（保质期倒计时依赖准确时间）
        ESP_LOGI(TAG, "  正在同步 NTP 时间...");
        if (rtc_time_init() == ESP_OK) {
            char buf[32];
            ESP_LOGI(TAG, "  时间已同步: %s", rtc_time_get_formatted(buf, sizeof(buf)));
        } else {
            ESP_LOGW(TAG, "  NTP 同步超时，将在后台持续重试");
        }
    } else {
        ESP_LOGE(TAG, "  WiFi 连接失败，进入离线模式");
    }

    // --------------------------------------------------------
    // Phase 4: 启动 Web 控制面板
    //   仅在网络可用时启动
    // --------------------------------------------------------
    if (s_network_ok) {
        ESP_LOGI(TAG, "[Phase 4] 启动 Web 控制面板...");
        if (web_panel_start() == ESP_OK) {
            ESP_LOGI(TAG, "  Web 面板已启动 — 管理: http://<device-ip>/");
            ESP_LOGI(TAG, "  浏览器语音 — https://<device-ip>/（首次需接受设备证书）");
        } else {
            ESP_LOGE(TAG, "  Web 面板启动失败");
        }
    } else {
        ESP_LOGW(TAG, "[Phase 4] 网络不可用，Web 面板已跳过");
    }

    // --------------------------------------------------------
    // Phase 5: 初始化 GUI
    // --------------------------------------------------------
    ESP_LOGI(TAG, "[Phase 5] 初始化 GUI (LVGL & Display)...");
    // Init GUI Port (Display + Touch + LVGL Task)
    gui_port_init();

    // Initialize SystemManager (NVS, Wi-Fi, Display, Settings)
    // Must be called after gui_port_init so that BSP display is ready
    smart_fridge::system::SystemManager::init();

    // Present a complete built-in-font frame before the expensive CJK font
    // cache and desktop construction begin. This prevents blue/uninitialized
    // DSI frames from becoming visible during startup.
    if (gui_port_show_boot_screen(
            smart_fridge::system::SystemManager::get_brightness()) !=
        ESP_OK) {
        ESP_LOGW(TAG, "  启动画面显示失败，将继续初始化 GUI");
    }

    lvgl_port_lock(0);
    gui_app_init();
    lvgl_port_unlock();
    gui_bridge_init();

    // 注册 dashboard 刷新回调：天气刷新成功后通知 GUI 更新桌面天气卡片
    dashboard_set_refresh_callback(gui_bridge_refresh_dashboard);

    // 更新 GUI 中的 WiFi 图标状态
    gui_app_set_wifi_status(s_network_ok);

    // 首次刷新桌面留言板（天气缓存尚未拉取，会显示占位 "--"）
    gui_bridge_refresh_dashboard();

    // --------------------------------------------------------
    // Phase 5.5: 初始化音频 HAL 并启动唤醒词监听
    // --------------------------------------------------------
    ESP_LOGI(TAG, "[Phase 5.5] 初始化音频 HAL...");
    esp_err_t audio_err = audio_hal_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "  音频 HAL 初始化失败: %s", esp_err_to_name(audio_err));
    } else {
        audio_hal_set_output_volume(
            smart_fridge::system::SystemManager::get_volume());
    }
    audio_hal_set_event_callback([](audio_hal_event_t evt, const char* data) {
        voice_session_report_audio_event((int)evt, data);
        switch (evt) {
            case AUDIO_EVT_WAKE_WORD:
                ESP_LOGI(TAG, "[Audio] Wake word");
                break;
            case AUDIO_EVT_LISTENING_START:
                ESP_LOGI(TAG, "[Audio] Listening start");
                break;
            case AUDIO_EVT_LISTENING_STOP:
                ESP_LOGI(TAG, "[Audio] Listening stop");
                break;
            case AUDIO_EVT_ASR_PARTIAL:
                ESP_LOGI(TAG, "[Audio] ASR partial: %s", data ? data : "");
                break;
            case AUDIO_EVT_ASR_RESULT:
                ESP_LOGI(TAG, "[Audio] ASR: %s", data ? data : "");
                break;
            case AUDIO_EVT_ASR_ERROR:
                ESP_LOGE(TAG, "[Audio] ASR error: %s", data ? data : "");
                break;
            case AUDIO_EVT_TTS_START:
                ESP_LOGI(TAG, "[Audio] TTS start");
                break;
            case AUDIO_EVT_TTS_DONE:
                ESP_LOGI(TAG, "[Audio] TTS done");
                break;
            case AUDIO_EVT_TTS_INTERRUPTED:
                ESP_LOGI(TAG, "[Audio] TTS interrupted");
                break;
            case AUDIO_EVT_TTS_ERROR:
                ESP_LOGE(TAG, "[Audio] TTS error: %s", data ? data : "");
                break;
            case AUDIO_EVT_REALTIME_TEXT:
                ESP_LOGI(TAG, "[Audio] Realtime text: %s",
                         data ? data : "");
                break;
            case AUDIO_EVT_REALTIME_TURN_DONE:
                ESP_LOGI(TAG, "[Audio] Realtime turn done");
                break;
            default:
                break;
        }
    });
    audio_hal_set_pcm_output_callback(
        [](const uint8_t* pcm, size_t length, int sample_rate) {
            if (sample_rate == 16000) {
                web_panel_broadcast_ws_binary(pcm, length);
            }
        });
    if (audio_err == ESP_OK) {
        audio_hal_start_wake_word();
        ESP_LOGI(TAG, "  音频采集/播放已就绪");
    }

    // --------------------------------------------------------
    // Phase 6: 启动后台周期任务
    // --------------------------------------------------------
    ESP_LOGI(TAG, "[Phase 6] 启动后台任务...");
    xTaskCreate(expiry_check_task, "expiry_chk", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "  临期检查任务已启动 (每小时检查一次)");

    // 启动 Dashboard 后台任务（每 30 分钟刷新天气，仅联网时生效）
    dashboard_start_task();
    ESP_LOGI(TAG, "  Dashboard 任务已启动 (每 30 分钟刷新天气)");

    // --------------------------------------------------------
    // 初始化完成：打印系统状态摘要
    // --------------------------------------------------------
    print_system_status();

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "   系统就绪，等待用户交互...           ");
    ESP_LOGI(TAG, "   - 语音: 说 \"小鲜小鲜\" 唤醒 (待B接入)");
    ESP_LOGI(TAG, "   - 触屏: 触摸屏操作 (待A接入)       ");
    ESP_LOGI(TAG, "   - Web:  手机浏览器访问控制面板      ");
    ESP_LOGI(TAG, "======================================");

    // 主任务保持存活，FreeRTOS 任务负责实际工作
    // event bus / web panel / expiry checker 均在独立任务中运行
    // All long-running work is owned by dedicated FreeRTOS tasks. Returning
    // lets ESP-IDF delete the main task and release its initialization stack.
}
