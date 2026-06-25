#include "inventory.hpp"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cctype>
#include <utility>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "InventoryMgr";
static const char *DB_PATH = "/sdcard/inventory.json";

namespace smart_fridge {
namespace inventory {

// 内存中的食材列表缓存，避免频繁读写 SD 卡
static std::vector<IngredientType> inventory_cache;
static bool is_initialized = false;
static bool sd_card_available = false; // SD 卡是否可用，不可用时回退到纯内存模式
static SemaphoreHandle_t inventory_mutex = NULL; // 保护 inventory_cache 的多线程并发访问
static int next_id = 0;  // 自增 ID 计数器，避免删除后 ID 碰撞
static int next_batch_id = 0;

static constexpr int MAX_NAME_BYTES = 96;
static constexpr int MAX_CATEGORY_BYTES = 48;
static constexpr int MAX_WRITE_QUANTITY = 9999;
static constexpr int MAX_EXPIRE_DAYS = 3650;

static bool is_blank_string(const std::string& value) {
    if (value.empty()) return true;
    for (unsigned char ch : value) {
        if (!std::isspace(ch)) return false;
    }
    return true;
}

static InventoryResult make_error(InventoryError error) {
    InventoryResult result;
    result.error = error;
    return result;
}

static auto find_item_by_name(const std::string& name) {
    return std::find_if(inventory_cache.begin(), inventory_cache.end(),
                        [&name](const IngredientType& item) {
                            return item.name == name;
                        });
}

/**
 * @brief 将当前缓存中的食材列表序列化为 JSON 并写入 SD 卡
 */
static bool save_to_disk() {
    if (!sd_card_available) {
        // 纯内存模式：跳过写盘，数据仅存在于内存中
        ESP_LOGD(TAG, "Memory-only mode: skipping disk write");
        return true;
    }
    cJSON *root = cJSON_CreateArray();
    for (const auto& item : inventory_cache) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", item.id);
        cJSON_AddStringToObject(obj, "name", item.name.c_str());
        cJSON_AddStringToObject(obj, "category", item.category.c_str());
        cJSON_AddNumberToObject(obj, "total_quantity", item.total_quantity);

        cJSON *batches_arr = cJSON_CreateArray();
        for (const auto& batch : item.batches) {
            cJSON *b_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(b_obj, "batch_id", batch.batch_id);
            cJSON_AddNumberToObject(b_obj, "quantity", batch.quantity);
            cJSON_AddNumberToObject(b_obj, "entry_time", batch.entry_time);
            cJSON_AddNumberToObject(b_obj, "expire_days", batch.expire_days);
            cJSON_AddNumberToObject(b_obj, "expire_time", batch.expire_time);
            cJSON_AddItemToArray(batches_arr, b_obj);
        }
        cJSON_AddItemToObject(obj, "batches", batches_arr);

        cJSON_AddItemToArray(root, obj);
    }

    // 转换为未格式化的紧凑字符串，节省存储空间
    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return false;
    }

    // 覆盖写入 SD 卡文件
    FILE *f = fopen(DB_PATH, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", DB_PATH);
        free(json_string);
        return false;
    }
    
    fprintf(f, "%s", json_string);
    fclose(f);
    free(json_string);
    
    ESP_LOGI(TAG, "Inventory saved to disk. Total items: %d", inventory_cache.size());
    return true;
}

/**
 * @brief 从 SD 卡读取 JSON 文件并反序列化到缓存中
 */
