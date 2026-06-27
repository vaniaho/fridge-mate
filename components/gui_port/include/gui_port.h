#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the GUI port (Display, Touch, LVGL task)
 */
void gui_port_init(void);

/**
 * @brief Draw a stable built-in-font boot screen while keeping backlight off.
 *
 * The panel is kept dark so the long CJK font load / desktop construction
 * phase does not expose uninitialized or torn DSI frames. Call
 * gui_port_brightness_fade_in() once the launcher is fully rendered.
 */
esp_err_t gui_port_show_boot_screen(int brightness_percent);

/**
 * @brief Set display backlight to a fixed level (0-100).
 */
void gui_port_brightness_set(int brightness_percent);

/**
 * @brief Fade backlight from 0 up to the requested level.
 */
void gui_port_brightness_fade_in(int brightness_percent, int duration_ms);

#ifdef __cplusplus
}
#endif
