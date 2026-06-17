#include "gui_app.h"
#include "gui_styles.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include <vector>

static const char *TAG = "GuiApp";
lv_obj_t * wifi_icon_ptr = NULL;

// -------- Global Fonts & Styles --------
const lv_font_t * font_cn_16 = &lv_font_montserrat_14; // Fallback
const lv_font_t * font_cn_18 = &lv_font_montserrat_14; // Fallback
const lv_font_t * font_cn_24 = &lv_font_montserrat_14; // Fallback
const lv_font_t * font_cn_36 = &lv_font_montserrat_14; // Fallback
const lv_font_t * font_icon_24 = &lv_font_montserrat_14; // Fallback

lv_style_t style_screen_bg;
lv_style_t style_card;
lv_style_t style_text_main;
lv_style_t style_text_sub;
lv_style_t style_text_title;

// -------- Screen Pointers --------
static lv_obj_t * screen_splash = NULL;
static lv_obj_t * screen_launcher = NULL;
static lv_obj_t * screen_standby = NULL;

// Active screen
static lv_obj_t * current_screen = NULL;

// Navigation Stack
static std::vector<gui_app_id_t> nav_stack;

// -------- External App Init --------
extern void gui_launcher_init(lv_obj_t* parent);
extern void gui_standby_init(lv_obj_t* parent);
extern void gui_overlays_init(void);

extern lv_obj_t* app_inventory_create(void);
extern lv_obj_t* app_recipes_create(void);
extern lv_obj_t* app_settings_create(void);

#if LV_USE_FREETYPE
#include "extra/libs/freetype/lv_freetype.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#endif

void gui_styles_init(void) {
#if LV_USE_FREETYPE
    ESP_LOGI(TAG, "Initializing FreeType fonts...");
    if (lv_freetype_init(64, 1, 0)) {
        lv_ft_info_t info;
        memset(&info, 0, sizeof(info));
        
        // Cache the TTF file into PSRAM to dramatically speed up FreeType and prevent watchdog timeouts
        FILE* f = fopen("/internal/NotoSansSC-Regular.ttf", "rb");
        void* font_mem = NULL;
        size_t font_size = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            font_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            font_mem = heap_caps_malloc(font_size, MALLOC_CAP_SPIRAM);
            if (font_mem) {
                ESP_LOGI(TAG, "Loading TTF into PSRAM (%u bytes)...", font_size);
                fread(font_mem, 1, font_size, f);
                info.mem = font_mem;
                info.mem_size = font_size;
                info.name = "/internal/NotoSansSC-Regular.ttf"; // Must not be NULL, used as cache key by LVGL
            } else {
                ESP_LOGE(TAG, "Failed to allocate PSRAM for font caching!");
                info.name = "/internal/NotoSansSC-Regular.ttf";
            }
            fclose(f);
        } else {
            ESP_LOGE(TAG, "Failed to open TTF file!");
            info.name = "/internal/NotoSansSC-Regular.ttf";
        }

        info.style = FT_FONT_STYLE_NORMAL;

        info.weight = 16;
        if (lv_ft_font_init(&info)) font_cn_16 = info.font;

        info.weight = 18;
        if (lv_ft_font_init(&info)) font_cn_18 = info.font;

        info.weight = 24;
        if (lv_ft_font_init(&info)) font_cn_24 = info.font;

        info.weight = 36;
        if (lv_ft_font_init(&info)) font_cn_36 = info.font;
        
        info.weight = 24;
        if (lv_ft_font_init(&info)) font_icon_24 = info.font;
        ESP_LOGI(TAG, "FreeType fonts loaded successfully!");
    } else {
        ESP_LOGE(TAG, "Failed to initialize FreeType!");
    }
#else
#warning "LV_USE_FREETYPE is not enabled! Chinese characters will display as squares. Please enable it in menuconfig and provide the TTF font on the SD card."
    ESP_LOGW(TAG, "FreeType not enabled. Using fallback fonts.");
#endif

    // 1. Init global styles
    // 1. Init global styles
    lv_style_init(&style_screen_bg);
    lv_style_set_bg_color(&style_screen_bg, COLOR_BG);
    lv_style_set_text_font(&style_screen_bg, font_cn_18);

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, COLOR_CARD);
    lv_style_set_radius(&style_card, 16);
    // Simple shadow
    lv_style_set_shadow_width(&style_card, 12);
    lv_style_set_shadow_ofs_y(&style_card, 4);
    lv_style_set_shadow_opa(&style_card, LV_OPA_10);
    lv_style_set_border_width(&style_card, 0);

    lv_style_init(&style_text_main);
    lv_style_set_text_color(&style_text_main, COLOR_TEXT_MAIN);
    lv_style_set_text_font(&style_text_main, font_cn_18);

    lv_style_init(&style_text_sub);
    lv_style_set_text_color(&style_text_sub, COLOR_TEXT_SUB);
    lv_style_set_text_font(&style_text_sub, font_cn_16);

    lv_style_init(&style_text_title);
    lv_style_set_text_color(&style_text_title, COLOR_TEXT_MAIN);
    lv_style_set_text_font(&style_text_title, font_cn_24);
}

