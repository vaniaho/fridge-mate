#pragma once

#include <stdint.h>
#include <time.h>
#include <string>
#include <vector>
#include "sd_storage.h"
#include "inventory_history.hpp"
#include "recipe_matcher.hpp"
#include "category_table.h"

namespace smart_fridge {
namespace inventory {

    /**
     * @brief 食材批次结构体
     */
    struct IngredientBatch {
        int batch_id;          // 批次唯一标识 ID
        int quantity;          // 该批次的当前剩余数量
        time_t entry_time;     // 存入时间戳
        int expire_days;       // 保质期天数
        time_t expire_time;    // 预计过期时间戳
    };

    /**
     * @brief 食材种类结构体（包含多个批次）
     */
    struct IngredientType {
        int id;                // 唯一标识 ID
        std::string name;      // 食材名称，如 "苹果"
        std::string category;  // 食材分类，如 "水果"
        int total_quantity;    // 所有批次数量总和
        std::vector<IngredientBatch> batches; // 按照存入时间升序排列的批次列表
    };

    /**
     * @brief 初始化本地数据库
     */
    bool init_database();

    /**
     * @brief 存入/增加食材（创建新批次）
     */
    bool add_ingredient(const std::string& name, const std::string& category, int quantity, int expire_days);

    /**
     * @brief 取出/减少食材（按 FIFO 优先扣减最早批次）
     */
    bool remove_ingredient(const std::string& name, int quantity);

    /**
     * @brief 删除某食材的所有批次
     */
    bool clear_ingredient(const std::string& name);

    /**
     * @brief 修改食材（清空批次，覆盖为一个新批次）
     */
    bool update_ingredient(const std::string& name, int quantity, const std::string& category, int expire_days, time_t entry_time);

    /**
     * @brief 获取当前所有食材列表
     */
    std::vector<IngredientType> get_all_ingredients();

    /**
     * @brief 获取 SD 卡是否可用
     */
    bool is_sd_card_available();

    /**
     * @brief 检查即将过期（或已过期）的食材
     */
    std::vector<IngredientType> check_expiring_ingredients(int warning_days_threshold = 3);

} // namespace inventory
} // namespace smart_fridge
