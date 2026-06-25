#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 定义应用页面 ID
typedef enum {
    GUI_APP_LAUNCHER,
    GUI_APP_INVENTORY,
    GUI_APP_RECIPES,
    GUI_APP_SETTINGS,
    GUI_APP_VOICE_ASSIST,
    GUI_APP_SHOPPING
} gui_app_id_t;

/**
 * @brief Initialize the GUI bridge layer
 */
void gui_bridge_init(void);

// ======== Screen Control ========

/** @brief Turn on the display / Wake up */
void gui_bridge_wake(void);

/** @brief Turn off the display / Enter standby */
void gui_bridge_show_standby(void);

// ======== Navigation ========

/** @brief Navigate to a specific application */
void gui_bridge_navigate_to(gui_app_id_t app_id);

/** @brief Go back to previous screen */
void gui_bridge_go_back(void);

// ======== Modals & Overlays ========

/** @brief Request GUI to refresh the ingredient list view */
void gui_bridge_refresh_inventory(void);

/** @brief Request GUI to refresh the dashboard (weather + notes board) on the launcher */
void gui_bridge_refresh_dashboard(void);

/** @brief Show a notification popup on the screen */
void gui_bridge_show_notification(const char* title, const char* message);

/** @brief Show/hide the "listening" indicator during voice input */
void gui_bridge_show_listening_indicator(bool is_listening);

/** @brief Display TTS text on screen while being spoken */
void gui_bridge_show_tts_text(const char* text);

/** @brief Show Camera Preview modal */
void gui_bridge_show_camera_preview(void);

/** @brief Show Full-screen Voice Assistant */
void gui_bridge_show_voice_assist(void);
void gui_bridge_voice_set_state(int state, const char* text);
void gui_bridge_voice_add_message(const char* text, bool is_user);

#ifdef __cplusplus
}
#endif
