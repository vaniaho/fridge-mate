#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Web 面板服务 (HTTP + WebSocket)
 * @return esp_err_t 
 */
esp_err_t web_panel_start(void);

/**
 * @brief 通过 WebSocket 广播消息给所有连接的前端客户端
 * @param msg 消息内容，如 "update"
 */
void web_panel_broadcast_ws(const char* msg);

#ifdef __cplusplus
}
#endif
