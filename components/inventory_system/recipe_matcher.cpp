#include "recipe_matcher.hpp"
#include "inventory.hpp"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "RecipeMatcher";
static const char *RECIPES_PATH = "/sdcard/recipes.json";

namespace smart_fridge {
namespace inventory {

// 内存中的食谱缓存
static std::vector<Recipe> recipe_cache;
static bool recipe_initialized = false;
static SemaphoreHandle_t recipe_mutex = NULL;  // 保护 recipe_cache 的多线程并发访问

// ======== 内置默认食谱 ========

/**
 * @brief 写入内置的默认食谱集到 SD 卡
 * 首次运行时自动创建，用户后续可以手动编辑 SD 卡上的 recipes.json 来增删食谱
 */
static void write_default_recipes() {
    recipe_cache.clear();

    // --- 1. 西红柿炒蛋 ---
    recipe_cache.push_back({
        "西红柿炒蛋", "家常菜",
        {{"西红柿", 2}, {"鸡蛋", 3}},
        "鸡蛋打散煎至金黄盛出，西红柿切块翻炒出汁，倒回鸡蛋翻匀，加盐和少许糖调味。"
    });

    // --- 2. 蛋炒饭 ---
    recipe_cache.push_back({
        "蛋炒饭", "主食",
        {{"鸡蛋", 2}, {"米饭", 1}},
        "隔夜米饭打散，鸡蛋炒碎，下饭翻炒至粒粒分明，加葱花和盐调味。"
    });

    // --- 3. 可乐鸡翅 ---
    recipe_cache.push_back({
        "可乐鸡翅", "家常菜",
        {{"鸡翅", 6}, {"可乐", 1}},
        "鸡翅两面煎至金黄，倒入可乐没过鸡翅，加生抽老抽，大火收汁。"
    });

    // --- 4. 蛋花汤 ---
    recipe_cache.push_back({
        "蛋花汤", "汤类",
        {{"鸡蛋", 2}},
        "水烧开后加盐，将打散的蛋液缓缓倒入，搅出蛋花，撒葱花即可。"
    });

    // --- 5. 清炒时蔬 ---
    recipe_cache.push_back({
        "清炒时蔬", "素菜",
        {{"青菜", 1}},
        "青菜洗净沥水，热锅凉油加蒜片爆香，下青菜大火翻炒至断生，加盐调味。"
    });

    // --- 6. 红烧排骨 ---
    recipe_cache.push_back({
        "红烧排骨", "家常菜",
        {{"排骨", 1}},
        "排骨焯水去腥，炒糖色裹匀，加生抽老抽八角桂皮，加水没过炖40分钟收汁。"
    });

    // --- 7. 番茄牛腩 ---
    recipe_cache.push_back({
        "番茄牛腩", "家常菜",
        {{"牛腩", 1}, {"西红柿", 3}},
        "牛腩切块焯水，西红柿去皮切块，先炒番茄出汁，加牛腩炖煮1.5小时至软烂。"
    });

    // --- 8. 醋溜土豆丝 ---
    recipe_cache.push_back({
        "醋溜土豆丝", "素菜",
        {{"土豆", 2}},
        "土豆切细丝泡水去淀粉，热油爆香干辣椒，下土豆丝大火翻炒，淋醋和盐。"
    });

    // --- 9. 牛奶燕麦 ---
    recipe_cache.push_back({
        "牛奶燕麦", "早餐",
        {{"牛奶", 1}, {"燕麦", 1}},
        "牛奶倒入碗中微波加热，加入即食燕麦片搅拌均匀，可加水果丁点缀。"
    });

    // --- 10. 西兰花炒虾仁 ---
    recipe_cache.push_back({
        "西兰花炒虾仁", "家常菜",
        {{"西兰花", 1}, {"虾仁", 1}},
        "西兰花焯水，虾仁料酒腌制，先炒虾仁变色盛出，再炒西兰花，合炒调味。"
    });

    // --- 11. 青椒肉丝 ---
    recipe_cache.push_back({
        "青椒肉丝", "家常菜",
        {{"青椒", 2}, {"猪肉", 1}},
        "猪肉切丝加淀粉腌制，青椒切丝，先炒肉丝至变色，加青椒翻炒调味。"
    });

    // --- 12. 麻婆豆腐 ---
    recipe_cache.push_back({
        "麻婆豆腐", "家常菜",
        {{"豆腐", 1}, {"猪肉", 1}},
        "豆腐切丁焯水，猪肉末炒散加豆瓣酱炒出红油，加豆腐和水烧入味，勾芡。"
    });

    // --- 13. 紫菜蛋花汤 ---
    recipe_cache.push_back({
        "紫菜蛋花汤", "汤类",
        {{"紫菜", 1}, {"鸡蛋", 1}},
        "水烧开放入紫菜，加盐，缓缓倒入蛋液搅出蛋花，淋香油出锅。"
    });

    // --- 14. 凉拌黄瓜 ---
    recipe_cache.push_back({
        "凉拌黄瓜", "凉菜",
        {{"黄瓜", 2}},
        "黄瓜拍碎切段，加蒜末、生抽、醋、辣椒油、白糖拌匀，冷藏后食用更佳。"
    });

    // --- 15. 香蕉奶昔 ---
    recipe_cache.push_back({
        "香蕉奶昔", "饮品",
        {{"香蕉", 2}, {"牛奶", 1}},
        "香蕉掰成小段，加牛奶一起放入搅拌机打至顺滑，可加蜂蜜调味。"
    });

    ESP_LOGI(TAG, "Written %d default recipes.", recipe_cache.size());
}

// ======== JSON 序列化 / 反序列化 ========

/**
 * @brief 将食谱缓存写入 SD 卡
 */
static bool recipe_save_to_disk() {
    cJSON *root = cJSON_CreateArray();
    for (const auto& recipe : recipe_cache) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", recipe.name.c_str());
        cJSON_AddStringToObject(obj, "category", recipe.category.c_str());
        cJSON_AddStringToObject(obj, "brief", recipe.brief.c_str());

        cJSON *ingredients = cJSON_CreateArray();
        for (const auto& ing : recipe.ingredients) {
            cJSON *ing_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(ing_obj, "name", ing.name.c_str());
            cJSON_AddNumberToObject(ing_obj, "min_qty", ing.min_quantity);
            cJSON_AddItemToArray(ingredients, ing_obj);
        }
        cJSON_AddItemToObject(obj, "ingredients", ingredients);

        cJSON_AddItemToArray(root, obj);
    }

