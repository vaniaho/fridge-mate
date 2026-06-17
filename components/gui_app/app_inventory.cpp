#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include "inventory.hpp"
#include <time.h>
#include <stdio.h>

static lv_obj_t * content_area;
// 记录当前库存页的筛选模式，供外部刷新调用复用
static int s_current_filter = 0;

static void back_btn_event_cb(lv_event_t * e) {
    gui_app_navigate_to(GUI_APP_LAUNCHER);
}

static void nav_recipes_cb(lv_event_t * e) {
    gui_app_navigate_to(GUI_APP_RECIPES);
}

static void card_click_cb(lv_event_t * e) {
    // Add interaction logic here if needed later
}

static void render_inventory_items(int filter_mode) {
    s_current_filter = filter_mode;
    // Clear old items
    lv_obj_clean(content_area);

    auto items = smart_fridge::inventory::get_all_ingredients();
    time_t now;
    time(&now);

    int count = 0;

    for (const auto& item : items) {
        // 取最早过期的批次来计算剩余天数（batches 按存入时间排序，但最早存入 ≠ 最早过期）
        double remaining = 9999.0;
        for (const auto& batch : item.batches) {
            double r = difftime(batch.expire_time, now) / (24.0 * 3600.0);
            if (r < remaining) remaining = r;
        }

        if (filter_mode == 1 && remaining > 3.0) continue; // Expiring soon
        if (filter_mode == 2 && item.category != "水果") continue; // Fruits
        if (filter_mode == 3 && item.category != "蔬菜") continue; // Veggies
        if (filter_mode == 4 && item.category == "水果") continue; // Other
        if (filter_mode == 4 && item.category == "蔬菜") continue; // Other

        count++;

        lv_obj_t * card = lv_obj_create(content_area);
        lv_obj_set_size(card, 240, 160);
        lv_obj_add_style(card, &style_card, 0);
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, NULL);

        if (filter_mode == 1 || remaining <= 3.0) {
            lv_obj_set_style_border_width(card, 2, 0);
            lv_obj_set_style_border_color(card, remaining < 0 ? COLOR_DANGER : COLOR_WARNING, 0);
        }

        // Title
        lv_obj_t * title = lv_label_create(card);
        lv_label_set_text(title, item.name.c_str());
        lv_obj_add_style(title, &style_text_title, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

        // Category Tag
        lv_obj_t * cat_tag = lv_label_create(card);
        lv_label_set_text(cat_tag, item.category.c_str());
        lv_obj_set_style_bg_color(cat_tag, COLOR_PRIMARY, 0);
        lv_obj_set_style_text_color(cat_tag, lv_color_white(), 0);
        lv_obj_set_style_radius(cat_tag, 8, 0);
        lv_obj_set_style_pad_all(cat_tag, 4, 0);
        lv_obj_set_style_text_font(cat_tag, font_cn_16, 0);
        lv_obj_align(cat_tag, LV_ALIGN_TOP_RIGHT, 0, 0);

        // Quantity
        lv_obj_t * qty = lv_label_create(card);
        lv_label_set_text_fmt(qty, "x %d", item.total_quantity);
        lv_obj_add_style(qty, &style_text_sub, 0);
        lv_obj_align(qty, LV_ALIGN_LEFT_MID, 0, -10);

        // Suggestion
        lv_obj_t * suggestion = lv_label_create(card);
        if (remaining < 0) {
            lv_label_set_text(suggestion, "尽快食用！");
            lv_obj_set_style_text_color(suggestion, COLOR_DANGER, 0);
        } else if (remaining <= 3.0) {
            lv_label_set_text_fmt(suggestion, "建议 %d 天内食用", (int)remaining);
            lv_obj_set_style_text_color(suggestion, COLOR_WARNING, 0);
        } else {
            lv_label_set_text_fmt(suggestion, "建议 %d 天内食用", (int)remaining);
            lv_obj_set_style_text_color(suggestion, COLOR_SUCCESS, 0);
        }
        lv_obj_set_style_text_font(suggestion, font_cn_16, 0);
        lv_obj_align(suggestion, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    if (count == 0) {
        lv_obj_t * empty_label = lv_label_create(content_area);
        lv_label_set_text(empty_label, filter_mode == 1 ? "所有食材都很新鲜！" : "没有找到食材。");
        lv_obj_add_style(empty_label, &style_text_sub, 0);
        lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
    }
}

static void sidebar_btn_event_cb(lv_event_t * e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    render_inventory_items(id);
}

lv_obj_t* app_inventory_create(void) {
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

    // Sidebar menus
    const char * menus[] = {"全部食材", "临期预警", "水果", "蔬菜", "其他"};
    for(int i=0; i<5; i++) {
        lv_obj_t * menu_btn = lv_btn_create(sidebar);
        lv_obj_set_size(menu_btn, LV_PCT(100), 50);
        lv_obj_align(menu_btn, LV_ALIGN_TOP_MID, 0, 80 + i * 55);
        lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(menu_btn, 0, 0);
        lv_obj_add_event_cb(menu_btn, sidebar_btn_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t * label = lv_label_create(menu_btn);
        lv_label_set_text(label, menus[i]);
        if (i >= 2) {
            // Smaller padding, slightly gray for sub-categories
            lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
            lv_obj_set_style_text_font(label, font_cn_16, 0);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 40, 0);
        } else {
            lv_obj_set_style_text_color(label, COLOR_TEXT_MAIN, 0);
            lv_obj_set_style_text_font(label, font_cn_18, 0);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);
        }
    }

    // Main Content Area
    content_area = lv_obj_create(screen);
    // Fixed: Do not subtract pixels from LV_PCT macro. Calculate explicitly: 600 - 70 = 530
    lv_obj_set_size(content_area, 824, 530); 
    lv_obj_align(content_area, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(content_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_area, 0, 0);
    lv_obj_set_flex_flow(content_area, LV_FLEX_FLOW_ROW_WRAP);
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
    
    // Bottom actions: Camera, Voice, My Recipes
    lv_obj_t * cam_btn = lv_btn_create(bottom_bar);
    lv_obj_align(cam_btn, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_t * cam_icon = lv_label_create(cam_btn);
    lv_label_set_text(cam_icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(cam_icon, &lv_font_montserrat_18, 0);
    lv_obj_t * cam_lbl = lv_label_create(cam_btn);
    lv_label_set_text(cam_lbl, " 拍照录入");
    lv_obj_set_style_text_font(cam_lbl, font_cn_18, 0);
    lv_obj_set_flex_flow(cam_btn, LV_FLEX_FLOW_ROW);

    lv_obj_t * mic_btn = lv_btn_create(bottom_bar);
    lv_obj_align(mic_btn, LV_ALIGN_LEFT_MID, 180, 0);
    lv_obj_t * mic_icon = lv_label_create(mic_btn);
    lv_label_set_text(mic_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(mic_icon, &lv_font_montserrat_18, 0);
    lv_obj_t * mic_lbl = lv_label_create(mic_btn);
    lv_label_set_text(mic_lbl, " 语音录入");
    lv_obj_set_style_text_font(mic_lbl, font_cn_18, 0);
    lv_obj_set_flex_flow(mic_btn, LV_FLEX_FLOW_ROW);

    lv_obj_t * recipe_btn = lv_btn_create(bottom_bar);
    lv_obj_align(recipe_btn, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_color(recipe_btn, COLOR_PRIMARY, 0);
    lv_obj_add_event_cb(recipe_btn, nav_recipes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * recipe_lbl = lv_label_create(recipe_btn);
    lv_label_set_text(recipe_lbl, "我的食谱 →");
    lv_obj_set_style_text_font(recipe_lbl, font_cn_18, 0);
    lv_obj_set_style_text_color(recipe_lbl, lv_color_white(), 0);

    // Initial render
    render_inventory_items(0);

    return screen;
}

void app_inventory_refresh(void) {
    // 仅当库存应用的内容区仍然有效（即用户正在库存页）时才真正重绘
    if (content_area && lv_obj_is_valid(content_area)) {
        render_inventory_items(s_current_filter);
    }
}
