#pragma once

#include <stdbool.h>
#include "gui_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

void gui_app_init(void);

// -------- Screen Manager --------
void gui_app_wake_from_standby(void);
void gui_app_show_standby(void);
void gui_app_navigate_to(gui_app_id_t app_id);
void gui_app_go_back(void);

// -------- Modals & Overlays --------
void gui_app_refresh_inventory(void);
void gui_app_show_notification(const char* title, const char* message);
void gui_app_show_listening_indicator(bool is_listening);
void gui_app_show_tts_text(const char* text);
void gui_app_show_camera_preview(void);
void gui_app_show_voice_assist(void);

// -------- Status --------
void gui_app_set_wifi_status(bool connected);

// -------- Per-app refresh hooks --------
// 重新渲染当前库存页内容（按最近一次的筛选模式），仅在库存页可见时生效
void app_inventory_refresh(void);
// 重新渲染当前食谱页内容（按最近一次的筛选模式），仅在食谱页可见时生效
void app_recipes_refresh(void);

#ifdef __cplusplus
}
#endif
