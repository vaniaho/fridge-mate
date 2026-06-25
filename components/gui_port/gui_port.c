#include "gui_port.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"

static const char *TAG = "GUI_PORT";
static lv_disp_t *s_display = NULL;

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
    
    // This task can write NVS from settings callbacks, so its stack must stay
    // in internal SRAM while flash cache is disabled. 20 KB also leaves enough
    // room for FreeType CJK auto-hinting observed on the real board.
    cfg.lvgl_port_cfg.task_stack = 20480;
    
    // Enable software rotation to avoid hardware rotation constraints (e.g. swap_xy not supported)
    cfg.flags.sw_rotate = 1;

    // Initialize the LCD, touch, and LVGL timer task with custom config
    lv_disp_t *disp = bsp_display_start_with_config(&cfg);
    s_display = disp;
    
    // Rotate the display 180 degrees (upside down fix)
    if (disp && disp->driver) {
        disp->driver->sw_rotate = 1;
        disp->driver->drv_update_cb = NULL; // Forcefully remove hardware rotation callback
        lv_disp_set_rotation(disp, LV_DISP_ROT_180);
    }

    // Keep the panel dark until a complete, explicitly refreshed frame is in
    // the scan-out buffer. Enabling it here exposes uninitialized DSI frames
    // while the 10 MB CJK font is still being read from flash.
    bsp_display_brightness_set(0);

    ESP_LOGI(TAG, "GUI Port initialized successfully (backlight held off)");
}

esp_err_t gui_port_show_boot_screen(int brightness_percent) {
    if (!s_display) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness_percent < 5) brightness_percent = 5;
    if (brightness_percent > 100) brightness_percent = 100;

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B172A), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Smart Fridge");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t *status = lv_label_create(screen);
    lv_label_set_text(status, "Starting...");
    lv_obj_set_style_text_font(status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(status, lv_color_hex(0x9FC5E8), 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 36);

    // Force one full refresh while the backlight is still off.
    lv_obj_invalidate(screen);
    lv_refr_now(s_display);
    lvgl_port_unlock();

    // Give the DSI scan-out one frame to latch the completed buffer, then
    // fade in gently instead of exposing the panel at full brightness.
    vTaskDelay(pdMS_TO_TICKS(34));
    const int steps = 6;
    for (int step = 1; step <= steps; ++step) {
        bsp_display_brightness_set(
            (brightness_percent * step) / steps);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "Stable boot frame presented at %d%% brightness",
             brightness_percent);
    return ESP_OK;
}
