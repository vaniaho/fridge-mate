#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Dashboard 后台定时任务（每 30 分钟刷新一次天气）
 *        拉取成功后调用已注册的 on_refresh 回调（通常由 main 设置为
 *        gui_bridge_refresh_dashboard，避免组件间循环依赖）。
 *        应在 WiFi 连接成功后调用。
 */
void dashboard_start_task(void);

/**
 * @brief 注册天气刷新成功后的回调（用于通知 GUI 刷新桌面）
 *        回调在 dashboard 任务上下文中执行。传 NULL 取消注册。
 */
void dashboard_set_refresh_callback(void (*cb)(void));

#ifdef __cplusplus
}
#endif
