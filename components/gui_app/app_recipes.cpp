#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include "gui_theme.h"
#include "gui_components.h"
#include "gui_icons.h"
#include "recipe_matcher.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

static lv_obj_t * content_area;
// 记录当前食谱页的筛选模式，供外部刷新调用复用
static int s_current_filter = 0;

static void back_btn_event_cb(lv_event_t * e) {
    gui_app_navigate_to(GUI_APP_LAUNCHER);
}

static void nav_inventory_cb(lv_event_t * e) {
    gui_app_navigate_to(GUI_APP_INVENTORY);
}

static void btn_close_popup_cb(lv_event_t * e) {
    lv_obj_t * popup_bg = (lv_obj_t *)lv_event_get_user_data(e);
    if (popup_bg) {
        lv_obj_del(popup_bg);
    }
}

static void create_recipe_detail_popup(const std::string& name) {
    // 查找该食谱并获取匹配状态
    auto matches = smart_fridge::inventory::recipe_match_near(99);
    const smart_fridge::inventory::RecipeMatch* p_match = nullptr;
    for (const auto& m : matches) {
        if (m.recipe.name == name) {
            p_match = &m;
            break;
        }
    }
    if (!p_match) return;

    lv_obj_t * screen = lv_scr_act();

    // 背景半透明遮罩
    lv_obj_t * popup_bg = lv_obj_create(screen);
    lv_obj_set_size(popup_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(popup_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_bg, LV_OPA_50, 0);
    lv_obj_set_style_border_width(popup_bg, 0, 0);
    lv_obj_set_style_radius(popup_bg, 0, 0);
    lv_obj_add_event_cb(popup_bg, [](lv_event_t* e){ /* 阻止穿透 */ }, LV_EVENT_ALL, NULL);

    // 弹窗主体
    lv_obj_t * popup = lv_obj_create(popup_bg);
    lv_obj_set_size(popup, 540, 440);
    lv_obj_center(popup);
    lv_obj_set_style_bg_color(popup, lv_color_white(), 0);
    lv_obj_set_style_radius(popup, 16, 0);
    lv_obj_set_style_shadow_width(popup, 20, 0);
    lv_obj_set_style_shadow_opa(popup, LV_OPA_30, 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t * title = lv_label_create(popup);
    lv_label_set_text(title, p_match->recipe.name.c_str());
    lv_obj_set_style_text_font(title, font_cn_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 20);

    // 分类标签
    lv_obj_t * cat_tag = lv_label_create(popup);
    lv_label_set_text(cat_tag, p_match->recipe.category.c_str());
    lv_obj_set_style_bg_color(cat_tag, THEME_PRIMARY, 0);
    lv_obj_set_style_text_color(cat_tag, lv_color_white(), 0);
    lv_obj_set_style_radius(cat_tag, 8, 0);
    lv_obj_set_style_pad_all(cat_tag, 4, 0);
    lv_obj_set_style_text_font(cat_tag, font_cn_16, 0);
    lv_obj_align_to(cat_tag, title, LV_ALIGN_OUT_RIGHT_MID, 15, 0);

    // 关闭按钮
    lv_obj_t * close_btn = lv_btn_create(popup);
    lv_obj_set_size(close_btn, 40, 40);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(close_btn, THEME_DIVIDER, 0);
    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, ICON_CLOSE);
    lv_obj_set_style_text_font(close_lbl, font_icon_24, 0);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, btn_close_popup_cb, LV_EVENT_CLICKED, popup_bg);

    // 标题：所需食材
    lv_obj_t * ing_title = lv_label_create(popup);
    lv_label_set_text(ing_title, "所需食材：");
    lv_obj_set_style_text_font(ing_title, font_cn_18, 0);
    lv_obj_align(ing_title, LV_ALIGN_TOP_LEFT, 20, 70);

    // 食材列表区域 (可滚动)
    lv_obj_t * ing_list = lv_obj_create(popup);
    lv_obj_set_size(ing_list, 480, 120);
    lv_obj_align(ing_list, LV_ALIGN_TOP_LEFT, 20, 100);
    lv_obj_set_flex_flow(ing_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(ing_list, 1, 0);
    lv_obj_set_style_border_color(ing_list, THEME_DIVIDER, 0);

    for (const auto& ing : p_match->recipe.ingredients) {
        lv_obj_t * row = lv_obj_create(ing_list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(row, 5, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        bool is_missing = (std::find(p_match->missing_items.begin(), p_match->missing_items.end(), ing.name) != p_match->missing_items.end());

        lv_obj_t * lbl = lv_label_create(row);
        lv_label_set_text_fmt(lbl, "• %s (需 %d)", ing.name.c_str(), ing.min_quantity);
        lv_obj_set_style_text_font(lbl, font_cn_18, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t * status = lv_label_create(row);
        if (is_missing) {
            lv_label_set_text(status, "缺少");
            lv_obj_set_style_text_color(status, THEME_DANGER, 0);
        } else {
            lv_label_set_text(status, "已备齐");
            lv_obj_set_style_text_color(status, THEME_SUCCESS, 0);
        }
        lv_obj_set_style_text_font(status, font_cn_18, 0);
        lv_obj_align(status, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    // 标题：做法说明
    lv_obj_t * brief_title = lv_label_create(popup);
    lv_label_set_text(brief_title, "做法说明：");
    lv_obj_set_style_text_font(brief_title, font_cn_18, 0);
    lv_obj_align(brief_title, LV_ALIGN_TOP_LEFT, 20, 240);

    // 做法内容 (可滚动文本区)
    lv_obj_t * brief_box = lv_obj_create(popup);
    lv_obj_set_size(brief_box, 480, 120);
    lv_obj_align(brief_box, LV_ALIGN_TOP_LEFT, 20, 270);
    lv_obj_set_style_bg_color(brief_box, THEME_BG, 0);
    lv_obj_set_style_border_width(brief_box, 0, 0);
    
    lv_obj_t * brief_text = lv_label_create(brief_box);
    lv_label_set_long_mode(brief_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(brief_text, 440);
    lv_label_set_text(brief_text, p_match->recipe.brief.c_str());
    lv_obj_set_style_text_font(brief_text, font_cn_18, 0);
    lv_obj_set_style_text_color(brief_text, THEME_TEXT_MAIN, 0);
}

static void card_click_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    char * name = (char*)lv_event_get_user_data(e);
    if (!name) return;

    if (code == LV_EVENT_CLICKED) {
        create_recipe_detail_popup(name);
    } else if (code == LV_EVENT_DELETE) {
        free(name);
    }
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
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_ALL, strdup(match.recipe.name.c_str()));

        // Title
        lv_obj_t * title = lv_label_create(card);
        lv_label_set_text(title, match.recipe.name.c_str());
        lv_obj_add_style(title, &style_text_title, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

        // Category Tag
        lv_obj_t * cat_tag = lv_label_create(card);
        lv_label_set_text(cat_tag, match.recipe.category.c_str());
        lv_obj_set_style_bg_color(cat_tag, THEME_PRIMARY, 0);
        lv_obj_set_style_text_color(cat_tag, lv_color_white(), 0);
        lv_obj_set_style_radius(cat_tag, 8, 0);
        lv_obj_set_style_pad_all(cat_tag, 4, 0);
        lv_obj_set_style_text_font(cat_tag, font_cn_16, 0);
        lv_obj_align(cat_tag, LV_ALIGN_TOP_RIGHT, 0, 0);

        // Match Status
        lv_obj_t * status = lv_label_create(card);
        if (match.missing_count == 0) {
            lv_label_set_text(status, "100% 匹配");
            lv_obj_set_style_text_color(status, THEME_SUCCESS, 0);
        } else {
            lv_label_set_text_fmt(status, "缺: %d 种食材", match.missing_count);
            lv_obj_set_style_text_color(status, THEME_WARNING, 0);
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
        lv_obj_t * empty = gui_empty_state_create(content_area, ICON_RECIPES,
            "还没有食谱", "先录入食材，AI 会为你推荐做法");
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
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
    lv_obj_set_style_bg_color(sidebar, THEME_SURFACE, 0);
    lv_obj_set_style_border_width(sidebar, 0, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);

    lv_obj_t * back_btn = lv_btn_create(sidebar);
    lv_obj_set_size(back_btn, LV_PCT(90), 50);
    lv_obj_align(back_btn, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(back_btn, THEME_BG, 0);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "← 返回桌面");
    lv_obj_set_style_text_color(back_label, THEME_TEXT_MAIN, 0);
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
        lv_obj_set_style_text_color(label, THEME_TEXT_MAIN, 0);
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
    lv_obj_set_style_bg_color(bottom_bar, THEME_SURFACE, 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_clear_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * rand_btn = lv_btn_create(bottom_bar);
    lv_obj_align(rand_btn, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_t * rand_icon = lv_label_create(rand_btn);
    lv_label_set_text(rand_icon, ICON_RECIPES);
    lv_obj_set_style_text_font(rand_icon, font_icon_24, 0);
    lv_obj_t * rand_lbl = lv_label_create(rand_btn);
    lv_label_set_text(rand_lbl, " AI推荐");
    lv_obj_set_style_text_font(rand_lbl, font_cn_18, 0);
    lv_obj_set_flex_flow(rand_btn, LV_FLEX_FLOW_ROW);

    lv_obj_t * inv_btn = lv_btn_create(bottom_bar);
    lv_obj_align(inv_btn, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_color(inv_btn, THEME_PRIMARY, 0);
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
