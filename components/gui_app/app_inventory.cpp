#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include "gui_theme.h"
#include "gui_components.h"
#include "gui_icons.h"
#include "inventory.hpp"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t * content_area;
// 记录当前库存页的筛选模式，供外部刷新调用复用
static int s_current_filter = 0;

static void back_btn_event_cb(lv_event_t * e) {
    gui_app_navigate_to(GUI_APP_LAUNCHER);
}

static void nav_recipes_cb(lv_event_t * e) {
    gui_app_navigate_to(GUI_APP_RECIPES);
}

static void btn_add_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    char * name = (char*)lv_event_get_user_data(e);
    if (!name) return;

    if (code == LV_EVENT_CLICKED) {
        auto items = smart_fridge::inventory::get_all_ingredients();
        for (auto& it : items) {
            if (it.name == name) {
                int expire_days = it.batches.empty() ? 7 : it.batches.front().expire_days;
                auto result = smart_fridge::inventory::add_ingredient_checked(
                    name, it.category, 1, expire_days);
                if (result.ok()) {
                    app_inventory_refresh();
                    gui_app_show_notification("库存已更新", "已增加 1 个食材");
                } else {
                    gui_app_show_notification(
                        "操作失败",
                        smart_fridge::inventory::inventory_error_message(result.error));
                }
                break;
            }
        }
    } else if (code == LV_EVENT_DELETE) {
        free(name);
    }
}

static void btn_sub_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    char * name = (char*)lv_event_get_user_data(e);
    if (!name) return;

    if (code == LV_EVENT_CLICKED) {
        auto result = smart_fridge::inventory::remove_ingredient_checked(name, 1);
        if (result.ok()) {
            app_inventory_refresh();
            gui_app_show_notification("库存已更新", "已取出 1 个食材");
        } else {
            gui_app_show_notification(
                "操作失败",
                smart_fridge::inventory::inventory_error_message(result.error));
        }
    } else if (code == LV_EVENT_DELETE) {
        free(name);
    }
}

static void btn_close_popup_cb(lv_event_t * e) {
    lv_obj_t * popup_bg = (lv_obj_t *)lv_event_get_user_data(e);
    if (popup_bg) {
        lv_obj_del(popup_bg);
    }
}