static bool load_from_disk() {
    FILE *f = fopen(DB_PATH, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "No existing database found (%s), starting fresh.", DB_PATH);
        return true; // 文件不存在视为初次运行，这是正常的
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize == 0) {
        fclose(f);
        return true;
    }

    char *json_string = (char *)malloc(fsize + 1);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for reading JSON");
        fclose(f);
        return false;
    }

    fread(json_string, 1, fsize, f);
    fclose(f);
    json_string[fsize] = '\0';

    cJSON *root = cJSON_Parse(json_string);
    free(json_string);

    if (root == NULL) {
        ESP_LOGE(TAG, "Error parsing JSON from disk.");
        return false;
    }

    inventory_cache.clear();
    int size = cJSON_GetArraySize(root);
    for (int i = 0; i < size; i++) {
        cJSON *obj = cJSON_GetArrayItem(root, i);
        if (!obj) continue;

        IngredientType item;
        item.id = cJSON_GetObjectItem(obj, "id") ? cJSON_GetObjectItem(obj, "id")->valueint : i;
        item.name = cJSON_GetObjectItem(obj, "name") ? cJSON_GetObjectItem(obj, "name")->valuestring : "";
        item.category = cJSON_GetObjectItem(obj, "category") ? cJSON_GetObjectItem(obj, "category")->valuestring : "";
        item.total_quantity = cJSON_GetObjectItem(obj, "total_quantity") ? cJSON_GetObjectItem(obj, "total_quantity")->valueint : 0;
        
        cJSON *batches_arr = cJSON_GetObjectItem(obj, "batches");
        if (batches_arr) {
            int b_size = cJSON_GetArraySize(batches_arr);
            for (int j = 0; j < b_size; j++) {
                cJSON *b_obj = cJSON_GetArrayItem(batches_arr, j);
                if (!b_obj) continue;
                IngredientBatch b;
                b.batch_id = cJSON_GetObjectItem(b_obj, "batch_id") ? cJSON_GetObjectItem(b_obj, "batch_id")->valueint : j;
                b.quantity = cJSON_GetObjectItem(b_obj, "quantity") ? cJSON_GetObjectItem(b_obj, "quantity")->valueint : 0;
                b.entry_time = cJSON_GetObjectItem(b_obj, "entry_time") ? (time_t)cJSON_GetObjectItem(b_obj, "entry_time")->valuedouble : 0;
                b.expire_days = cJSON_GetObjectItem(b_obj, "expire_days") ? cJSON_GetObjectItem(b_obj, "expire_days")->valueint : 0;
                b.expire_time = cJSON_GetObjectItem(b_obj, "expire_time") ? (time_t)cJSON_GetObjectItem(b_obj, "expire_time")->valuedouble : 0;
                item.batches.push_back(b);
            }
        } else {
            // 旧版数据兼容：尝试把旧的 quantity 等转化为第一个批次
            if (cJSON_GetObjectItem(obj, "quantity")) {
                IngredientBatch b;
                b.batch_id = 0;
                b.quantity = cJSON_GetObjectItem(obj, "quantity")->valueint;
                b.entry_time = cJSON_GetObjectItem(obj, "entry_time") ? (time_t)cJSON_GetObjectItem(obj, "entry_time")->valuedouble : 0;
                b.expire_days = cJSON_GetObjectItem(obj, "expire_days") ? cJSON_GetObjectItem(obj, "expire_days")->valueint : 0;
                b.expire_time = cJSON_GetObjectItem(obj, "expire_time") ? (time_t)cJSON_GetObjectItem(obj, "expire_time")->valuedouble : 0;
                item.batches.push_back(b);
                item.total_quantity = b.quantity;
            }
        }
        
        inventory_cache.push_back(item);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d items from disk.", inventory_cache.size());
    return true;
}

bool init_database() {
    if (is_initialized) return true;

    // 0. 创建互斥锁，保护多线程并发访问
    inventory_mutex = xSemaphoreCreateMutex();
    if (inventory_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create inventory mutex");
        return false;
    }

    // 1. 尝试初始化 SD 卡硬件
    if (sd_card_init() != 0) {
        ESP_LOGW(TAG, "SD Card initialization failed! Falling back to memory-only mode.");
        ESP_LOGW(TAG, "Inventory data will NOT be persisted across reboots.");
        sd_card_available = false;
    } else {
        sd_card_available = true;
        // 2. 从 SD 卡读取过往数据
        if (!load_from_disk()) {
            ESP_LOGE(TAG, "Failed to load database from SD card.");
            return false;
        }
    }
    
    is_initialized = true;

    // 计算下一个可用 ID 和 batch ID（避免删除后 ID 碰撞）
    for (const auto& item : inventory_cache) {
        if (item.id >= next_id) {
            next_id = item.id + 1;
        }
        for (const auto& b : item.batches) {
            if (b.batch_id >= next_batch_id) {
                next_batch_id = b.batch_id + 1;
            }
        }
    }

    // 3. 初始化历史记录模块
    if (!history_init()) {
        ESP_LOGW(TAG, "History module init failed, history recording will be disabled.");
    }

    // 4. 初始化食谱模块
    if (!recipe_init()) {
        ESP_LOGW(TAG, "Recipe module init failed, recipe matching will be disabled.");
    }

    ESP_LOGI(TAG, "Inventory Database Initialized Successfully. (Storage: %s)",
             sd_card_available ? "SD Card" : "Memory-only");
    return true;
}

