#include "notes_board.hpp"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_storage.h"

static const char *TAG = "NotesBoard";
static const char *NOTES_PATH = "/sdcard/notes.json";

namespace smart_fridge {
namespace dashboard {

// 留言最多保留 10 条，超出按 FIFO 丢弃最早的
constexpr size_t MAX_NOTES = 10;

static std::vector<Note> notes_cache;
static bool is_initialized = false;
static bool sd_card_available = false;
static SemaphoreHandle_t notes_mutex = NULL;

/**
 * @brief 将缓存序列化为 JSON 写入 SD 卡
 */
static bool save_to_disk() {
    if (!sd_card_available) {
        ESP_LOGD(TAG, "Memory-only mode: skipping disk write");
        return true;
    }

    cJSON *root = cJSON_CreateArray();
    for (const auto& note : notes_cache) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "timestamp", (double)note.timestamp);
        cJSON_AddStringToObject(obj, "text", note.text.c_str());
        cJSON_AddItemToArray(root, obj);
    }

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_string) {
        ESP_LOGE(TAG, "Failed to serialize notes JSON");
        return false;
    }

    FILE *f = fopen(NOTES_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", NOTES_PATH);
        free(json_string);
        return false;
    }
    fprintf(f, "%s", json_string);
    fclose(f);
    free(json_string);

    ESP_LOGI(TAG, "Notes saved to disk. Count: %d", (int)notes_cache.size());
    return true;
}

/**
 * @brief 从 SD 卡读取 JSON 反序列化到缓存
 */
static bool load_from_disk() {
    FILE *f = fopen(NOTES_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No existing notes file (%s), starting fresh.", NOTES_PATH);
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
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate memory for notes JSON");
        return false;
    }
    fread(json_string, 1, fsize, f);
    fclose(f);
    json_string[fsize] = '\0';

    cJSON *root = cJSON_Parse(json_string);
    free(json_string);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse notes JSON");
        return false;
    }

    notes_cache.clear();
    int size = cJSON_GetArraySize(root);
    for (int i = 0; i < size; i++) {
        cJSON *obj = cJSON_GetArrayItem(root, i);
        if (!obj) continue;
        Note note;
        cJSON *ts = cJSON_GetObjectItem(obj, "timestamp");
        cJSON *tx = cJSON_GetObjectItem(obj, "text");
        note.timestamp = ts ? (time_t)ts->valuedouble : 0;
        note.text = tx ? tx->valuestring : "";
        notes_cache.push_back(note);
    }
    cJSON_Delete(root);

    // 按时间升序排列（最早在前，便于 FIFO 截断）
    std::sort(notes_cache.begin(), notes_cache.end(),
              [](const Note& a, const Note& b) { return a.timestamp < b.timestamp; });

    ESP_LOGI(TAG, "Loaded %d notes from disk.", (int)notes_cache.size());
    return true;
}

bool notes_init() {
    if (is_initialized) return true;

    notes_mutex = xSemaphoreCreateMutex();
    if (!notes_mutex) {
        ESP_LOGE(TAG, "Failed to create notes mutex");
        return false;
    }

    if (sd_card_init() != 0) {
        ESP_LOGW(TAG, "SD Card init failed! Notes will run in memory-only mode.");
        sd_card_available = false;
    } else {
        sd_card_available = true;
        if (!load_from_disk()) {
            ESP_LOGE(TAG, "Failed to load notes from SD card.");
        }
    }

    is_initialized = true;
    ESP_LOGI(TAG, "Notes board initialized. (Storage: %s)",
             sd_card_available ? "SD Card" : "Memory-only");
    return true;
}

std::vector<Note> notes_get_all() {
    std::vector<Note> result;
    if (!notes_mutex) return result;
    xSemaphoreTake(notes_mutex, portMAX_DELAY);
    result = notes_cache;
    xSemaphoreGive(notes_mutex);
    // 调用方期望最新在前：反转升序为倒序
    std::reverse(result.begin(), result.end());
    return result;
}

time_t notes_add(const std::string& text) {
    if (!is_initialized || text.empty()) return 0;

    xSemaphoreTake(notes_mutex, portMAX_DELAY);

    time_t now;
    time(&now);

    Note note;
    note.timestamp = now;
    note.text = text;
    notes_cache.push_back(note);

    // FIFO 截断：丢弃最早的（升序排列时最早的在头部）
    while (notes_cache.size() > MAX_NOTES) {
        notes_cache.erase(notes_cache.begin());
    }

    bool ok = save_to_disk();
    xSemaphoreGive(notes_mutex);

    ESP_LOGI(TAG, "Added note (ts=%lld): %.40s", (long long)now, text.c_str());
    return ok ? now : 0;
}

bool notes_delete(time_t timestamp) {
    if (!is_initialized) return false;

    xSemaphoreTake(notes_mutex, portMAX_DELAY);
    bool found = false;
    for (auto it = notes_cache.begin(); it != notes_cache.end(); ++it) {
        if (it->timestamp == timestamp) {
            notes_cache.erase(it);
            found = true;
            break;
        }
    }
    if (found) save_to_disk();
    xSemaphoreGive(notes_mutex);

    if (found) ESP_LOGI(TAG, "Deleted note ts=%lld", (long long)timestamp);
    return found;
}

bool notes_clear() {
    if (!is_initialized) return false;
    xSemaphoreTake(notes_mutex, portMAX_DELAY);
    notes_cache.clear();
    bool ok = save_to_disk();
    xSemaphoreGive(notes_mutex);
    ESP_LOGI(TAG, "Cleared all notes");
    return ok;
}

bool notes_is_persistent() {
    return sd_card_available;
}

} // namespace dashboard
} // namespace smart_fridge
