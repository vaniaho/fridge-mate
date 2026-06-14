#include "inventory.hpp"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "InventoryMgr";
static const char *DB_PATH = "/sdcard/inventory.json";

namespace smart_fridge {
namespace inventory {

// 内存中的食材列表缓存，避免频繁读写 SD 卡
static std::vector<IngredientItem> inventory_cache;
static bool is_initialized = false;
static bool sd_card_available = false; // SD 卡是否可用，不可用时回退到纯内存模式
static SemaphoreHandle_t inventory_mutex = NULL; // 保护 inventory_cache 的多线程并发访问
static int next_id = 0;  // 自增 ID 计数器，避免删除后 ID 碰撞

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
        cJSON_AddNumberToObject(obj, "quantity", item.quantity);
        cJSON_AddNumberToObject(obj, "entry_time", item.entry_time);
        cJSON_AddNumberToObject(obj, "expire_days", item.expire_days);
        cJSON_AddNumberToObject(obj, "expire_time", item.expire_time);
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

        IngredientItem item;
        item.id = cJSON_GetObjectItem(obj, "id") ? cJSON_GetObjectItem(obj, "id")->valueint : i;
        item.name = cJSON_GetObjectItem(obj, "name") ? cJSON_GetObjectItem(obj, "name")->valuestring : "";
        item.category = cJSON_GetObjectItem(obj, "category") ? cJSON_GetObjectItem(obj, "category")->valuestring : "";
        item.quantity = cJSON_GetObjectItem(obj, "quantity") ? cJSON_GetObjectItem(obj, "quantity")->valueint : 0;
        item.entry_time = cJSON_GetObjectItem(obj, "entry_time") ? (time_t)cJSON_GetObjectItem(obj, "entry_time")->valuedouble : 0;
        item.expire_days = cJSON_GetObjectItem(obj, "expire_days") ? cJSON_GetObjectItem(obj, "expire_days")->valueint : 0;
        item.expire_time = cJSON_GetObjectItem(obj, "expire_time") ? (time_t)cJSON_GetObjectItem(obj, "expire_time")->valuedouble : 0;
        
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
        ESP_LOGW(TAG, "Please check: 1) SD card inserted? 2) Card contacts clean? 3) GPIO45 power?");
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

    // 计算下一个可用 ID（避免删除后 ID 碰撞）
    for (const auto& item : inventory_cache) {
        if (item.id >= next_id) {
            next_id = item.id + 1;
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

    // 检查缓存中是否已存在同名食材
    for (auto& item : inventory_cache) {
        if (item.name == name) {
            item.quantity += quantity;
            // 覆盖原有存入时间和过期时间（默认新老食材合并，按最新保质期算）
            item.entry_time = now;
            item.expire_days = expire_days;
            item.expire_time = now + (expire_days * 24 * 3600);
            ESP_LOGI(TAG, "Updated existing ingredient: %s, new qty: %d", name.c_str(), item.quantity);
            history_append(HistoryAction::ADD, name, quantity);
            bool result = save_to_disk();
            xSemaphoreGive(inventory_mutex);
            return result;
        }
    }

    // 缓存中不存在，创建新记录
    IngredientItem new_item;
    new_item.id = next_id++;  // 使用自增计数器，避免删除后 ID 碰撞
    new_item.name = name;
    new_item.category = category;
    new_item.quantity = quantity;
    new_item.entry_time = now;
    new_item.expire_days = expire_days;
    // 计算绝对过期时间戳
    new_item.expire_time = now + (expire_days * 24 * 3600);

    inventory_cache.push_back(new_item);
    ESP_LOGI(TAG, "Added new ingredient: %s (Qty: %d)", name.c_str(), quantity);
    history_append(HistoryAction::ADD, name, quantity);
    
    bool result = save_to_disk();
    xSemaphoreGive(inventory_mutex);
    return result;
}

bool remove_ingredient(const std::string& name, int quantity) {
    if (!is_initialized) return false;

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);

    for (auto it = inventory_cache.begin(); it != inventory_cache.end(); ++it) {
        if (it->name == name) {
            int actual_removed = quantity;
            if (it->quantity > quantity) {
                // 如果只取出一部分
                it->quantity -= quantity;
                ESP_LOGI(TAG, "Decreased %s quantity to %d", name.c_str(), it->quantity);
            } else {
                // 如果取出的数量大于等于库存，直接删除该食材记录
                actual_removed = it->quantity; // 实际取出量 = 库存量
                ESP_LOGI(TAG, "Removed %s from inventory completely", name.c_str());
                inventory_cache.erase(it);
            }
            history_append(HistoryAction::REMOVE, name, actual_removed);
            bool result = save_to_disk();
            xSemaphoreGive(inventory_mutex);
            return result;
        }
    }
    
    xSemaphoreGive(inventory_mutex);
    ESP_LOGW(TAG, "Ingredient '%s' not found, nothing removed.", name.c_str());
    return false;
}

bool update_ingredient(const std::string& name, int new_quantity, const std::string& new_category, int new_expire_days, time_t new_entry_time) {
    if (!is_initialized) return false;
    
    // 如果数量 <= 0，等同于全部取出
    if (new_quantity <= 0) {
        return remove_ingredient(name, 999999);
    }

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);

