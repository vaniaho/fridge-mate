#pragma once

#include <string>
#include <vector>

namespace smart_fridge {
namespace inventory {

    // ======== 食谱数据结构 ========

    /**
     * @brief 食谱中的单个食材需求
     */
    struct RecipeIngredient {
        std::string name;           // 食材名称 (需与库存中的名称一致)
        int min_quantity;           // 最低需要数量
    };

    /**
     * @brief 食谱定义
     */
    struct Recipe {
        std::string name;           // 菜名，如 "西红柿炒蛋"
        std::string category;       // 分类，如 "家常菜"、"汤类"
        std::vector<RecipeIngredient> ingredients;  // 所需食材列表
        std::string brief;          // 简要做法说明
    };

    /**
     * @brief 食谱匹配结果
     */
    struct RecipeMatch {
        Recipe recipe;              // 匹配到的食谱
        float coverage;             // 库存覆盖率 (0.0 ~ 1.0, 1.0 表示食材全齐)
        std::vector<std::string> missing_items;  // 缺少的食材名称列表
        int missing_count;          // 缺少的食材数量
    };

    // ======== 食谱匹配接口 ========

    /**
     * @brief 初始化食谱模块
     * 从 /sdcard/recipes.json 加载食谱，如果文件不存在则写入内置默认食谱
     * @return true 初始化成功
     */
    bool recipe_init();

    /**
     * @brief 匹配当前库存能完全满足的食谱 (coverage == 1.0)
     * @return 按菜名字母序排列的匹配结果列表
     */
    std::vector<RecipeMatch> recipe_match_available();

    /**
     * @brief 匹配 "差 N 样食材就能做" 的食谱
     * @param max_missing 最多允许缺少的食材数量 (例如 max_missing=2 表示差2样以内)
     * @return 按 coverage 降序、missing_count 升序排列的匹配结果列表
     */
    std::vector<RecipeMatch> recipe_match_near(int max_missing = 2);

    /**
     * @brief 获取所有预置食谱
     * @return 完整的食谱列表
     */
    std::vector<Recipe> recipe_get_all();

    /**
     * @brief 新增食谱
     */
    bool recipe_add(const Recipe& recipe);

    /**
     * @brief 删除食谱
     */
    bool recipe_remove(const std::string& name);

    /**
     * @brief 更新食谱
     */
    bool recipe_update(const std::string& old_name, const Recipe& new_recipe);

} // namespace inventory
} // namespace smart_fridge
