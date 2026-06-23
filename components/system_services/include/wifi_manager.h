#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connect to WiFi AP (blocking)
 * Uses ESP-Hosted via ESP32-C6 co-processor
 * @param ssid WiFi SSID
 * @param pass WiFi password
 * @return true 成功连接 AP 并获取 IP；false 未配置 SSID 或连接失败
 */
bool wifi_init_sta(const char* ssid, const char* pass);
void wifi_manager_disable_retry(void);

/**
 * @brief 注销 boot 阶段注册的事件 handler
 * 当 SystemManager 接管 WiFi 状态机后调用，避免重复处理事件。
 */
void wifi_manager_unregister_handlers(void);

#ifdef __cplusplus
}
#endif