    for (auto& item : inventory_cache) {
        if (item.name == name) {
            // 计算数量变化，补充历史记录
            if (new_quantity > item.quantity) {
                history_append(HistoryAction::ADD, name, new_quantity - item.quantity);
            } else if (new_quantity < item.quantity) {
                history_append(HistoryAction::REMOVE, name, item.quantity - new_quantity);
            }
            
            item.quantity = new_quantity;
            if (!new_category.empty()) {
                item.category = new_category;
            }
            item.expire_days = new_expire_days;
            item.entry_time = new_entry_time;
            item.expire_time = new_entry_time + (new_expire_days * 24 * 3600);
            
            ESP_LOGI(TAG, "Updated %s info: qty=%d, cat=%s, expire_days=%d", name.c_str(), new_quantity, item.category.c_str(), new_expire_days);
            
            bool result = save_to_disk();
            xSemaphoreGive(inventory_mutex);
            return result;
        }
    }

    xSemaphoreGive(inventory_mutex);
    ESP_LOGW(TAG, "Update failed: Ingredient '%s' not found.", name.c_str());
    return false;
}

std::vector<IngredientItem> get_all_ingredients() {
    // 允许在 init 之前调用（返回空列表），故需判断 mutex 是否已创建
    if (inventory_mutex) xSemaphoreTake(inventory_mutex, portMAX_DELAY);
    auto copy = inventory_cache;
    if (inventory_mutex) xSemaphoreGive(inventory_mutex);
    return copy;
}

std::vector<IngredientItem> check_expiring_ingredients(int warning_days_threshold) {
    std::vector<IngredientItem> expiring_items;
    if (!is_initialized) return expiring_items;

    xSemaphoreTake(inventory_mutex, portMAX_DELAY);

    time_t now;
    time(&now);
    
    // 将阈值天数转换为秒
    double warning_seconds = warning_days_threshold * 24 * 3600.0;

    for (const auto& item : inventory_cache) {
        double seconds_left = difftime(item.expire_time, now);
        
        // 如果剩余时间小于警告阈值（包含已经过期的负数）
        if (seconds_left <= warning_seconds) {
            expiring_items.push_back(item);
            
            if (seconds_left < 0) {
                ESP_LOGW(TAG, "[EXPIRED] %s has expired!", item.name.c_str());
            } else {
                ESP_LOGW(TAG, "[EXPIRING] %s expires in %.1f days", 
                         item.name.c_str(), seconds_left / (24.0 * 3600.0));
            }
        }
    }
    
    xSemaphoreGive(inventory_mutex);
    return expiring_items;
}

} // namespace inventory
} // namespace smart_fridge
