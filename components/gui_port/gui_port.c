#include "gui_port.h"
#include "esp_log.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"

static const char *TAG = "GUI_PORT";

void gui_port_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL and Display via BSP...");
    
    // We must manually configure the display to override the LVGL task stack size.
    // The default 7KB stack is too small for FreeType's af_autofitter_load_glyph when rendering new Chinese text.
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }
    };
    
    // Increase LVGL task stack size for FreeType dynamic rendering
    cfg.lvgl_port_cfg.task_stack = 32768;
    
    // Enable software rotation to avoid hardware rotation constraints (e.g. swap_xy not supported)
    cfg.flags.sw_rotate = 1;

    // Initialize the LCD, touch, and LVGL timer task with custom config
    lv_disp_t *disp = bsp_display_start_with_config(&cfg);
    
    // Rotate the display 180 degrees (upside down fix)
    if (disp && disp->driver) {
        disp->driver->sw_rotate = 1;
        disp->driver->drv_update_cb = NULL; // Forcefully remove hardware rotation callback
        lv_disp_set_rotation(disp, LV_DISP_ROT_180);
    }
    
    // Turn on backlight
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "GUI Port initialized successfully");
}