    // 格式化输出，方便用户手动编辑
    char *json_string = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_string) {
        ESP_LOGE(TAG, "Failed to serialize recipes JSON");
        return false;
    }

    FILE *f = fopen(RECIPES_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", RECIPES_PATH);
        free(json_string);
        return false;
    }

    fprintf(f, "%s", json_string);
    fclose(f);
    free(json_string);

    ESP_LOGI(TAG, "Recipes saved to disk. Total: %d", recipe_cache.size());
    return true;
}

/**
 * @brief 从 SD 卡加载食谱
 */
static bool recipe_load_from_disk() {
    FILE *f = fopen(RECIPES_PATH, "r");
    if (!f) {
        // 文件不存在，返回 false 触发写入默认食谱
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize == 0) {
        fclose(f);
        return false;
    }

    char *json_string = (char *)malloc(fsize + 1);
    if (!json_string) {
        ESP_LOGE(TAG, "Failed to allocate memory for recipes JSON");
        fclose(f);
        return false;
    }

    fread(json_string, 1, fsize, f);
    fclose(f);
    json_string[fsize] = '\0';

    cJSON *root = cJSON_Parse(json_string);
    free(json_string);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse recipes JSON");
        return false;
    }

    recipe_cache.clear();
    int size = cJSON_GetArraySize(root);
    for (int i = 0; i < size; i++) {
        cJSON *obj = cJSON_GetArrayItem(root, i);
        if (!obj) continue;

        Recipe recipe;
        cJSON *name = cJSON_GetObjectItem(obj, "name");
        cJSON *category = cJSON_GetObjectItem(obj, "category");
        cJSON *brief = cJSON_GetObjectItem(obj, "brief");
        cJSON *ingredients = cJSON_GetObjectItem(obj, "ingredients");

        recipe.name = name ? name->valuestring : "";
        recipe.category = category ? category->valuestring : "";
        recipe.brief = brief ? brief->valuestring : "";

        if (ingredients && cJSON_IsArray(ingredients)) {
            int ing_count = cJSON_GetArraySize(ingredients);
            for (int j = 0; j < ing_count; j++) {
                cJSON *ing_obj = cJSON_GetArrayItem(ingredients, j);
                if (!ing_obj) continue;

                RecipeIngredient ing;
                cJSON *ing_name = cJSON_GetObjectItem(ing_obj, "name");
                cJSON *min_qty = cJSON_GetObjectItem(ing_obj, "min_qty");
                ing.name = ing_name ? ing_name->valuestring : "";
                ing.min_quantity = min_qty ? min_qty->valueint : 1;
                recipe.ingredients.push_back(ing);
            }
        }

        recipe_cache.push_back(recipe);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d recipes from disk.", recipe_cache.size());
    return true;
}

// ======== 匹配算法核心 ========

/**
 * @brief 对单个食谱执行匹配，生成 RecipeMatch 结果
 * 遍历食谱所需食材，检查库存中是否存在同名且数量充足的食材
 */
