#include "inventory.hpp"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
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

bool add_ingredient(const std::string& name, const std::string& category, int quantity, int expire_days) {
    if (!is_initialized) return false;

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);

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
            item.batches.push_back(new_batch);
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
    history_append(HistoryAction::ADD, name, quantity);
    
    bool result = save_to_disk();
    xSemaphoreGive(inventory_mutex);
    return result;
}

bool remove_ingredient(const std::string& name, int quantity) {
    if (!is_initialized || quantity <= 0) return false;

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);

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
            history_append(HistoryAction::REMOVE, name, actual_removed);
            
            if (it->total_quantity <= 0 || it->batches.empty()) {
                inventory_cache.erase(it);
                ESP_LOGI(TAG, "Removed %s from inventory completely", name.c_str());
            }
            
            bool result = save_to_disk();
            xSemaphoreGive(inventory_mutex);
            return result;
        }
    }
    
    xSemaphoreGive(inventory_mutex);
    ESP_LOGW(TAG, "Ingredient '%s' not found, nothing removed.", name.c_str());
    return false;
}

bool clear_ingredient(const std::string& name) {
    if (!is_initialized) return false;
    xSemaphoreTake(inventory_mutex, portMAX_DELAY);

    for (auto it = inventory_cache.begin(); it != inventory_cache.end(); ++it) {
        if (it->name == name) {
            history_append(HistoryAction::REMOVE, name, it->total_quantity);
            inventory_cache.erase(it);
            bool result = save_to_disk();
            xSemaphoreGive(inventory_mutex);
            return result;
        }
    }
    xSemaphoreGive(inventory_mutex);
    return false;
}

bool update_ingredient(const std::string& name, int quantity, const std::string& category, int expire_days, time_t entry_time) {
    if (!is_initialized) return false;
    xSemaphoreTake(inventory_mutex, portMAX_DELAY);

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
            
            bool result = save_to_disk();
            xSemaphoreGive(inventory_mutex);
            return result;
        }
    }
    
    xSemaphoreGive(inventory_mutex);
    return false;
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