InventoryResult validate_add_ingredient(const std::string& name, const std::string& category,
                                        int quantity, int expire_days) {
    if (!is_initialized) return make_error(InventoryError::NOT_INITIALIZED);
    if (is_blank_string(name) || name.size() > MAX_NAME_BYTES) {
        return make_error(InventoryError::INVALID_NAME);
    }
    if (is_blank_string(category) || category.size() > MAX_CATEGORY_BYTES) {
        return make_error(InventoryError::INVALID_CATEGORY);
    }
    if (quantity <= 0 || quantity > MAX_WRITE_QUANTITY) {
        return make_error(InventoryError::INVALID_QUANTITY);
    }
    if (expire_days <= 0 || expire_days > MAX_EXPIRE_DAYS) {
        return make_error(InventoryError::INVALID_EXPIRE_DAYS);
    }

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);
    auto it = find_item_by_name(name);
    if (it != inventory_cache.end() &&
        it->total_quantity > MAX_WRITE_QUANTITY - quantity) {
        xSemaphoreGive(inventory_mutex);
        return make_error(InventoryError::INVALID_QUANTITY);
    }
    xSemaphoreGive(inventory_mutex);
    return {};
}

InventoryResult add_ingredient_checked(const std::string& name, const std::string& category,
                                       int quantity, int expire_days) {
    InventoryResult validation = validate_add_ingredient(name, category, quantity, expire_days);
    if (!validation.ok()) return validation;

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);
    auto current_item = find_item_by_name(name);
    if (current_item != inventory_cache.end() &&
        current_item->total_quantity > MAX_WRITE_QUANTITY - quantity) {
        xSemaphoreGive(inventory_mutex);
        return make_error(InventoryError::INVALID_QUANTITY);
    }
    auto old_cache = inventory_cache;
    int old_next_id = next_id;
    int old_next_batch_id = next_batch_id;
    // 获取当前时间戳（这依赖于 rtc_time.c 模块已经同步了 NTP 或 RTC）
    time_t now;
    time(&now);

    IngredientBatch new_batch;
    new_batch.batch_id = next_batch_id++;
    new_batch.quantity = quantity;
    new_batch.entry_time = now;
    new_batch.expire_days = expire_days;
    new_batch.expire_time = now + (expire_days * 24 * 3600);

    bool found = false;
    // 检查缓存中是否已存在同名食材
    for (auto& item : inventory_cache) {
        if (item.name == name) {
            bool merged = false;
            for (auto& b : item.batches) {
                struct tm tm_b, tm_n;
                localtime_r(&b.entry_time, &tm_b);
                localtime_r(&now, &tm_n);
                if (tm_b.tm_year == tm_n.tm_year && tm_b.tm_yday == tm_n.tm_yday) {
                    b.quantity += quantity;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                item.batches.push_back(new_batch);
            }
            item.total_quantity += quantity;
            found = true;
            break;
        }
    }

    if (!found) {
        IngredientType new_item;
        new_item.id = next_id++;
        new_item.name = name;
        new_item.category = category;
        new_item.total_quantity = quantity;
        new_item.batches.push_back(new_batch);
        inventory_cache.push_back(new_item);
    }

    ESP_LOGI(TAG, "Added new ingredient batch: %s (Qty: %d)", name.c_str(), quantity);
    if (!save_to_disk()) {
        inventory_cache = std::move(old_cache);
        next_id = old_next_id;
        next_batch_id = old_next_batch_id;
        xSemaphoreGive(inventory_mutex);
        return make_error(InventoryError::STORAGE_ERROR);
    }

    auto saved_item = find_item_by_name(name);
    InventoryResult result;
    result.affected_quantity = quantity;
    result.remaining_quantity = saved_item != inventory_cache.end() ? saved_item->total_quantity : 0;
    xSemaphoreGive(inventory_mutex);

    history_append(HistoryAction::ADD, name, quantity);
    return result;
}

bool add_ingredient(const std::string& name, const std::string& category,
                    int quantity, int expire_days) {
    return add_ingredient_checked(name, category, quantity, expire_days).ok();
}

InventoryResult validate_remove_ingredient(const std::string& name, int quantity) {
    if (!is_initialized) return make_error(InventoryError::NOT_INITIALIZED);
    if (is_blank_string(name) || name.size() > MAX_NAME_BYTES) {
        return make_error(InventoryError::INVALID_NAME);
    }
    if (quantity <= 0 || quantity > MAX_WRITE_QUANTITY) {
        return make_error(InventoryError::INVALID_QUANTITY);
    }

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);
    auto it = find_item_by_name(name);
    if (it == inventory_cache.end()) {
        xSemaphoreGive(inventory_mutex);
        return make_error(InventoryError::NOT_FOUND);
    }
    if (quantity > it->total_quantity) {
        InventoryResult result = make_error(InventoryError::INSUFFICIENT_QUANTITY);
        result.remaining_quantity = it->total_quantity;
        xSemaphoreGive(inventory_mutex);
        return result;
    }
    xSemaphoreGive(inventory_mutex);
    return {};
}

