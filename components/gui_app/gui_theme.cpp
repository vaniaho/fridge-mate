#include "gui_theme.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "GuiTheme";

// -------- Fonts --------
const lv_font_t *font_cn_16 = &lv_font_montserrat_14;
const lv_font_t *font_cn_18 = &lv_font_montserrat_14;
const lv_font_t *font_cn_24 = &lv_font_montserrat_14;
const lv_font_t *font_cn_36 = &lv_font_montserrat_14;
const lv_font_t *font_icon_24 = &lv_font_montserrat_14;
const lv_font_t *font_icon_36 = &lv_font_montserrat_14;

// -------- Styles --------
lv_style_t style_screen_bg;
lv_style_t style_card;
lv_style_t style_text_main;
lv_style_t style_text_sub;
lv_style_t style_text_title;
lv_style_t style_text_caption;

#if LV_USE_FREETYPE
#include "extra/libs/freetype/lv_freetype.h"

static bool load_ttf_to_psram(const char *path, void **mem_out, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    *size_out = ftell(f);
    fseek(f, 0, SEEK_SET);
    *mem_out = heap_caps_malloc(*size_out, MALLOC_CAP_SPIRAM);
    if (*mem_out) {
        fread(*mem_out, 1, *size_out, f);
        ESP_LOGI(TAG, "Cached %s into PSRAM (%u bytes)", path, (unsigned)*size_out);
    } else {
        ESP_LOGE(TAG, "Failed to allocate PSRAM for %s", path);
    }
    fclose(f);
    return true;
}

static void init_freetype_font(const char *path, void *mem, size_t mem_size,
                                uint16_t weight, lv_ft_info_t *info,
                                const lv_font_t **out_font) {
    memset(info, 0, sizeof(lv_ft_info_t));
    info->name = path;
    info->mem = mem;
    info->mem_size = mem_size;
    info->weight = weight;
    info->style = FT_FONT_STYLE_NORMAL;
    if (lv_ft_font_init(info)) {
        *out_font = info->font;
    } else {
        ESP_LOGE(TAG, "Failed to load font %s at weight %u", path, weight);
    }
}
#endif

void gui_theme_init(void) {
#if LV_USE_FREETYPE
    ESP_LOGI(TAG, "Initializing FreeType fonts...");
    if (lv_freetype_init(64, 8, 0)) {
        void *cn_mem = NULL;
        size_t cn_size = 0;
        load_ttf_to_psram("/internal/NotoSansSC-Regular.ttf", &cn_mem, &cn_size);

        void *icon_mem = NULL;
        size_t icon_size = 0;
        load_ttf_to_psram("/internal/MaterialIcons-Regular.ttf", &icon_mem, &icon_size);

        lv_ft_info_t info;

        // Chinese fonts
        init_freetype_font("/internal/NotoSansSC-Regular.ttf", cn_mem, cn_size, 16, &info, &font_cn_16);
        init_freetype_font("/internal/NotoSansSC-Regular.ttf", cn_mem, cn_size, 18, &info, &font_cn_18);
        init_freetype_font("/internal/NotoSansSC-Regular.ttf", cn_mem, cn_size, 24, &info, &font_cn_24);
        init_freetype_font("/internal/NotoSansSC-Regular.ttf", cn_mem, cn_size, 36, &info, &font_cn_36);

        // Icon fonts
        init_freetype_font("/internal/MaterialIcons-Regular.ttf", icon_mem, icon_size, 24, &info, &font_icon_24);
        init_freetype_font("/internal/MaterialIcons-Regular.ttf", icon_mem, icon_size, 36, &info, &font_icon_36);

        ESP_LOGI(TAG, "Fonts loaded successfully");
    } else {
        ESP_LOGE(TAG, "Failed to initialize FreeType");
    }
#else
    ESP_LOGW(TAG, "FreeType not enabled. Using fallback fonts.");
#endif

    // Screen background
    lv_style_init(&style_screen_bg);
    lv_style_set_bg_color(&style_screen_bg, THEME_BG);
    lv_style_set_text_font(&style_screen_bg, font_cn_18);
    lv_style_set_text_color(&style_screen_bg, THEME_TEXT_MAIN);

    // Card
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, THEME_SURFACE);
    lv_style_set_radius(&style_card, THEME_RADIUS_M);
    lv_style_set_shadow_width(&style_card, 8);
    lv_style_set_shadow_ofs_y(&style_card, 3);
    lv_style_set_shadow_opa(&style_card, LV_OPA_10);
    lv_style_set_shadow_color(&style_card, THEME_SHADOW);
    lv_style_set_border_width(&style_card, 0);
    lv_style_set_pad_all(&style_card, 0);

    // Text main
    lv_style_init(&style_text_main);
    lv_style_set_text_color(&style_text_main, THEME_TEXT_MAIN);
    lv_style_set_text_font(&style_text_main, font_cn_18);

    // Text sub
    lv_style_init(&style_text_sub);
    lv_style_set_text_color(&style_text_sub, THEME_TEXT_SUB);
    lv_style_set_text_font(&style_text_sub, font_cn_16);

    // Text title
    lv_style_init(&style_text_title);
    lv_style_set_text_color(&style_text_title, THEME_TEXT_MAIN);
    lv_style_set_text_font(&style_text_title, font_cn_24);

    // Text caption
    lv_style_init(&style_text_caption);
    lv_style_set_text_color(&style_text_caption, THEME_TEXT_SUB);
    lv_style_set_text_font(&style_text_caption, font_cn_16);
}

void theme_apply_gradient_bg(lv_obj_t *obj, lv_color_t start, lv_color_t end) {
    lv_obj_set_style_bg_color(obj, start, 0);
    lv_obj_set_style_bg_grad_color(obj, end, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_HOR, 0);
}

static void press_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);
    if (code == LV_EVENT_PRESSED) {
        lv_obj_set_style_transform_zoom(obj, 240, 0);   // 94%
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_transform_zoom(obj, 256, 0);   // 100%
    }
}

void theme_apply_press_effect(lv_obj_t *obj) {
    lv_obj_set_style_transform_zoom(obj, 256, 0);
    lv_obj_add_event_cb(obj, press_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, press_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, press_event_cb, LV_EVENT_PRESS_LOST, NULL);
}
