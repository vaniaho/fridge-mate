#include "rtc_time.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>

static const char *TAG = "RtcTime";

// 首次 NTP 同步是否已完成
static bool s_time_synced = false;

// ======== SNTP 回调 ========

/**
 * @brief SNTP 时间同步完成的回调函数
 * 每次 NTP 成功获取时间后由 lwip 内部调用
 */
static void sntp_sync_notification_cb(struct timeval *tv) {
    s_time_synced = true;

    // 打印同步后的时间
    char buf[32];
    rtc_time_get_formatted(buf, sizeof(buf));
    ESP_LOGI(TAG, "NTP time synchronized: %s", buf);
}

// ======== 公共接口实现 ========

esp_err_t rtc_time_init(void) {
    ESP_LOGI(TAG, "Initializing RTC time module...");

    // 1. 设置时区为北京时间 (CST-8)
    // POSIX 时区格式：CST-8 表示 UTC+8
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to CST-8 (Beijing Time, UTC+8)");

    // 2. 配置 SNTP 客户端
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = sntp_sync_notification_cb;
    // 平滑校正模式：小偏差时使用 adjtime 渐进调整，大偏差时直接 settimeofday
    config.smooth_sync = true;
    // 同步间隔：默认 1 小时 (3600000ms)，首次同步后自动定期刷新
    config.renew_servers_after_new_IP = true;

    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SNTP: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SNTP client started. Server: pool.ntp.org");
    ESP_LOGI(TAG, "Waiting for first NTP sync (max 30s)...");

    // 3. 阻塞等待首次同步完成，最多 30 秒
    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000));
    if (ret == ESP_OK) {
        char buf[32];
        rtc_time_get_formatted(buf, sizeof(buf));
        ESP_LOGI(TAG, "Time sync successful: %s", buf);
    } else {
        ESP_LOGW(TAG, "NTP sync timeout! System time may be inaccurate.");
        ESP_LOGW(TAG, "Time will auto-sync when NTP server becomes reachable.");

        // 即使超时也不算致命错误：
        // - 后台 SNTP 任务会持续重试
        // - 如果电池座有电，ESP32-P4 内部 RTC 可能保持了上次的时间
        time_t now;
        time(&now);
        if (now > 1700000000) {
            // 时间戳大于 2023-11-14，说明 RTC 电池维持了有效时间
            ESP_LOGI(TAG, "RTC battery maintained time from previous boot.");
            s_time_synced = true;
            return ESP_OK;
        }

        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool rtc_time_is_synced(void) {
    return s_time_synced;
}

void rtc_time_force_sync(void) {
    ESP_LOGI(TAG, "Forcing NTP re-sync...");
    // esp_netif_sntp 会在下一个同步周期重新同步
    // 这里通过重新初始化触发立即同步
    esp_sntp_restart();
}

char* rtc_time_get_formatted(char* buf, int buf_size) {
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S", &timeinfo);
    return buf;
}

char* rtc_time_get_date(char* buf, int buf_size) {
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(buf, buf_size, "%Y-%m-%d", &timeinfo);
    return buf;
}