InventoryResult remove_ingredient_checked(const std::string& name, int quantity) {
    InventoryResult validation = validate_remove_ingredient(name, quantity);
    if (!validation.ok()) return validation;

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);
    auto current_item = find_item_by_name(name);
    if (current_item == inventory_cache.end()) {
        xSemaphoreGive(inventory_mutex);
        return make_error(InventoryError::NOT_FOUND);
    }
    if (quantity > current_item->total_quantity) {
        InventoryResult result = make_error(InventoryError::INSUFFICIENT_QUANTITY);
        result.remaining_quantity = current_item->total_quantity;
        xSemaphoreGive(inventory_mutex);
        return result;
    }
    auto old_cache = inventory_cache;

    for (auto it = inventory_cache.begin(); it != inventory_cache.end(); ++it) {
        if (it->name == name) {
            int remaining_to_remove = quantity;
            int actual_removed = 0;
            
            // 按照存入时间扣减（假设 batches 已经是按照加入顺序排列的，即 entry_time 升序）
            auto batch_it = it->batches.begin();
            while (batch_it != it->batches.end() && remaining_to_remove > 0) {
                if (batch_it->quantity <= remaining_to_remove) {
                    remaining_to_remove -= batch_it->quantity;
                    actual_removed += batch_it->quantity;
                    batch_it = it->batches.erase(batch_it);
                } else {
                    batch_it->quantity -= remaining_to_remove;
                    actual_removed += remaining_to_remove;
                    remaining_to_remove = 0;
                    ++batch_it;
                }
            }
            
            it->total_quantity -= actual_removed;
            ESP_LOGI(TAG, "Removed %s quantity %d, remaining total: %d", name.c_str(), actual_removed, it->total_quantity);
            int remaining_quantity = it->total_quantity;

            if (it->total_quantity <= 0 || it->batches.empty()) {
                inventory_cache.erase(it);
                ESP_LOGI(TAG, "Removed %s from inventory completely", name.c_str());
                remaining_quantity = 0;
            }

            if (!save_to_disk()) {
                inventory_cache = std::move(old_cache);
                xSemaphoreGive(inventory_mutex);
                return make_error(InventoryError::STORAGE_ERROR);
            }

            xSemaphoreGive(inventory_mutex);
            history_append(HistoryAction::REMOVE, name, actual_removed);

            InventoryResult result;
            result.affected_quantity = actual_removed;
            result.remaining_quantity = remaining_quantity;
            return result;
        }
    }
    
    xSemaphoreGive(inventory_mutex);
    ESP_LOGW(TAG, "Ingredient '%s' not found, nothing removed.", name.c_str());
    return make_error(InventoryError::NOT_FOUND);
}

bool remove_ingredient(const std::string& name, int quantity) {
    return remove_ingredient_checked(name, quantity).ok();
}

InventoryResult clear_ingredient_checked(const std::string& name) {
    if (!is_initialized) return make_error(InventoryError::NOT_INITIALIZED);
    if (is_blank_string(name) || name.size() > MAX_NAME_BYTES) {
        return make_error(InventoryError::INVALID_NAME);
    }

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);
    auto old_cache = inventory_cache;

    for (auto it = inventory_cache.begin(); it != inventory_cache.end(); ++it) {
        if (it->name == name) {
            int removed_quantity = it->total_quantity;
            inventory_cache.erase(it);
            if (!save_to_disk()) {
                inventory_cache = std::move(old_cache);
                xSemaphoreGive(inventory_mutex);
                return make_error(InventoryError::STORAGE_ERROR);
            }
            xSemaphoreGive(inventory_mutex);

            history_append(HistoryAction::REMOVE, name, removed_quantity);
            InventoryResult result;
            result.affected_quantity = removed_quantity;
            return result;
        }
    }
    xSemaphoreGive(inventory_mutex);
    return make_error(InventoryError::NOT_FOUND);
}

bool clear_ingredient(const std::string& name) {
    return clear_ingredient_checked(name).ok();
}

