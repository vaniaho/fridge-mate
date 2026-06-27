#include "rtc_time.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "RtcTime";

static bool s_time_synced = false;
static bool s_sntp_started = false;

static void set_beijing_timezone(void) {
    setenv("TZ", "CST-8", 1);
    tzset();
}

static void sntp_sync_notification_cb(struct timeval *tv) {
    (void)tv;
    s_time_synced = true;

    char buf[32];
    rtc_time_get_formatted(buf, sizeof(buf));
    ESP_LOGI(TAG, "NTP time synchronized: %s", buf);
}

static esp_err_t ensure_sntp_started(void) {
    if (s_sntp_started) {
        return ESP_OK;
    }

    set_beijing_timezone();

    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = sntp_sync_notification_cb;
    config.smooth_sync = true;
    config.renew_servers_after_new_IP = true;

    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP client ready. Server: pool.ntp.org");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to init SNTP: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t rtc_time_init(void) {
    ESP_LOGI(TAG, "Initializing RTC time module...");
    return rtc_time_sync_now(30000);
}

esp_err_t rtc_time_sync_now(uint32_t timeout_ms) {
    esp_err_t ret = ensure_sntp_started();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Starting NTP sync, timeout=%lu ms",
             (unsigned long)timeout_ms);
    esp_sntp_restart();

    TickType_t wait_ticks =
        timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    ret = esp_netif_sntp_sync_wait(wait_ticks);
    if (ret == ESP_OK || s_time_synced) {
        char buf[32];
        rtc_time_get_formatted(buf, sizeof(buf));
        ESP_LOGI(TAG, "Time sync successful: %s", buf);
        s_time_synced = true;
        return ESP_OK;
    }

    time_t now;
    time(&now);
    if (now > 1700000000) {
        ESP_LOGI(TAG, "System RTC already has a valid time.");
        s_time_synced = true;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "NTP sync timeout or failed: %s", esp_err_to_name(ret));
    return ret == ESP_OK ? ESP_ERR_TIMEOUT : ret;
}

bool rtc_time_is_synced(void) {
    return s_time_synced;
}

void rtc_time_force_sync(void) {
    ESP_LOGI(TAG, "Forcing NTP re-sync...");
    (void)rtc_time_sync_now(30000);
}

esp_err_t rtc_time_set_epoch(time_t epoch) {
    if (epoch < 946684800) {
        return ESP_ERR_INVALID_ARG;
    }

    set_beijing_timezone();

    struct timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    s_time_synced = true;
    char buf[32];
    rtc_time_get_formatted(buf, sizeof(buf));
    ESP_LOGI(TAG, "System time set manually: %s", buf);
    return ESP_OK;
}

esp_err_t rtc_time_set_datetime(const char *datetime) {
    if (!datetime || datetime[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int matched = sscanf(datetime, "%d-%d-%d %d:%d:%d",
                         &year, &month, &day, &hour, &minute, &second);
    if (matched < 5) {
        matched = sscanf(datetime, "%d-%d-%dT%d:%d:%d",
                         &year, &month, &day, &hour, &minute, &second);
    }
    if (matched < 5) {
        matched = sscanf(datetime, "%d-%d-%dT%d:%d",
                         &year, &month, &day, &hour, &minute);
        second = 0;
    }
    if (matched < 5 || year < 2000 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return ESP_ERR_INVALID_ARG;
    }

    set_beijing_timezone();

    struct tm timeinfo = {};
    timeinfo.tm_year = year - 1900;
    timeinfo.tm_mon = month - 1;
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = second;
    timeinfo.tm_isdst = -1;

    time_t epoch = mktime(&timeinfo);
    if (epoch < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return rtc_time_set_epoch(epoch);
}

char* rtc_time_get_formatted(char* buf, int buf_size) {
    if (!buf || buf_size <= 0) {
        return buf;
    }

    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S", &timeinfo);
    return buf;
}

char* rtc_time_get_date(char* buf, int buf_size) {
    if (!buf || buf_size <= 0) {
        return buf;
    }

    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(buf, buf_size, "%Y-%m-%d", &timeinfo);
    return buf;
}
