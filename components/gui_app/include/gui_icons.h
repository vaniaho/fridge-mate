#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Material Icons codepoints encoded as 3-byte UTF-8 strings.
 * These map to the MaterialIcons-Regular.ttf shipped in internal_fs_data.
 */

/* Navigation / UI */
#define ICON_HOME            "\xee\xa2\x8a"  // 0xe88a
#define ICON_BACK            "\xee\x97\x84"  // 0xe5c4
#define ICON_FORWARD         "\xee\x97\x88"  // 0xe5c8
#define ICON_CLOSE           "\xee\x97\x8d"  // 0xe5cd
#define ICON_SETTINGS        "\xee\xa2\xb8"  // 0xe8b8
#define ICON_INFO            "\xee\xa2\x8e"  // 0xe88e
#define ICON_CHECK           "\xee\x97\x8a"  // 0xe5ca
#define ICON_ADD             "\xee\x85\x85"  // 0xe145
#define ICON_REMOVE          "\xee\x85\x9b"  // 0xe15b
#define ICON_MORE            "\xee\x97\x93"  // 0xe5d3

/* Communication / Input */
#define ICON_MIC             "\xee\x80\xa9"  // 0xe029
#define ICON_WIFI_4          "\xee\x87\x98"  // 0xe1d8
#define ICON_WIFI_OFF        "\xee\x87\x9a"  // 0xe1da
#define ICON_NOTIFICATION    "\xee\x9f\xb4"  // 0xe7f4

/* Apps */
#define ICON_INVENTORY       "\xee\x85\xb9"  // 0xe179
#define ICON_RECIPES         "\xee\x95\xa1"  // 0xe561
#define ICON_VOICE           "\xee\x80\xa9"  // 0xe029
#define ICON_SHOPPING        "\xee\xa3\x8c"  // 0xe8cc
#define ICON_CAMERA          "\xee\x90\x92"  // 0xe412

/* Weather */
#define ICON_SUNNY           "\xee\xa0\x9a"  // 0xe81a
#define ICON_CLOUDY          "\xee\x8a\xbd"  // 0xe2bd
#define ICON_RAIN            "\xee\x9e\x98"  // 0xe798
#define ICON_SNOW            "\xee\xac\xbb"  // 0xeb3b
#define ICON_FOG             "\xee\xa0\x98"  // 0xe818
#define ICON_STORM           "\xef\x81\xb0"  // 0xf070

/* Food categories */
#define ICON_FRUIT           "\xee\x95\x85"  // 0xe545
#define ICON_VEGETABLE       "\xee\xa8\xb5"  // 0xea35
#define ICON_MEAT            "\xee\xa1\x82"  // 0xe842
#define ICON_SEAFOOD         "\xee\xa9\xa4"  // 0xea64
#define ICON_EGG             "\xee\xab\x8c"  // 0xeacc
#define ICON_DAIRY           "\xee\x95\x81"  // 0xe541
#define ICON_SOY             "\xef\x87\xb5"  // 0xf1f5
#define ICON_STAPLE          "\xef\x87\xb5"  // 0xf1f5
#define ICON_SEASONING       "\xee\xa2\x91"  // 0xe891
#define ICON_DRINK           "\xee\x95\x84"  // 0xe544
#define ICON_SNACK           "\xee\x9f\xa9"  // 0xe7e9
#define ICON_FROZEN          "\xee\xac\xbb"  // 0xeb3b
#define ICON_OTHER           "\xee\x97\x93"  // 0xe5d3

/**
 * @brief Convert a Material Icons codepoint to a temporary UTF-8 string.
 *        Do NOT call twice in the same expression; uses a static buffer.
 */
const char *gui_icon_codepoint(uint32_t cp);

#ifdef __cplusplus
}
#endif