static RecipeMatch match_single_recipe(const Recipe& recipe,
                                       const std::vector<IngredientType>& inventory) {
    RecipeMatch match;
    match.recipe = recipe;
    match.missing_count = 0;

    int total_required = recipe.ingredients.size();
    int satisfied = 0;

    for (const auto& req : recipe.ingredients) {
        bool found = false;
        for (const auto& inv_item : inventory) {
            // 精确匹配食材名称
            if (inv_item.name == req.name &&
                inv_item.total_quantity >= req.min_quantity) {
                found = true;
                break;
            }
        }

        if (found) {
            satisfied++;
        } else {
            match.missing_items.push_back(req.name);
            match.missing_count++;
        }
    }

    match.coverage = (total_required > 0) ? (float)satisfied / (float)total_required : 0.0f;
    return match;
}

// ======== 公共接口实现 ========

bool recipe_init() {
    if (recipe_initialized) return true;

    // 创建互斥锁
    recipe_mutex = xSemaphoreCreateMutex();
    if (recipe_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create recipe mutex");
        return false;
    }

    if (!recipe_load_from_disk()) {
        ESP_LOGI(TAG, "No recipes file found, writing defaults...");
        write_default_recipes();
        recipe_save_to_disk();
    }

    recipe_initialized = true;
    ESP_LOGI(TAG, "Recipe module initialized. Total recipes: %d", recipe_cache.size());
    return true;
}

std::vector<RecipeMatch> recipe_match_available() {
    // 先快照库存，避免在持有 recipe_mutex 时再去获取 inventory_mutex，防止 AB-BA 死锁
    auto inventory = get_all_ingredients();
    std::vector<RecipeMatch> results;

    xSemaphoreTake(recipe_mutex, portMAX_DELAY);
    for (const auto& recipe : recipe_cache) {
        RecipeMatch match = match_single_recipe(recipe, inventory);
        if (match.coverage >= 1.0f) {
            results.push_back(match);
        }
    }
    xSemaphoreGive(recipe_mutex);

    ESP_LOGI(TAG, "Found %d fully available recipes.", results.size());
    return results;
}

std::vector<RecipeMatch> recipe_match_near(int max_missing) {
    // 先快照库存，确保全局锁顺序为 inventory -> recipe，避免与 prompt_builder 等形成死锁
    auto inventory = get_all_ingredients();
    std::vector<RecipeMatch> results;

    xSemaphoreTake(recipe_mutex, portMAX_DELAY);
    for (const auto& recipe : recipe_cache) {
        RecipeMatch match = match_single_recipe(recipe, inventory);
        // 包含完全满足的 (missing=0) 和差 N 样以内的
        if (match.missing_count <= max_missing) {
            results.push_back(match);
        }
    }
    xSemaphoreGive(recipe_mutex);

    // 排序在锁外进行（results 是本地副本）
    std::sort(results.begin(), results.end(),
        [](const RecipeMatch& a, const RecipeMatch& b) {
            if (a.coverage != b.coverage) return a.coverage > b.coverage;
            return a.missing_count < b.missing_count;
        });

    ESP_LOGI(TAG, "Found %d near-match recipes (max_missing=%d).", results.size(), max_missing);
    return results;
}

std::vector<Recipe> recipe_get_all() {
    if (recipe_mutex) xSemaphoreTake(recipe_mutex, portMAX_DELAY);
    auto copy = recipe_cache;
    if (recipe_mutex) xSemaphoreGive(recipe_mutex);
    return copy;
}

bool recipe_add(const Recipe& recipe) {
    if (!recipe_initialized) return false;
    xSemaphoreTake(recipe_mutex, portMAX_DELAY);
    // Check if exists
    for (const auto& r : recipe_cache) {
        if (r.name == recipe.name) {
            xSemaphoreGive(recipe_mutex);
            return false; // Already exists
        }
    }
    recipe_cache.push_back(recipe);
    bool res = recipe_save_to_disk();
    xSemaphoreGive(recipe_mutex);
    return res;
}

bool recipe_remove(const std::string& name) {
    if (!recipe_initialized) return false;
    xSemaphoreTake(recipe_mutex, portMAX_DELAY);
    for (auto it = recipe_cache.begin(); it != recipe_cache.end(); ++it) {
        if (it->name == name) {
            recipe_cache.erase(it);
            bool res = recipe_save_to_disk();
            xSemaphoreGive(recipe_mutex);
            return res;
        }
    }
    xSemaphoreGive(recipe_mutex);
    return false;
}

bool recipe_update(const std::string& old_name, const Recipe& new_recipe) {
    if (!recipe_initialized) return false;
    xSemaphoreTake(recipe_mutex, portMAX_DELAY);
    for (auto& r : recipe_cache) {
        if (r.name == old_name) {
            r = new_recipe;
            bool res = recipe_save_to_disk();
            xSemaphoreGive(recipe_mutex);
            return res;
        }
    }
    xSemaphoreGive(recipe_mutex);
    return false;
}

} // namespace inventory
} // namespace smart_fridge
