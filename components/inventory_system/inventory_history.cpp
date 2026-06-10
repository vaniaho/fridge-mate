#include "inventory_history.hpp"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <set>

static const char *TAG = "InvHistory";
static const char *HISTORY_PATH = "/sdcard/history.json";

// 历史记录最多保留 90 天
static const int MAX_RETENTION_DAYS = 90;

namespace smart_fridge {
namespace inventory {

// 内存中的历史记录缓存
static std::vector<HistoryRecord> history_cache;
static bool history_initialized = false;

// ======== 私有工具函数 ========

/**
 * @brief 将 HistoryAction 转换为字符串
 */
static const char* action_to_str(HistoryAction action) {
    return action == HistoryAction::ADD ? "ADD" : "REMOVE";
}

/**
 * @brief 将字符串转换为 HistoryAction
 */
static HistoryAction str_to_action(const char* str) {
    if (str && strcmp(str, "ADD") == 0) return HistoryAction::ADD;
    return HistoryAction::REMOVE;
}

/**
 * @brief 清理超过保留期的旧记录
 */
static void purge_old_records() {
    time_t now;
    time(&now);
    time_t cutoff = now - (MAX_RETENTION_DAYS * 24 * 3600);

    size_t before = history_cache.size();
    history_cache.erase(
        std::remove_if(history_cache.begin(), history_cache.end(),
            [cutoff](const HistoryRecord& r) { return r.timestamp < cutoff; }),
        history_cache.end()
    );

    size_t removed = before - history_cache.size();
    if (removed > 0) {
        ESP_LOGI(TAG, "Purged %d records older than %d days", removed, MAX_RETENTION_DAYS);
    }
}

/**
 * @brief 将历史记录序列化并写入 SD 卡
 */
static bool history_save_to_disk() {
    cJSON *root = cJSON_CreateArray();
    for (const auto& record : history_cache) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "ts", (double)record.timestamp);
        cJSON_AddStringToObject(obj, "act", action_to_str(record.action));
        cJSON_AddStringToObject(obj, "item", record.item_name.c_str());
        cJSON_AddNumberToObject(obj, "qty", record.quantity);
        cJSON_AddItemToArray(root, obj);
    }

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_string) {
        ESP_LOGE(TAG, "Failed to serialize history JSON");
        return false;
    }

    FILE *f = fopen(HISTORY_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", HISTORY_PATH);
        free(json_string);
        return false;
    }

    fprintf(f, "%s", json_string);
    fclose(f);
    free(json_string);

    ESP_LOGD(TAG, "History saved. Total records: %d", history_cache.size());
    return true;
}

/**
 * @brief 从 SD 卡加载历史记录
 */
static bool history_load_from_disk() {
    FILE *f = fopen(HISTORY_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No existing history file found, starting fresh.");
        return true;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize == 0) {
        fclose(f);
        return true;
    }

    char *json_string = (char *)malloc(fsize + 1);
    if (!json_string) {
        ESP_LOGE(TAG, "Failed to allocate memory for history JSON");
        fclose(f);
        return false;
    }

    fread(json_string, 1, fsize, f);
    fclose(f);
    json_string[fsize] = '\0';

    cJSON *root = cJSON_Parse(json_string);
    free(json_string);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse history JSON");
        return false;
    }

    history_cache.clear();
    int size = cJSON_GetArraySize(root);
    for (int i = 0; i < size; i++) {
        cJSON *obj = cJSON_GetArrayItem(root, i);
        if (!obj) continue;

        HistoryRecord record;
        cJSON *ts = cJSON_GetObjectItem(obj, "ts");
        cJSON *act = cJSON_GetObjectItem(obj, "act");
        cJSON *item = cJSON_GetObjectItem(obj, "item");
        cJSON *qty = cJSON_GetObjectItem(obj, "qty");

        record.timestamp = ts ? (time_t)ts->valuedouble : 0;
        record.action = act ? str_to_action(act->valuestring) : HistoryAction::ADD;
        record.item_name = item ? item->valuestring : "";
        record.quantity = qty ? qty->valueint : 0;

        history_cache.push_back(record);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d history records from disk.", history_cache.size());
    return true;
}

