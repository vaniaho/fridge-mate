#include "gui_app.h"
#include "gui_styles.h"
#include "gui_icons.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include <vector>

static const char *TAG = "GuiApp";
lv_obj_t * wifi_icon_ptr = NULL;

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

static void create_splash_screen(void) {
    screen_splash = lv_obj_create(NULL);
    theme_apply_gradient_bg(screen_splash, THEME_PRIMARY, THEME_ACCENT);

    lv_obj_t *icon = lv_label_create(screen_splash);
    lv_label_set_text(icon, ICON_CAMERA);  // Placeholder fridge/camera icon
    lv_obj_set_style_text_font(icon, font_icon_36, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *title = lv_label_create(screen_splash);
    lv_label_set_text(title, "小鲜智能冰箱");
    lv_obj_set_style_text_font(title, font_cn_36, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *subtitle = lv_label_create(screen_splash);
    lv_label_set_text(subtitle, "Smart Fridge");
    lv_obj_set_style_text_font(subtitle, font_cn_18, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_white(), 0);
    lv_obj_set_style_text_opa(subtitle, LV_OPA_80, 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 45);

    lv_obj_t *bar = lv_bar_create(screen_splash);
    lv_obj_set_size(bar, 280, 8);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 95);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);
    lv_obj_set_style_radius(bar, THEME_RADIUS_PILL, 0);
    lv_bar_set_value(bar, 60, LV_ANIM_OFF);
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
            lv_label_set_text(wifi_icon_ptr, LV_SYMBOL_WIFI);
            lv_obj_set_style_text_color(wifi_icon_ptr, connected ? THEME_PRIMARY : THEME_TEXT_SUB, 0);
            lvgl_port_unlock();
        }
    }
}
