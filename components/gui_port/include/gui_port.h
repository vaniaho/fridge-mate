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
 * @brief Draw a stable built-in-font boot screen and then enable backlight.
 *
 * This must be called after SystemManager has loaded the saved brightness.
 * The screen stays visible while the large CJK fonts and desktop widgets are
 * initialized.
 */
esp_err_t gui_port_show_boot_screen(int brightness_percent);

#ifdef __cplusplus
}
#endif
