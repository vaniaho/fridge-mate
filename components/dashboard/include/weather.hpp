#pragma once

#include <time.h>
#include <string>

namespace smart_fridge {
namespace dashboard {

/**
 * @brief 天气信息缓存
 */
struct WeatherInfo {
    float temp;             // 温度（摄氏度）
    std::string text;       // 天气描述（如 "晴"）
    std::string city;       // 城市名
    time_t updated;         // 上次成功刷新时间戳，0 表示从未刷新
    bool valid;             // 是否有有效数据
};

/**
 * @brief 初始化天气模块（从 NVS 读取配置，不主动拉取）
 *        拉取由 weather_refresh() 或 dashboard 定时任务触发。
 */
void weather_init();

/**
 * @brief 获取天气缓存（线程安全的拷贝）
 */
WeatherInfo weather_get();

/**
 * @brief 调用用户配置的天气 API 拉取并更新缓存。
 *        内部从 credentials_manager 读取 URL/Key/Location 与字段路径。
 *        会阻塞直到 HTTP 请求完成（通常 1~5 秒），应在后台任务中调用。
 * @return true 拉取成功并更新了缓存
 */
bool weather_refresh();

} // namespace dashboard
} // namespace smart_fridge