static void create_splash_screen(void) {
    screen_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_splash, lv_color_hex(0xE3F2FD), 0);

    lv_obj_t * title = lv_label_create(screen_splash);
    lv_label_set_text(title, "Smart Fridge");
    lv_obj_add_style(title, &style_text_title, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t * loading = lv_label_create(screen_splash);
    lv_label_set_text(loading, "Loading...");
    lv_obj_add_style(loading, &style_text_sub, 0);
    lv_obj_align(loading, LV_ALIGN_CENTER, 0, 20);
}

void gui_app_init(void) {
    ESP_LOGI(TAG, "GUI App Initializing...");
    gui_styles_init();

    // 1. Create and show Splash
    create_splash_screen();
    lv_scr_load(screen_splash);
    current_screen = screen_splash;

    // 2. Initialize Overlays
    gui_overlays_init();

    // 3. Initialize Launcher (kept in memory)
    screen_launcher = lv_obj_create(NULL);
    lv_obj_add_style(screen_launcher, &style_screen_bg, 0);
    gui_launcher_init(screen_launcher);

    // 4. Initialize Standby
    screen_standby = lv_obj_create(NULL);
    gui_standby_init(screen_standby);

    // Switch to Launcher
    lv_scr_load_anim(screen_launcher, LV_SCR_LOAD_ANIM_FADE_ON, 500, 1000, true);
    current_screen = screen_launcher;
}

void gui_app_navigate_to(gui_app_id_t app_id) {
    lv_obj_t * new_screen = NULL;

    switch (app_id) {
        case GUI_APP_LAUNCHER:
            new_screen = screen_launcher;
            break;
        case GUI_APP_INVENTORY:
            new_screen = app_inventory_create();
            break;
        case GUI_APP_RECIPES:
            new_screen = app_recipes_create();
            break;
        case GUI_APP_SETTINGS:
            new_screen = app_settings_create();
            break;
        case GUI_APP_VOICE_ASSIST:
            // Delegate to overlays/modals instead of screen navigation
            gui_app_show_voice_assist();
            return;
        case GUI_APP_SHOPPING:
            // Placeholder for Shopping app
            return;
        default:
            return;
    }

    if (new_screen) {
        bool auto_del = (current_screen != screen_launcher && current_screen != screen_standby);
        lv_scr_load_anim(new_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, auto_del);
        current_screen = new_screen;
        
        if (app_id != GUI_APP_LAUNCHER) {
            nav_stack.push_back(app_id);
        } else {
            nav_stack.clear();
        }
    }
}

void gui_app_go_back(void) {
    if (!nav_stack.empty()) {
        nav_stack.pop_back();
    }
    
    if (nav_stack.empty()) {
        lv_scr_load_anim(screen_launcher, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
        current_screen = screen_launcher;
    } else {
        gui_app_id_t prev_app = nav_stack.back();
        nav_stack.pop_back(); // Pop because navigate_to will push it again
        gui_app_navigate_to(prev_app);
    }
}

void gui_app_wake_from_standby(void) {
    if (current_screen == screen_standby) {
        lv_scr_load_anim(screen_launcher, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
        current_screen = screen_launcher;
    }
}

void gui_app_show_standby(void) {
    if (current_screen != screen_standby) {
        lv_scr_load_anim(screen_standby, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
        current_screen = screen_standby;
    }
}

// 刷新当前可见的业务页内容。
// inventory 与 recipes 两个应用页各自内部会判断 content_area 是否仍然有效，
// 因此只有用户当前停留在的页面会真正重绘，其它页面的 content_area 已随屏幕销毁而失效。
void gui_app_refresh_inventory(void) {
    app_inventory_refresh();
    app_recipes_refresh();
}
void gui_app_set_wifi_status(bool connected) {
    if (wifi_icon_ptr) {
        if (lvgl_port_lock(0)) {
            if (connected) {
                lv_obj_set_style_text_color(wifi_icon_ptr, COLOR_PRIMARY, 0);
            } else {
                lv_obj_set_style_text_color(wifi_icon_ptr, COLOR_TEXT_SUB, 0);
            }
            lvgl_port_unlock();
        }
    }
}
