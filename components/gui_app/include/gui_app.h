#pragma once

#include <stdbool.h>
#include "gui_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

// 语音助手 UI 状态
typedef enum {
    VOICE_STATE_IDLE,
    VOICE_STATE_LISTENING,
    VOICE_STATE_THINKING,
    VOICE_STATE_SPEAKING,
} voice_assist_state_t;

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
void gui_voice_assist_set_state(voice_assist_state_t state, const char* text);
void gui_voice_assist_add_message(const char* text, bool is_user);

// -------- Status --------
void gui_app_set_wifi_status(bool connected);

// -------- Per-app refresh hooks --------
// 重新渲染当前库存页内容（按最近一次的筛选模式），仅在库存页可见时生效
void app_inventory_refresh(void);
// 重新渲染当前食谱页内容（按最近一次的筛选模式），仅在食谱页可见时生效
void app_recipes_refresh(void);

// -------- Launcher (桌面) 实时数据刷新 --------
// 更新桌面天气卡片（temp 摄氏度，city/text 可为 NULL 表示未知/离线）
void gui_launcher_update_weather(float temp, const char* city, const char* text);
// 更新桌面留言板（notes_vec_ptr 指向 std::vector<smart_fridge::dashboard::Note>*，NULL 表示清空）
void gui_launcher_update_notes(const void* notes_vec_ptr);
// 从 dashboard 缓存同步天气 + 留言到桌面（供 gui_bridge_refresh_dashboard 调用）
void gui_launcher_refresh_dashboard(void);
// 标记某条留言为"新"（高亮显示），在 gui_launcher_update_notes 时生效
void gui_launcher_mark_note_new(long long timestamp);

#ifdef __cplusplus
}
#endif
