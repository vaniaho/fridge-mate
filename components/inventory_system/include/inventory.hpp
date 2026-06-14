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
     * @brief 食材记录结构体
     */
    struct IngredientItem {
        int id;                // 唯一标识 ID
        std::string name;      // 食材名称，如 "苹果"
        std::string category;  // 食材分类，如 "水果"
        int quantity;          // 数量 (可根据分类决定是 个数 还是 重量克数)
        time_t entry_time;     // 最近一次存入的时间戳
        int expire_days;       // 保质期天数
        time_t expire_time;    // 预计过期时间戳
    };

    /**
     * @brief 初始化本地数据库
     * 内部会自动调用 sd_card_init()，然后加载本地 JSON/SQLite 到内存缓存中
     * @return true 初始化成功，false 失败
     */
    bool init_database();

    /**
     * @brief 存入/增加食材
     * 如果已存在同名食材，则累加数量，并重置其保质期
     * @param name 食材名称
     * @param category 分类
     * @param quantity 存入的数量
     * @param expire_days 预估保质期(天)
     * @return true 操作成功并持久化
     */
    bool add_ingredient(const std::string& name, const std::string& category, int quantity, int expire_days);

    /**
     * @brief 取出/减少食材
     * 如果剩余数量小于等于0，则将该食材从库中完全移除
     * @param name 食材名称
     * @param quantity 取出数量
     * @return true 操作成功并持久化
     */
    bool remove_ingredient(const std::string& name, int quantity);

    /**
     * @brief 直接修改食材信息 (数量、分类、保质期、存入时间)
     * 若 new_quantity <= 0，则直接删除该食材
     */
    bool update_ingredient(const std::string& name, int new_quantity, const std::string& new_category, int new_expire_days, time_t new_entry_time);

    /**
     * @brief 获取当前所有食材列表 (用于手机 App 或 本地屏幕显示)
     */
    std::vector<IngredientItem> get_all_ingredients();

    /**
     * @brief 检查即将过期（或已过期）的食材
     * @param warning_days_threshold 临期警告阈值（比如剩余 <= 3天）
     * @return 临期食材列表
     */
    std::vector<IngredientItem> check_expiring_ingredients(int warning_days_threshold = 3);

} // namespace inventory
} // namespace smart_fridge
