#pragma once

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 根据食材名称推断分类
 * 基于内置的品类知识库匹配，未匹配时返回 "其他"
 * @param name 食材名称
 * @return 分类名称 (如 "水果"、"蔬菜"、"肉禽" 等)
 */
const char* category_lookup(const char* name);

/**
 * @brief 根据食材分类获取默认保质期天数
 * @param category 分类名称
 * @return 保质期天数
 */
int default_expire_days(const char* category);

/**
 * @brief 一步到位：根据食材名称获取默认保质期天数
 * 内部先 category_lookup 推断分类，再 default_expire_days 查保质期
 * @param name 食材名称
 * @return 保质期天数
 */
int expire_days_for_item(const char* name);

#ifdef __cplusplus
}
#endif
