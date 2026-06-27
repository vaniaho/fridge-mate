#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 RTC 时间模块
 *
 * 1. 启动 SNTP 客户端，连接 NTP 服务器同步时间
 * 2. 设置系统时区为 CST-8 (北京时间)
 * 3. 阻塞等待首次时间同步完成（最多 30 秒）
 *
 * @note 必须在 WiFi 连接成功之后调用
 * @return ESP_OK 同步成功, ESP_ERR_TIMEOUT 超时未同步
 */
esp_err_t rtc_time_init(void);

/**
 * @brief Trigger an immediate SNTP sync and wait for the result.
 *
 * This is safe to call multiple times after Wi-Fi becomes available.
 */
esp_err_t rtc_time_sync_now(uint32_t timeout_ms);

/**
 * @brief 获取当前是否已完成至少一次 NTP 时间同步
 * @return true 已同步, false 尚未同步（系统时间可能不准确）
 */
bool rtc_time_is_synced(void);

/**
 * @brief 手动触发一次 NTP 重新同步
 * 可在检测到时间偏差过大时调用
 */
void rtc_time_force_sync(void);

/**
 * @brief Set system time from a local epoch timestamp.
 */
esp_err_t rtc_time_set_epoch(time_t epoch);

/**
 * @brief Set system time from "YYYY-MM-DD HH:MM:SS" or datetime-local text.
 */
esp_err_t rtc_time_set_datetime(const char *datetime);

/**
 * @brief 获取格式化的当前时间字符串
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 格式化后的字符串指针 (即 buf)，格式为 "2026-06-12 15:30:45"
 */
char* rtc_time_get_formatted(char* buf, int buf_size);

/**
 * @brief 获取格式化的日期字符串
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 格式化后的字符串指针，格式为 "2026-06-12"
 */
char* rtc_time_get_date(char* buf, int buf_size);

#ifdef __cplusplus
}
#endif