InventoryResult update_ingredient_checked(const std::string& name, int quantity,
                                          const std::string& category, int expire_days,
                                          time_t entry_time) {
    if (!is_initialized) return make_error(InventoryError::NOT_INITIALIZED);
    if (is_blank_string(name) || name.size() > MAX_NAME_BYTES) {
        return make_error(InventoryError::INVALID_NAME);
    }
    if (is_blank_string(category) || category.size() > MAX_CATEGORY_BYTES) {
        return make_error(InventoryError::INVALID_CATEGORY);
    }
    if (quantity <= 0 || quantity > MAX_WRITE_QUANTITY) {
        return make_error(InventoryError::INVALID_QUANTITY);
    }
    if (expire_days <= 0 || expire_days > MAX_EXPIRE_DAYS) {
        return make_error(InventoryError::INVALID_EXPIRE_DAYS);
    }
    if (entry_time <= 0) return make_error(InventoryError::INVALID_ENTRY_TIME);

    time_t now;
    time(&now);
    if (now > 1700000000 && entry_time > now + 24 * 3600) {
        return make_error(InventoryError::INVALID_ENTRY_TIME);
    }

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);
    auto old_cache = inventory_cache;
    int old_next_batch_id = next_batch_id;

    for (auto& item : inventory_cache) {
        if (item.name == name) {
            item.category = category;
            item.total_quantity = quantity;
            item.batches.clear();
            
            IngredientBatch new_batch;
            new_batch.batch_id = next_batch_id++;
            new_batch.quantity = quantity;
            new_batch.entry_time = entry_time;
            new_batch.expire_days = expire_days;
            new_batch.expire_time = entry_time + (expire_days * 24 * 3600);
            
            item.batches.push_back(new_batch);

            if (!save_to_disk()) {
                inventory_cache = std::move(old_cache);
                next_batch_id = old_next_batch_id;
                xSemaphoreGive(inventory_mutex);
                return make_error(InventoryError::STORAGE_ERROR);
            }
            xSemaphoreGive(inventory_mutex);

            InventoryResult result;
            result.affected_quantity = quantity;
            result.remaining_quantity = quantity;
            return result;
        }
    }
    
    xSemaphoreGive(inventory_mutex);
    return make_error(InventoryError::NOT_FOUND);
}

bool update_ingredient(const std::string& name, int quantity, const std::string& category,
                       int expire_days, time_t entry_time) {
    return update_ingredient_checked(name, quantity, category, expire_days, entry_time).ok();
}

const char* inventory_error_message(InventoryError error) {
    switch (error) {
        case InventoryError::OK: return "操作成功";
        case InventoryError::NOT_INITIALIZED: return "库存系统尚未初始化";
        case InventoryError::INVALID_NAME: return "食材名称不能为空或过长";
        case InventoryError::INVALID_CATEGORY: return "食材分类不能为空或过长";
        case InventoryError::INVALID_QUANTITY: return "数量必须在 1 到 9999 之间";
        case InventoryError::INVALID_EXPIRE_DAYS: return "保质期必须在 1 到 3650 天之间";
        case InventoryError::INVALID_ENTRY_TIME: return "存入时间无效，不能晚于当前时间一天以上";
        case InventoryError::NOT_FOUND: return "未找到该食材";
        case InventoryError::INSUFFICIENT_QUANTITY: return "库存数量不足";
        case InventoryError::STORAGE_ERROR: return "库存保存失败，操作已回滚";
        default: return "库存操作失败";
    }
}

std::vector<IngredientType> get_all_ingredients() {
    // 允许在 init 之前调用（返回空列表），故需判断 mutex 是否已创建
    if (inventory_mutex) xSemaphoreTake(inventory_mutex, portMAX_DELAY);
    auto copy = inventory_cache;
    if (inventory_mutex) xSemaphoreGive(inventory_mutex);
    return copy;
}

bool is_sd_card_available() {
    return sd_card_available;
}

std::vector<IngredientType> check_expiring_ingredients(int warning_days_threshold) {
    std::vector<IngredientType> expiring_items;
    if (!inventory_mutex) return expiring_items;

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);
    time_t now;
    time(&now);

    for (const auto& item : inventory_cache) {
        IngredientType expiring_item = item;
        expiring_item.batches.clear();
        expiring_item.total_quantity = 0;
        
        for (const auto& batch : item.batches) {
            double diff_seconds = difftime(batch.expire_time, now);
            int remaining_days = (int)(diff_seconds / (24 * 3600));
            if (remaining_days <= warning_days_threshold) {
                expiring_item.batches.push_back(batch);
                expiring_item.total_quantity += batch.quantity;
            }
        }
        
        if (!expiring_item.batches.empty()) {
            expiring_items.push_back(expiring_item);
        }
    }
    
    xSemaphoreGive(inventory_mutex);
    return expiring_items;
}

} // namespace inventory
} // namespace smart_fridge