// ======== 公共接口实现 ========

bool history_init() {
    if (history_initialized) return true;

    if (!history_load_from_disk()) {
        ESP_LOGE(TAG, "Failed to load history from disk.");
        return false;
    }

    // 启动时清理过期记录
    purge_old_records();

    history_initialized = true;
    ESP_LOGI(TAG, "History module initialized. Records in memory: %d", history_cache.size());
    return true;
}

void history_append(HistoryAction action, const std::string& item_name, int quantity) {
    if (!history_initialized) {
        ESP_LOGW(TAG, "History not initialized, skipping append.");
        return;
    }

    HistoryRecord record;
    time(&record.timestamp);
    record.action = action;
    record.item_name = item_name;
    record.quantity = quantity;

    history_cache.push_back(record);

    ESP_LOGI(TAG, "History: %s %s x%d",
             action_to_str(action), item_name.c_str(), quantity);

    // 定期清理（每 100 条检查一次，避免每次都遍历）
    if (history_cache.size() % 100 == 0) {
        purge_old_records();
    }

    history_save_to_disk();
}

std::vector<HistoryRecord> history_get_recent(int days) {
    std::vector<HistoryRecord> result;

    time_t now;
    time(&now);
    time_t cutoff = (days > 0) ? now - (days * 24 * 3600) : 0;

    for (const auto& record : history_cache) {
        if (record.timestamp >= cutoff) {
            result.push_back(record);
        }
    }

    // 按时间倒序排列（最新的在前）
    std::sort(result.begin(), result.end(),
        [](const HistoryRecord& a, const HistoryRecord& b) {
            return a.timestamp > b.timestamp;
        });

    return result;
}

std::vector<HistoryRecord> history_get_by_item(const std::string& item_name, int days) {
    std::vector<HistoryRecord> result;

    time_t now;
    time(&now);
    time_t cutoff = (days > 0) ? now - (days * 24 * 3600) : 0;

    for (const auto& record : history_cache) {
        if (record.item_name == item_name && record.timestamp >= cutoff) {
            result.push_back(record);
        }
    }

    std::sort(result.begin(), result.end(),
        [](const HistoryRecord& a, const HistoryRecord& b) {
            return a.timestamp > b.timestamp;
        });

    return result;
}

ConsumptionRate compute_consumption_rate(const std::string& item_name, int window_days) {
    ConsumptionRate rate;
    rate.item_name = item_name;
    rate.window_days = window_days;
    rate.total_consumed = 0;
    rate.daily_rate = 0.0f;

    time_t now;
    time(&now);
    time_t cutoff = now - (window_days * 24 * 3600);

    // 统计窗口内该食材的所有 REMOVE 事件总量
    for (const auto& record : history_cache) {
        if (record.action == HistoryAction::REMOVE &&
            record.item_name == item_name &&
            record.timestamp >= cutoff) {
            rate.total_consumed += record.quantity;
        }
    }

    // 计算日均消耗量
    if (window_days > 0) {
        rate.daily_rate = (float)rate.total_consumed / (float)window_days;
    }

    return rate;
}

std::vector<ConsumptionRate> compute_all_consumption_rates(int window_days) {
    time_t now;
    time(&now);
    time_t cutoff = now - (window_days * 24 * 3600);

    // 1. 收集窗口内所有有 REMOVE 记录的食材名
    std::set<std::string> consumed_items;
    for (const auto& record : history_cache) {
        if (record.action == HistoryAction::REMOVE && record.timestamp >= cutoff) {
            consumed_items.insert(record.item_name);
        }
    }

    // 2. 逐个计算消耗速率
    std::vector<ConsumptionRate> rates;
    for (const auto& name : consumed_items) {
        rates.push_back(compute_consumption_rate(name, window_days));
    }

    // 3. 按 daily_rate 降序排列（消耗最快的排在前面）
    std::sort(rates.begin(), rates.end(),
        [](const ConsumptionRate& a, const ConsumptionRate& b) {
            return a.daily_rate > b.daily_rate;
        });

    return rates;
}

} // namespace inventory
} // namespace smart_fridge
