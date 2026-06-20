#include "dashboard.hpp"
#include "weather.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtc_time.h"

static const char *TAG = "Dashboard";

// 天气刷新间隔（秒）。30 分钟。
static constexpr int WEATHER_REFRESH_INTERVAL_S = 30 * 60;

// 刷新成功后的回调（由 main 注册，通常指向 gui_bridge_refresh_dashboard）
static void (*s_refresh_cb)(void) = NULL;

void dashboard_set_refresh_callback(void (*cb)(void)) {
    s_refresh_cb = cb;
}

/**
 * @brief Dashboard 后台任务：周期性刷新天气缓存并通知 GUI 刷新。
 *        首次启动延迟 30 秒（等待网络稳定），之后每 30 分钟一次。
 */
static void dashboard_task(void *pvParameters) {
    // 首次延迟，等待 WiFi 完全连接 + NTP 同步
    vTaskDelay(pdMS_TO_TICKS(30000));

    while (1) {
        // 仅在时间已同步（间接表示网络可用）时拉取天气
        if (rtc_time_is_synced()) {
            bool ok = smart_fridge::dashboard::weather_refresh();
            if (ok && s_refresh_cb) {
                // 通知 GUI 刷新桌面天气卡片与留言板
                s_refresh_cb();
            }
        } else {
            ESP_LOGD(TAG, "Time not synced yet, skipping weather refresh");
        }

        vTaskDelay(pdMS_TO_TICKS(WEATHER_REFRESH_INTERVAL_S * 1000));
    }
}

extern "C" void dashboard_start_task(void) {
    xTaskCreate(dashboard_task, "dashboard", 8192, NULL, 3, NULL);
    ESP_LOGI(TAG, "Dashboard task started (weather refresh every %d min)",
             WEATHER_REFRESH_INTERVAL_S / 60);
}
