#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the GUI bridge layer
 * Should be called after the GUI framework is initialized.
 * Currently a stub - will connect to LVGL/ESP-Brookesia once decided.
 */
void gui_bridge_init(void);

// ======== Screen Control ========

/** @brief Turn on the display (e.g., after PIR detection) */
void gui_bridge_screen_on(void);

/** @brief Turn off the display (e.g., entering sleep mode) */
void gui_bridge_screen_off(void);

// ======== Inventory Display ========

/** @brief Request GUI to refresh the ingredient list view */
void gui_bridge_refresh_inventory(void);

// ======== Status & Notifications ========

/** @brief Show a notification popup on the screen */
void gui_bridge_show_notification(const char* title, const char* message);

/** @brief Show/hide the "listening" indicator during voice input */
void gui_bridge_show_listening_indicator(bool is_listening);

/** @brief Display TTS text on screen while being spoken */
void gui_bridge_show_tts_text(const char* text);

// ======== Navigation ========

/** @brief Navigate to the settings page */
void gui_bridge_open_settings(void);

#ifdef __cplusplus
}
#endif