// 提取的详情弹窗逻辑
static void create_ingredient_detail_popup(const std::string& name) {
    auto items = smart_fridge::inventory::get_all_ingredients();
    smart_fridge::inventory::IngredientType* p_item = nullptr;
    for (auto& it : items) {
        if (it.name == name) {
            p_item = &it;
            break;
        }
    }
    if (!p_item) return;

    lv_obj_t * screen = lv_scr_act();

    // 背景半透明遮罩
    lv_obj_t * popup_bg = lv_obj_create(screen);
    lv_obj_set_size(popup_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(popup_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_bg, LV_OPA_50, 0);
    lv_obj_set_style_border_width(popup_bg, 0, 0);
    lv_obj_set_style_radius(popup_bg, 0, 0);
    lv_obj_add_event_cb(popup_bg, [](lv_event_t* e){ /* 阻止事件穿透 */ }, LV_EVENT_ALL, NULL);

    // 弹窗主体容器
    lv_obj_t * popup = lv_obj_create(popup_bg);
    lv_obj_set_size(popup, 500, 400);
    lv_obj_center(popup);
    lv_obj_set_style_bg_color(popup, lv_color_white(), 0);
    lv_obj_set_style_radius(popup, 16, 0);
    lv_obj_set_style_shadow_width(popup, 20, 0);
    lv_obj_set_style_shadow_opa(popup, LV_OPA_30, 0);

    // 标题
    lv_obj_t * title = lv_label_create(popup);
    lv_label_set_text_fmt(title, "%s 详情", p_item->name.c_str());
    lv_obj_set_style_text_font(title, font_cn_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 20);

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

    // 信息文本
    lv_obj_t * info = lv_label_create(popup);
    lv_label_set_text_fmt(info, "分类: %s\n总数量: %d", p_item->category.c_str(), p_item->total_quantity);
    lv_obj_set_style_text_font(info, font_cn_18, 0);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 20, 70);

    // 批次列表（可滚动）
    lv_obj_t * batch_list = lv_obj_create(popup);
    lv_obj_set_size(batch_list, 460, 200);
    lv_obj_align(batch_list, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_flex_flow(batch_list, LV_FLEX_FLOW_COLUMN);
    
    time_t now;
    time(&now);

    for (const auto& batch : p_item->batches) {
        lv_obj_t * item = lv_obj_create(batch_list);
        lv_obj_set_size(item, LV_PCT(100), 50);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_border_color(item, THEME_DIVIDER, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t * batch_lbl = lv_label_create(item);
        double remaining = difftime(batch.expire_time, now) / (24.0 * 3600.0);
        lv_label_set_text_fmt(batch_lbl, "数量: %d | 剩余: %d 天", batch.quantity, (int)remaining);
        lv_obj_set_style_text_font(batch_lbl, font_cn_16, 0);
        lv_obj_align(batch_lbl, LV_ALIGN_LEFT_MID, 10, 0);

        if (remaining < 0) lv_obj_set_style_text_color(batch_lbl, THEME_DANGER, 0);
        else if (remaining <= 3.0) lv_obj_set_style_text_color(batch_lbl, THEME_WARNING, 0);
    }

    if (p_item->batches.empty()) {
        lv_obj_t * empty = lv_label_create(batch_list);
        lv_label_set_text(empty, "暂无批次信息");
        lv_obj_set_style_text_font(empty, font_cn_16, 0);
        lv_obj_center(empty);
    }

    // 底部操作区
    lv_obj_t * btn_take = lv_btn_create(popup);
    lv_obj_set_size(btn_take, 140, 40);
    lv_obj_align(btn_take, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_set_style_bg_color(btn_take, THEME_DANGER, 0);
    lv_obj_t * lbl_take = lv_label_create(btn_take);
    lv_label_set_text(lbl_take, "全部取出");
    lv_obj_set_style_text_font(lbl_take, font_cn_18, 0);
    lv_obj_center(lbl_take);
    // 这里使用一个 lambda + 捕获 popup_bg，但 LVGL 事件是 C 函数。
    // 为了安全，我们可以复用 btn_take_all_cb 并将 popup_bg 存在 btn 的 user_data 里，把 name 放哪？
    // 我们可以专门写一个弹出层的 take all callback
    struct TakeAllData {
        char* name;
        lv_obj_t* popup_bg;
        bool armed;
    };
    TakeAllData* data = (TakeAllData*)malloc(sizeof(TakeAllData));
    if (!data) {
        gui_app_show_notification("操作失败", "内存不足，无法创建确认操作");
        return;
    }
    data->name = strdup(name.c_str());
    if (!data->name) {
        free(data);
        gui_app_show_notification("操作失败", "内存不足，无法创建确认操作");
        return;
    }
    data->popup_bg = popup_bg;
    data->armed = false;

    lv_obj_add_event_cb(btn_take, [](lv_event_t* e){
        lv_event_code_t code = lv_event_get_code(e);
        TakeAllData* d = (TakeAllData*)lv_event_get_user_data(e);
        if (code == LV_EVENT_CLICKED) {
            lv_obj_t* button = lv_event_get_target(e);
            lv_obj_t* label = lv_obj_get_child(button, 0);
            if (!d->armed) {
                d->armed = true;
                if (label) lv_label_set_text(label, "再次点击确认");
                gui_app_show_notification("请确认", "再次点击将清空该食材全部库存");
                return;
            }

            auto result = smart_fridge::inventory::clear_ingredient_checked(d->name);
            if (result.ok()) {
                app_inventory_refresh();
                gui_app_show_notification("库存已更新", "该食材已全部取出");
                lv_obj_del(d->popup_bg);
            } else {
                d->armed = false;
                if (label) lv_label_set_text(label, "全部取出");
                gui_app_show_notification(
                    "操作失败",
                    smart_fridge::inventory::inventory_error_message(result.error));
            }
        } else if (code == LV_EVENT_DELETE) {
            free(d->name);
            free(d);
        }
    }, LV_EVENT_ALL, data);
}

static void card_click_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    char * name = (char*)lv_event_get_user_data(e);
    if (!name) return;

    if (code == LV_EVENT_CLICKED) {
        create_ingredient_detail_popup(name);
    } else if (code == LV_EVENT_DELETE) {
        free(name);
    }
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
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_ALL, strdup(item.name.c_str()));

        if (filter_mode == 1 || remaining <= 3.0) {
            lv_obj_set_style_border_width(card, 2, 0);
            lv_obj_set_style_border_color(card, remaining < 0 ? THEME_DANGER : THEME_WARNING, 0);
        }

        // Title
        lv_obj_t * title = lv_label_create(card);
        lv_label_set_text(title, item.name.c_str());
        lv_obj_add_style(title, &style_text_title, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

        // Category Tag
        lv_obj_t * cat_tag = lv_label_create(card);
        lv_label_set_text(cat_tag, item.category.c_str());
        lv_obj_set_style_bg_color(cat_tag, THEME_PRIMARY, 0);
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
            lv_obj_set_style_text_color(suggestion, THEME_DANGER, 0);
        } else if (remaining <= 3.0) {
            lv_label_set_text_fmt(suggestion, "建议 %d 天内食用", (int)remaining);
            lv_obj_set_style_text_color(suggestion, THEME_WARNING, 0);
        } else {
            lv_label_set_text_fmt(suggestion, "建议 %d 天内食用", (int)remaining);
            lv_obj_set_style_text_color(suggestion, THEME_SUCCESS, 0);
        }
        lv_obj_set_style_text_font(suggestion, font_cn_16, 0);
        lv_obj_align(suggestion, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        // +/- 按钮
        lv_obj_t * btn_minus = lv_btn_create(card);
        lv_obj_set_size(btn_minus, 32, 32);
        lv_obj_align(btn_minus, LV_ALIGN_BOTTOM_RIGHT, -40, 0);
        lv_obj_set_style_bg_color(btn_minus, THEME_BG, 0);
        lv_obj_set_style_text_color(btn_minus, THEME_TEXT_MAIN, 0);
        lv_obj_set_style_radius(btn_minus, 16, 0);
        lv_obj_set_style_pad_all(btn_minus, 0, 0);
        lv_obj_t * lbl_minus = lv_label_create(btn_minus);
        lv_label_set_text(lbl_minus, ICON_REMOVE);
        lv_obj_set_style_text_font(lbl_minus, font_icon_24, 0);
        lv_obj_center(lbl_minus);
        lv_obj_add_event_cb(btn_minus, btn_sub_cb, LV_EVENT_ALL, strdup(item.name.c_str()));

        lv_obj_t * btn_plus = lv_btn_create(card);
        lv_obj_set_size(btn_plus, 32, 32);
        lv_obj_align(btn_plus, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_bg_color(btn_plus, THEME_PRIMARY, 0);
        lv_obj_set_style_text_color(btn_plus, lv_color_white(), 0);
        lv_obj_set_style_radius(btn_plus, 16, 0);
        lv_obj_set_style_pad_all(btn_plus, 0, 0);
        lv_obj_t * lbl_plus = lv_label_create(btn_plus);
        lv_label_set_text(lbl_plus, ICON_ADD);
        lv_obj_set_style_text_font(lbl_plus, font_icon_24, 0);
        lv_obj_center(lbl_plus);
        lv_obj_add_event_cb(btn_plus, btn_add_cb, LV_EVENT_ALL, strdup(item.name.c_str()));
    }

    if (count == 0) {
        lv_obj_t * empty = gui_empty_state_create(content_area, ICON_INVENTORY,
            filter_mode == 1 ? "所有食材都很新鲜" : "没有找到食材",
            filter_mode == 1 ? "近期没有临期食材" : "点击拍照或语音录入食材");
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
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
            lv_obj_set_style_text_color(label, THEME_TEXT_MAIN, 0);
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
    lv_obj_set_style_bg_color(bottom_bar, THEME_SURFACE, 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_clear_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);
    
    // Bottom actions: Camera, Voice, My Recipes
    lv_obj_t * cam_btn = lv_btn_create(bottom_bar);
    lv_obj_align(cam_btn, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_t * cam_icon = lv_label_create(cam_btn);
    lv_label_set_text(cam_icon, ICON_CAMERA);
    lv_obj_set_style_text_font(cam_icon, font_icon_24, 0);
    lv_obj_t * cam_lbl = lv_label_create(cam_btn);
    lv_label_set_text(cam_lbl, " 拍照录入");
    lv_obj_set_style_text_font(cam_lbl, font_cn_18, 0);
    lv_obj_set_flex_flow(cam_btn, LV_FLEX_FLOW_ROW);

    lv_obj_t * mic_btn = lv_btn_create(bottom_bar);
    lv_obj_align(mic_btn, LV_ALIGN_LEFT_MID, 180, 0);
    lv_obj_t * mic_icon = lv_label_create(mic_btn);
    lv_label_set_text(mic_icon, ICON_MIC);
    lv_obj_set_style_text_font(mic_icon, font_icon_24, 0);
    lv_obj_t * mic_lbl = lv_label_create(mic_btn);
    lv_label_set_text(mic_lbl, " 语音录入");
    lv_obj_set_style_text_font(mic_lbl, font_cn_18, 0);
    lv_obj_set_flex_flow(mic_btn, LV_FLEX_FLOW_ROW);

    lv_obj_t * recipe_btn = lv_btn_create(bottom_bar);
    lv_obj_align(recipe_btn, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_color(recipe_btn, THEME_PRIMARY, 0);
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
