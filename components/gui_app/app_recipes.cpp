#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include "recipe_matcher.hpp"
#include <stdio.h>

static lv_obj_t * content_area;
// 记录当前食谱页的筛选模式，供外部刷新调用复用
static int s_current_filter = 0;

static void back_btn_event_cb(lv_event_t * e) {
    gui_app_navigate_to(GUI_APP_LAUNCHER);
}

static void nav_inventory_cb(lv_event_t * e) {
    gui_app_navigate_to(GUI_APP_INVENTORY);
}

static void card_click_cb(lv_event_t * e) {
    // Add interaction logic here if needed later
}

static void render_recipe_items(int filter_mode) {
    s_current_filter = filter_mode;
    lv_obj_clean(content_area);

    auto matches = smart_fridge::inventory::recipe_match_near(5); // Get up to 5 missing ingredients
    int count = 0;

    for (const auto& match : matches) {
        count++;

        lv_obj_t * card = lv_obj_create(content_area);
        lv_obj_set_size(card, 240, 160);
        lv_obj_add_style(card, &style_card, 0);
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, NULL);

        // Title
        lv_obj_t * title = lv_label_create(card);
        lv_label_set_text(title, match.recipe.name.c_str());
        lv_obj_add_style(title, &style_text_title, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

        // Category Tag
        lv_obj_t * cat_tag = lv_label_create(card);
        lv_label_set_text(cat_tag, match.recipe.category.c_str());
        lv_obj_set_style_bg_color(cat_tag, COLOR_PRIMARY, 0);
        lv_obj_set_style_text_color(cat_tag, lv_color_white(), 0);
        lv_obj_set_style_radius(cat_tag, 8, 0);
        lv_obj_set_style_pad_all(cat_tag, 4, 0);
        lv_obj_set_style_text_font(cat_tag, font_cn_16, 0);
        lv_obj_align(cat_tag, LV_ALIGN_TOP_RIGHT, 0, 0);

        // Match Status
        lv_obj_t * status = lv_label_create(card);
        if (match.missing_count == 0) {
            lv_label_set_text(status, "100% 匹配");
            lv_obj_set_style_text_color(status, COLOR_SUCCESS, 0);
        } else {
            lv_label_set_text_fmt(status, "缺: %d 种食材", match.missing_count);
            lv_obj_set_style_text_color(status, COLOR_WARNING, 0);
        }
        lv_obj_set_style_text_font(status, font_cn_18, 0);
        lv_obj_align(status, LV_ALIGN_LEFT_MID, 0, -10);

        // Brief
        lv_obj_t * brief = lv_label_create(card);
        lv_label_set_long_mode(brief, LV_LABEL_LONG_DOT);
        lv_label_set_text(brief, match.recipe.brief.c_str());
        lv_obj_set_size(brief, 200, 40);
        lv_obj_add_style(brief, &style_text_sub, 0);
        lv_obj_align(brief, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    if (count == 0) {
        lv_obj_t * empty_label = lv_label_create(content_area);
        lv_label_set_text(empty_label, "还没有食谱，去添加一个吧！");
        lv_obj_add_style(empty_label, &style_text_sub, 0);
        lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
    }
}

static void sidebar_btn_event_cb(lv_event_t * e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    render_recipe_items(id);
}

lv_obj_t* app_recipes_create(void) {
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_add_style(screen, &style_screen_bg, 0);

    // Sidebar
    lv_obj_t * sidebar = lv_obj_create(screen);
    lv_obj_set_size(sidebar, 200, LV_PCT(100));
    lv_obj_align(sidebar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(sidebar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(sidebar, 0, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);

    lv_obj_t * back_btn = lv_btn_create(sidebar);
    lv_obj_set_size(back_btn, LV_PCT(90), 50);
    lv_obj_align(back_btn, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xF0F0F0), 0);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "← 返回桌面");
    lv_obj_set_style_text_color(back_label, COLOR_TEXT_MAIN, 0);
    lv_obj_set_style_text_font(back_label, font_cn_18, 0);
    lv_obj_align(back_label, LV_ALIGN_CENTER, 0, 0);

    const char * menus[] = {"推荐食谱", "全部食谱"};
    for(int i=0; i<2; i++) {
        lv_obj_t * menu_btn = lv_btn_create(sidebar);
        lv_obj_set_size(menu_btn, LV_PCT(100), 50);
        lv_obj_align(menu_btn, LV_ALIGN_TOP_MID, 0, 80 + i * 60);
        lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(menu_btn, 0, 0);
        lv_obj_add_event_cb(menu_btn, sidebar_btn_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t * label = lv_label_create(menu_btn);
        lv_label_set_text(label, menus[i]);
        lv_obj_set_style_text_color(label, COLOR_TEXT_MAIN, 0);
        lv_obj_set_style_text_font(label, font_cn_18, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);
    }

    // Main Content Area
    content_area = lv_obj_create(screen);
    // Explicitly calculate height: 600 - 70 = 530
    lv_obj_set_size(content_area, 824, 530); 
    lv_obj_align(content_area, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(content_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_area, 0, 0);
    lv_obj_set_flex_flow(content_area, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(content_area, LV_DIR_VER);
    lv_obj_set_flex_align(content_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(content_area, 20, 0);

    // Bottom Action Bar
    lv_obj_t * bottom_bar = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar, 824, 70);
    lv_obj_align(bottom_bar, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_clear_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * rand_btn = lv_btn_create(bottom_bar);
    lv_obj_align(rand_btn, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_t * rand_icon = lv_label_create(rand_btn);
    lv_label_set_text(rand_icon, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_font(rand_icon, &lv_font_montserrat_18, 0);
    lv_obj_t * rand_lbl = lv_label_create(rand_btn);
    lv_label_set_text(rand_lbl, " AI推荐");
    lv_obj_set_style_text_font(rand_lbl, font_cn_18, 0);
    lv_obj_set_flex_flow(rand_btn, LV_FLEX_FLOW_ROW);

    lv_obj_t * inv_btn = lv_btn_create(bottom_bar);
    lv_obj_align(inv_btn, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_color(inv_btn, COLOR_PRIMARY, 0);
    lv_obj_add_event_cb(inv_btn, nav_inventory_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * inv_lbl = lv_label_create(inv_btn);
    lv_label_set_text(inv_lbl, "我的库存 →");
    lv_obj_set_style_text_font(inv_lbl, font_cn_18, 0);
    lv_obj_set_style_text_color(inv_lbl, lv_color_white(), 0);

    render_recipe_items(0);

    return screen;
}

void app_recipes_refresh(void) {
    // 仅当食谱应用的内容区仍然有效（即用户正在食谱页）时才真正重绘
    if (content_area && lv_obj_is_valid(content_area)) {
        render_recipe_items(s_current_filter);
    }
}
