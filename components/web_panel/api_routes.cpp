#include "api_routes.h"
#include "web_panel.h"
#include <cJSON.h>
#include <esp_log.h>
#include <cstdlib>
#include <string>
#include <string.h>
#include "inventory.hpp"

static const char *TAG = "WebPanelApi";

#include "inventory_history.hpp"
#include "credentials_manager.h"
#include "ai_agent.hpp"
#include "tts_client.hpp"
#include "esp_system.h"
#include "esp_wifi.h"
#include "rtc_time.h"
#include "notes_board.hpp"
#include "weather.hpp"
#include "dashboard.hpp"
#include "gui_bridge.h"
#include "gui_app.h"
#include "system_events.h"
#include "audio_api.h"

static char* receive_request_body(httpd_req_t* req, size_t max_length) {
    if (!req || req->content_len <= 0 ||
        (size_t)req->content_len > max_length) {
        return NULL;
    }
    char* body = (char*)malloc((size_t)req->content_len + 1);
    if (!body) return NULL;
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(
            req, body + received,
            (size_t)req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) {
            free(body);
            return NULL;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';
    return body;
}

static void copy_json_config_field(cJSON* item, char* target,
                                   size_t target_size) {
    if (!cJSON_IsString(item) || !item->valuestring ||
        !target || target_size == 0) {
        return;
    }
    strncpy(target, item->valuestring, target_size - 1);
    target[target_size - 1] = '\0';
}

static const char* inventory_error_code(smart_fridge::inventory::InventoryError error) {
    using smart_fridge::inventory::InventoryError;
    switch (error) {
        case InventoryError::NOT_INITIALIZED: return "inventory_not_initialized";
        case InventoryError::INVALID_NAME: return "invalid_name";
        case InventoryError::INVALID_CATEGORY: return "invalid_category";
        case InventoryError::INVALID_QUANTITY: return "invalid_quantity";
        case InventoryError::INVALID_EXPIRE_DAYS: return "invalid_expire_days";
        case InventoryError::INVALID_ENTRY_TIME: return "invalid_entry_time";
        case InventoryError::NOT_FOUND: return "item_not_found";
        case InventoryError::INSUFFICIENT_QUANTITY: return "insufficient_quantity";
        case InventoryError::STORAGE_ERROR: return "storage_error";
        default: return "inventory_error";
    }
}

static const char* inventory_http_status(smart_fridge::inventory::InventoryError error) {
    using smart_fridge::inventory::InventoryError;
    switch (error) {
        case InventoryError::NOT_FOUND:
            return "404 Not Found";
        case InventoryError::INSUFFICIENT_QUANTITY:
            return "409 Conflict";
        case InventoryError::STORAGE_ERROR:
        case InventoryError::NOT_INITIALIZED:
            return "500 Internal Server Error";
        default:
            return "400 Bad Request";
    }
}

static esp_err_t send_json_error(httpd_req_t* req, const char* http_status,
                                 const char* code, const char* message,
                                 bool confirmation_required = false) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "code", code ? code : "request_failed");
    cJSON_AddStringToObject(root, "error", message ? message : "请求失败");
    if (confirmation_required) {
        cJSON_AddBoolToObject(root, "confirmation_required", true);
    }
    const char* json = cJSON_PrintUnformatted(root);
    httpd_resp_set_status(req, http_status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free((void*)json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t send_inventory_result(httpd_req_t* req,
                                       const smart_fridge::inventory::InventoryResult& result,
                                       const char* success_message) {
    if (!result.ok()) {
        return send_json_error(
            req,
            inventory_http_status(result.error),
            inventory_error_code(result.error),
            smart_fridge::inventory::inventory_error_message(result.error));
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "message", success_message ? success_message : "操作成功");
    cJSON_AddNumberToObject(root, "affected_quantity", result.affected_quantity);
    cJSON_AddNumberToObject(root, "remaining_quantity", result.remaining_quantity);
    const char* json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free((void*)json);
    cJSON_Delete(root);

    gui_bridge_refresh_inventory();
    web_panel_broadcast_ws("update");
    return ESP_OK;
}

// GET /api/inventory
static esp_err_t get_inventory_handler(httpd_req_t *req) {
    auto items = smart_fridge::inventory::get_all_ingredients();
    cJSON *root = cJSON_CreateArray();
    for (const auto& item : items) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", item.id);
        cJSON_AddStringToObject(obj, "name", item.name.c_str());
        cJSON_AddStringToObject(obj, "category", item.category.c_str());
        cJSON_AddNumberToObject(obj, "quantity", item.total_quantity);
        if (!item.batches.empty()) {
            cJSON_AddNumberToObject(obj, "expire_days", item.batches.front().expire_days);
            cJSON_AddNumberToObject(obj, "entry_time", item.batches.front().entry_time);
        } else {
            cJSON_AddNumberToObject(obj, "expire_days", 0);
            cJSON_AddNumberToObject(obj, "entry_time", 0);
        }
        
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
    const char *sys_info = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sys_info, strlen(sys_info));
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/inventory
static esp_err_t post_inventory_handler(httpd_req_t *req) {
    char buf[512];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        return send_json_error(req, "400 Bad Request", "payload_too_large", "请求内容过大");
    }
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return send_json_error(req, "400 Bad Request", "invalid_json", "请求 JSON 格式错误");
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *confirmed = cJSON_GetObjectItem(root, "confirmed");
    if (!action || !cJSON_IsString(action) || !action->valuestring) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "missing_action", "缺少库存操作类型");
    }

    if (!cJSON_IsTrue(confirmed)) {
        cJSON_Delete(root);
        return send_json_error(req, "409 Conflict", "confirmation_required",
                               "库存写操作需要用户确认", true);
    }

    const char* action_name = action->valuestring;
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *qty = cJSON_GetObjectItem(root, "quantity");
    if (!name || !cJSON_IsString(name) || !name->valuestring) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "invalid_name", "食材名称不能为空");
    }

    smart_fridge::inventory::InventoryResult result;
    const char* success_message = "库存已更新";

    if (strcmp(action_name, "add") == 0) {
        cJSON *cat = cJSON_GetObjectItem(root, "category");
        cJSON *exp = cJSON_GetObjectItem(root, "expire_days");
        if (!cat || !cJSON_IsString(cat) || !qty || !cJSON_IsNumber(qty) ||
            !exp || !cJSON_IsNumber(exp)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "invalid_parameters",
                                   "存入操作缺少分类、数量或保质期");
        }
        result = smart_fridge::inventory::add_ingredient_checked(
            name->valuestring, cat->valuestring, qty->valueint, exp->valueint);
        success_message = "食材已存入";
    } else if (strcmp(action_name, "remove") == 0) {
        if (!qty || !cJSON_IsNumber(qty)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "invalid_quantity", "取出数量无效");
        }
        result = smart_fridge::inventory::remove_ingredient_checked(
            name->valuestring, qty->valueint);
        success_message = "食材已取出";
    } else if (strcmp(action_name, "clear") == 0) {
        result = smart_fridge::inventory::clear_ingredient_checked(name->valuestring);
        success_message = "食材已全部清空";
    } else if (strcmp(action_name, "update") == 0) {
        cJSON *cat = cJSON_GetObjectItem(root, "category");
        cJSON *exp = cJSON_GetObjectItem(root, "expire_days");
        cJSON *ent = cJSON_GetObjectItem(root, "entry_time");
        if (!cat || !cJSON_IsString(cat) || !qty || !cJSON_IsNumber(qty) ||
            !exp || !cJSON_IsNumber(exp) || !ent || !cJSON_IsNumber(ent)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "invalid_parameters",
                                   "修改操作缺少分类、数量、保质期或存入时间");
        }
        result = smart_fridge::inventory::update_ingredient_checked(
            name->valuestring, qty->valueint, cat->valuestring, exp->valueint,
            (time_t)ent->valuedouble);
        success_message = "食材信息已修改";
    } else {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "unsupported_action", "不支持的库存操作");
    }

    cJSON_Delete(root);
    return send_inventory_result(req, result, success_message);
}

// GET /api/history
static esp_err_t get_history_handler(httpd_req_t *req) {
    auto records = smart_fridge::inventory::history_get_recent(0);
    cJSON *root = cJSON_CreateArray();
    for (const auto& rec : records) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "timestamp", rec.timestamp);
        cJSON_AddStringToObject(obj, "action", rec.action == smart_fridge::inventory::HistoryAction::ADD ? "ADD" : "REMOVE");
        cJSON_AddStringToObject(obj, "item_name", rec.item_name.c_str());
        cJSON_AddNumberToObject(obj, "quantity", rec.quantity);
        cJSON_AddItemToArray(root, obj);
    }
    const char *sys_info = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sys_info, strlen(sys_info));
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

// GET /api/status
static esp_err_t get_status_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "wifi_rssi", ap_info.rssi);
    } else {
        cJSON_AddNumberToObject(root, "wifi_rssi", 0);
    }
    
    // 增加时钟同步和系统时间状态
    cJSON_AddBoolToObject(root, "rtc_synced", rtc_time_is_synced());
    char time_buf[32] = {0};
    if (rtc_time_is_synced()) {
        rtc_time_get_formatted(time_buf, sizeof(time_buf));
        cJSON_AddStringToObject(root, "sys_time", time_buf);
    } else {
        cJSON_AddStringToObject(root, "sys_time", "Not Synced");
    }
    
    // 增加 SD 卡状态
    cJSON_AddBoolToObject(root, "sd_available", smart_fridge::inventory::is_sd_card_available());

    const char *sys_info = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sys_info, strlen(sys_info));
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

// GET /api/settings
static esp_err_t get_settings_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", credentials_get_wifi_ssid() ? credentials_get_wifi_ssid() : "");
    cJSON_AddStringToObject(root, "wifi_pass", "********"); // Mask password
    cJSON_AddStringToObject(root, "llm_url", credentials_get_llm_api_url() ? credentials_get_llm_api_url() : "");
    cJSON_AddStringToObject(root, "llm_key", "");
    cJSON_AddBoolToObject(root, "llm_has_key",
                         credentials_get_llm_api_key() && credentials_get_llm_api_key()[0] != '\0');
    cJSON_AddStringToObject(root, "llm_model", credentials_get_llm_model() ? credentials_get_llm_model() : "");
    const char *sys_info = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sys_info, strlen(sys_info));
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/settings
static esp_err_t post_settings_handler(httpd_req_t *req) {
    char buf[512];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "wifi_pass");
    if (ssid && pass && strlen(pass->valuestring) > 0 && strcmp(pass->valuestring, "********") != 0) {
        credentials_set_wifi(ssid->valuestring, pass->valuestring);
    }
    cJSON *url = cJSON_GetObjectItem(root, "llm_url");
    cJSON *key = cJSON_GetObjectItem(root, "llm_key");
    if (url && cJSON_IsString(url)) {
        const char* new_key = (key && cJSON_IsString(key)) ? key->valuestring : NULL;
        credentials_set_llm_api(url->valuestring, new_key);
    }
    cJSON *model = cJSON_GetObjectItem(root, "llm_model");
    if (model && strlen(model->valuestring) > 0) {
        credentials_set_llm_model(model->valuestring);
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}

// POST /api/chat
static esp_err_t post_chat_handler(httpd_req_t *req) {
    char buf[512];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (text && text->valuestring) {
        esp_err_t err = voice_session_submit_text(text->valuestring);
        cJSON_Delete(root);
        if (err == ESP_OK) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"status\":\"accepted\"}", -1);
            return ESP_OK;
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Voice session queue unavailable");
            return ESP_FAIL;
        }
    }
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing text");
    return ESP_FAIL;
}

// GET /api/recipes/match
static esp_err_t get_recipes_match_handler(httpd_req_t *req) {
    auto matches = smart_fridge::inventory::recipe_match_near(2);
    cJSON *root = cJSON_CreateArray();
    for (const auto& match : matches) {
        cJSON *obj = cJSON_CreateObject();
        
        cJSON *recipe_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(recipe_obj, "name", match.recipe.name.c_str());
        cJSON_AddStringToObject(recipe_obj, "category", match.recipe.category.c_str());
        cJSON_AddStringToObject(recipe_obj, "brief", match.recipe.brief.c_str());
        
        cJSON *ing_arr = cJSON_CreateArray();
        for (const auto& ing : match.recipe.ingredients) {
            cJSON *ing_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(ing_obj, "name", ing.name.c_str());
            cJSON_AddNumberToObject(ing_obj, "min_quantity", ing.min_quantity);
            cJSON_AddItemToArray(ing_arr, ing_obj);
        }
        cJSON_AddItemToObject(recipe_obj, "ingredients", ing_arr);
        cJSON_AddItemToObject(obj, "recipe", recipe_obj);
        
        cJSON_AddNumberToObject(obj, "coverage", match.coverage);
        cJSON_AddNumberToObject(obj, "missing_count", match.missing_count);
        
        cJSON *missing_arr = cJSON_CreateArray();
        for (const auto& m_item : match.missing_items) {
            cJSON_AddItemToArray(missing_arr, cJSON_CreateString(m_item.c_str()));
        }
        cJSON_AddItemToObject(obj, "missing_items", missing_arr);
        
        cJSON_AddItemToArray(root, obj);
    }
    const char *json_resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, strlen(json_resp));
    free((void *)json_resp);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/recipes
static esp_err_t post_recipes_handler(httpd_req_t *req) {
    char *buf = (char*)malloc(req->content_len + 1);
    if (!buf) return ESP_FAIL;
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (action && action->valuestring) {
        if (strcmp(action->valuestring, "remove") == 0) {
            cJSON *name = cJSON_GetObjectItem(root, "name");
            if (name) {
                smart_fridge::inventory::recipe_remove(name->valuestring);
            }
        } else if (strcmp(action->valuestring, "add") == 0 || strcmp(action->valuestring, "update") == 0) {
            smart_fridge::inventory::Recipe new_recipe;
            cJSON *name = cJSON_GetObjectItem(root, "name");
            cJSON *category = cJSON_GetObjectItem(root, "category");
            cJSON *brief = cJSON_GetObjectItem(root, "brief");
            cJSON *ingredients = cJSON_GetObjectItem(root, "ingredients");

            if (name) new_recipe.name = name->valuestring;
            if (category) new_recipe.category = category->valuestring;
            if (brief) new_recipe.brief = brief->valuestring;
            
            if (ingredients && cJSON_IsArray(ingredients)) {
                int count = cJSON_GetArraySize(ingredients);
                for (int i = 0; i < count; i++) {
                    cJSON *ing_obj = cJSON_GetArrayItem(ingredients, i);
                    if (ing_obj) {
                        smart_fridge::inventory::RecipeIngredient ri;
                        cJSON *ing_name = cJSON_GetObjectItem(ing_obj, "name");
                        cJSON *ing_qty = cJSON_GetObjectItem(ing_obj, "min_quantity");
                        if (ing_name) ri.name = ing_name->valuestring;
                        if (ing_qty) ri.min_quantity = ing_qty->valueint;
                        else ri.min_quantity = 1;
                        new_recipe.ingredients.push_back(ri);
                    }
                }
            }

            if (strcmp(action->valuestring, "add") == 0) {
                smart_fridge::inventory::recipe_add(new_recipe);
            } else {
                cJSON *old_name = cJSON_GetObjectItem(root, "old_name");
                if (old_name) {
                    smart_fridge::inventory::recipe_update(old_name->valuestring, new_recipe);
                } else if (name) {
                    // fallback to name if old_name not provided
                    smart_fridge::inventory::recipe_update(name->valuestring, new_recipe);
                }
            }
        }
    }
    
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}

// WS Handler
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (ws_pkt.len > 0) {
        uint8_t *buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
        if (buf) {
            ws_pkt.payload = buf;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (ret == ESP_OK && ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
                audio_hal_push_external_pcm(buf, ws_pkt.len);
            } else if (ret == ESP_OK &&
                       ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
                cJSON* root = cJSON_Parse((const char*)buf);
                cJSON* type = root ? cJSON_GetObjectItem(root, "type")
                                   : NULL;
                const char* type_str =
                    cJSON_IsString(type) ? type->valuestring : NULL;
                if (type_str && strcmp(type_str, "voice.start") == 0) {
                    cJSON* mode = cJSON_GetObjectItem(root, "mode");
                    voice_session_set_mode(
                        cJSON_IsString(mode) &&
                                strcmp(mode->valuestring, "realtime") == 0
                            ? AUDIO_VOICE_MODE_REALTIME
                            : AUDIO_VOICE_MODE_CASCADE);
                    voice_session_start(true);
                } else if (type_str &&
                           strcmp(type_str, "voice.stop") == 0) {
                    audio_hal_finish_external_listening();
                } else if (type_str &&
                           strcmp(type_str, "voice.close") == 0) {
                    voice_session_stop();
                } else if (type_str &&
                           strcmp(type_str, "voice.interrupt") == 0) {
                    voice_session_interrupt();
                } else if (type_str &&
                           strcmp(type_str, "chat.text") == 0) {
                    cJSON* text = cJSON_GetObjectItem(root, "text");
                    if (cJSON_IsString(text) && text->valuestring) {
                        voice_session_submit_text(text->valuestring);
                    }
                }
                cJSON_Delete(root);
            }
            free(buf);
        }
    }
    return ESP_OK;
}

// ============================================================
// Notes Board (留言板)
// ============================================================

// GET /api/notes — 返回留言列表
static esp_err_t get_notes_handler(httpd_req_t *req) {
    auto notes = smart_fridge::dashboard::notes_get_all();
    cJSON *root = cJSON_CreateArray();
    for (const auto& note : notes) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "timestamp", (double)note.timestamp);
        cJSON_AddStringToObject(obj, "text", note.text.c_str());
        cJSON_AddItemToArray(root, obj);
    }
    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free((void *)json);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/notes — 增/删/清空留言
static esp_err_t post_notes_handler(httpd_req_t *req) {
    char buf[1024];
    int ret, remaining = req->content_len;
    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (action && action->valuestring) {
        if (strcmp(action->valuestring, "add") == 0) {
            cJSON *text = cJSON_GetObjectItem(root, "text");
            if (text && text->valuestring && strlen(text->valuestring) > 0) {
                time_t new_ts = smart_fridge::dashboard::notes_add(text->valuestring);
                if (new_ts) {
                    gui_launcher_mark_note_new((long long)new_ts);
                }
            }
        } else if (strcmp(action->valuestring, "delete") == 0) {
            cJSON *ts = cJSON_GetObjectItem(root, "timestamp");
            if (ts) {
                smart_fridge::dashboard::notes_delete((time_t)ts->valuedouble);
            }
        } else if (strcmp(action->valuestring, "clear") == 0) {
            smart_fridge::dashboard::notes_clear();
        }
    }
    cJSON_Delete(root);

    // 通知 LCD 桌面刷新留言板
    gui_bridge_refresh_dashboard();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}

// ============================================================
// Weather (天气)
// ============================================================

// GET /api/weather — 返回天气缓存 + 配置（key 脱敏）
static esp_err_t get_weather_handler(httpd_req_t *req) {
    auto w = smart_fridge::dashboard::weather_get();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "valid", w.valid);
    if (w.valid) {
        cJSON_AddNumberToObject(root, "temp", w.temp);
        cJSON_AddStringToObject(root, "text", w.text.c_str());
        cJSON_AddStringToObject(root, "city", w.city.c_str());
        cJSON_AddNumberToObject(root, "updated", (double)w.updated);
    }
    // 配置（API Key 脱敏）
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "wx_url", credentials_get_weather_url() ? credentials_get_weather_url() : "");
    cJSON_AddStringToObject(cfg, "wx_key", (credentials_get_weather_key() && strlen(credentials_get_weather_key()) > 0) ? "********" : "");
    cJSON_AddStringToObject(cfg, "wx_city", credentials_get_weather_city() ? credentials_get_weather_city() : "");
    cJSON_AddStringToObject(cfg, "wx_location", credentials_get_weather_location() ? credentials_get_weather_location() : "");
    cJSON_AddStringToObject(cfg, "wx_temp_path", credentials_get_weather_temp_path() ? credentials_get_weather_temp_path() : "");
    cJSON_AddStringToObject(cfg, "wx_text_path", credentials_get_weather_text_path() ? credentials_get_weather_text_path() : "");
    cJSON_AddItemToObject(root, "config", cfg);

    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free((void *)json);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/weather — 更新天气 API 配置并触发立即刷新
static esp_err_t post_weather_handler(httpd_req_t *req) {
    char *buf = (char*)malloc(req->content_len + 1);
    if (!buf) return ESP_FAIL;
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) { free(buf); return ESP_FAIL; }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // 仅当传入了非空、非脱敏占位值时才更新
    cJSON *url = cJSON_GetObjectItem(root, "wx_url");
    if (url && url->valuestring) credentials_set_weather_url(url->valuestring);

    cJSON *key = cJSON_GetObjectItem(root, "wx_key");
    if (key && key->valuestring && strlen(key->valuestring) > 0 &&
        strcmp(key->valuestring, "********") != 0) {
        credentials_set_weather_key(key->valuestring);
    }

    cJSON *city = cJSON_GetObjectItem(root, "wx_city");
    if (city && city->valuestring) credentials_set_weather_city(city->valuestring);

    cJSON *loc = cJSON_GetObjectItem(root, "wx_location");
    if (loc && loc->valuestring) credentials_set_weather_location(loc->valuestring);

    cJSON *tp = cJSON_GetObjectItem(root, "wx_temp_path");
    if (tp && tp->valuestring) credentials_set_weather_temp_path(tp->valuestring);

    cJSON *txp = cJSON_GetObjectItem(root, "wx_text_path");
    if (txp && txp->valuestring) credentials_set_weather_text_path(txp->valuestring);

    cJSON_Delete(root);

    // 配置更新后立即尝试刷新一次
    bool ok = smart_fridge::dashboard::weather_refresh();

    httpd_resp_set_type(req, "application/json");
    if (ok) {
        httpd_resp_send(req, "{\"status\":\"ok\",\"refreshed\":true}", -1);
    } else {
        httpd_resp_send(req, "{\"status\":\"ok\",\"refreshed\":false}", -1);
    }
    return ESP_OK;
}

// POST /api/weather/refresh — 手动触发天气刷新
static esp_err_t post_weather_refresh_handler(httpd_req_t *req) {
    bool ok = smart_fridge::dashboard::weather_refresh();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ok ? "{\"status\":\"ok\",\"refreshed\":true}"
                            : "{\"status\":\"ok\",\"refreshed\":false}", -1);
    return ESP_OK;
}

// ============================================================
// Voice Model Config (ASR / LLM / TTS)
// ============================================================

static const char* voice_type_str(voice_model_type_t type) {
    switch (type) {
        case VOICE_MODEL_ASR: return "asr";
        case VOICE_MODEL_LLM: return "llm";
        case VOICE_MODEL_TTS: return "tts";
        case VOICE_MODEL_REALTIME: return "realtime";
        default: return "unknown";
    }
}

static voice_model_type_t voice_type_from_str(const char* s) {
    if (!s) return VOICE_MODEL_COUNT;
    if (strcmp(s, "asr") == 0) return VOICE_MODEL_ASR;
    if (strcmp(s, "llm") == 0) return VOICE_MODEL_LLM;
    if (strcmp(s, "tts") == 0) return VOICE_MODEL_TTS;
    if (strcmp(s, "realtime") == 0) return VOICE_MODEL_REALTIME;
    return VOICE_MODEL_COUNT;
}

static cJSON* voice_config_to_json(const voice_model_config_t* cfg, bool mask_key) {
    cJSON* obj = cJSON_CreateObject();
    if (!obj) return nullptr;
    cJSON_AddStringToObject(obj, "provider", cfg->provider);
    cJSON_AddStringToObject(obj, "url", cfg->url);
    cJSON_AddStringToObject(obj, "key", mask_key ? "" : cfg->key);
    cJSON_AddBoolToObject(obj, "has_key", cfg->key[0] != '\0');
    cJSON_AddStringToObject(obj, "model", cfg->model);
    cJSON_AddStringToObject(obj, "extra", cfg->extra);
    return obj;
}

static void json_to_voice_config(cJSON* obj,
                                 const voice_model_config_t* current,
                                 voice_model_config_t* cfg) {
    if (!obj || !cfg) return;
    if (current) {
        *cfg = *current;
    } else {
        memset(cfg, 0, sizeof(voice_model_config_t));
    }

    cJSON* provider = cJSON_GetObjectItem(obj, "provider");
    cJSON* url = cJSON_GetObjectItem(obj, "url");
    cJSON* key = cJSON_GetObjectItem(obj, "key");
    cJSON* model = cJSON_GetObjectItem(obj, "model");
    cJSON* extra = cJSON_GetObjectItem(obj, "extra");

    copy_json_config_field(provider, cfg->provider, sizeof(cfg->provider));
    copy_json_config_field(url, cfg->url, sizeof(cfg->url));
    if (key && cJSON_IsString(key) && key->valuestring && key->valuestring[0] != '\0' &&
        strcmp(key->valuestring, "********") != 0 &&
        strcmp(key->valuestring, "sk-********") != 0) {
        copy_json_config_field(key, cfg->key, sizeof(cfg->key));
    }
    copy_json_config_field(model, cfg->model, sizeof(cfg->model));
    copy_json_config_field(extra, cfg->extra, sizeof(cfg->extra));
}

static const char* validate_voice_config(voice_model_type_t type,
                                         const voice_model_config_t* cfg) {
    if (!cfg->provider[0]) return "Provider 不能为空";
    if (!cfg->url[0]) return "API URL 不能为空";
    if (strncmp(cfg->url, "wss://", 6) != 0 &&
        strncmp(cfg->url, "https://", 8) != 0 &&
        strncmp(cfg->url, "ws://", 5) != 0 &&
        strncmp(cfg->url, "http://", 7) != 0) {
        return "API URL 必须以 wss:// 或 https:// 开头";
    }
    if (!cfg->model[0]) return "Model/资源 ID 不能为空";
    if ((type == VOICE_MODEL_ASR || type == VOICE_MODEL_TTS ||
         type == VOICE_MODEL_REALTIME) &&
        !cfg->key[0]) {
        return "API Key 不能为空";
    }
    if ((type == VOICE_MODEL_TTS || type == VOICE_MODEL_REALTIME) &&
        cfg->extra[0]) {
        cJSON* extra = cJSON_Parse(cfg->extra);
        if (!extra) return "Extra 必须是有效 JSON";
        cJSON_Delete(extra);
    }
    return NULL;
}

// GET /api/voice/config
static esp_err_t get_voice_config_handler(httpd_req_t *req) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;

    for (int t = 0; t < VOICE_MODEL_COUNT; t++) {
        voice_model_type_t type = (voice_model_type_t)t;
        cJSON* section = cJSON_CreateObject();
        const voice_model_config_t* cur = credentials_get_voice_model_config(type);
        cJSON* cur_json = voice_config_to_json(cur, true);
        cJSON_AddItemToObject(section, "current", cur_json);

        cJSON* hist_arr = cJSON_CreateArray();
        int hist_count = credentials_get_voice_model_history_count(type);
        for (int h = 0; h < hist_count; h++) {
            const voice_model_config_t* hist = credentials_get_voice_model_history(type, h);
            cJSON* hist_json = voice_config_to_json(hist, true);
            cJSON_AddItemToArray(hist_arr, hist_json);
        }
        cJSON_AddItemToObject(section, "history", hist_arr);
        cJSON_AddItemToObject(root, voice_type_str(type), section);
    }

    const char* json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free((void*)json);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/voice/config
static esp_err_t post_voice_config_handler(httpd_req_t *req) {
    char* buf = receive_request_body(req, 4096);
    if (!buf) {
        return send_json_error(req, "400 Bad Request",
                               "invalid_body", "请求体为空、过大或未接收完整");
    }

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* type_json = cJSON_GetObjectItem(root, "type");
    cJSON* cfg_json = cJSON_GetObjectItem(root, "config");

    if (!type_json || !type_json->valuestring || !cfg_json) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing type or config");
        return ESP_FAIL;
    }

    voice_model_type_t type = voice_type_from_str(type_json->valuestring);
    if (type == VOICE_MODEL_COUNT) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid type");
        return ESP_FAIL;
    }

    voice_model_config_t cfg;
    const voice_model_config_t* current = credentials_get_voice_model_config(type);
    json_to_voice_config(cfg_json, current, &cfg);
    cJSON_Delete(root);

    const char* validation_error = validate_voice_config(type, &cfg);
    if (validation_error) {
        return send_json_error(req, "400 Bad Request",
                               "invalid_config", validation_error);
    }

    esp_err_t err = credentials_set_voice_model_config(type, &cfg);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        const voice_model_config_t* persisted =
            credentials_get_voice_model_config(type);
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "ok");
        cJSON_AddBoolToObject(response, "persisted", true);
        cJSON_AddItemToObject(
            response, "config", voice_config_to_json(persisted, true));
        const char* json = cJSON_PrintUnformatted(response);
        httpd_resp_send(req, json, strlen(json));
        free((void*)json);
        cJSON_Delete(response);
    } else {
        return send_json_error(
            req, "500 Internal Server Error", "nvs_save_failed",
            esp_err_to_name(err));
    }
    return ESP_OK;
}

// POST /api/voice/config/delete
static esp_err_t post_voice_config_delete_handler(httpd_req_t *req) {
    char* buf = receive_request_body(req, 1024);
    if (!buf) {
        return send_json_error(req, "400 Bad Request",
                               "invalid_body", "请求体未接收完整");
    }

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* type_json = cJSON_GetObjectItem(root, "type");
    cJSON* index_json = cJSON_GetObjectItem(root, "index");

    if (!type_json || !type_json->valuestring || !index_json || !cJSON_IsNumber(index_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing type or index");
        return ESP_FAIL;
    }

    voice_model_type_t type = voice_type_from_str(type_json->valuestring);
    int index = index_json->valueint;
    cJSON_Delete(root);

    if (type == VOICE_MODEL_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid type");
        return ESP_FAIL;
    }

    esp_err_t err = credentials_delete_voice_model_history(type, index);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete history");
    }
    return (err == ESP_OK) ? ESP_OK : ESP_FAIL;
}

// POST /api/voice/test-tts — 测试 TTS 配置
static esp_err_t post_voice_test_tts_handler(httpd_req_t *req) {
    char* buf = receive_request_body(req, 2048);
    if (!buf) {
        return send_json_error(req, "400 Bad Request",
                               "invalid_body", "请求体未接收完整");
    }

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* text_json = cJSON_GetObjectItem(root, "text");
    std::string text =
        (cJSON_IsString(text_json) && text_json->valuestring)
            ? text_json->valuestring
            : "你好，我是小鲜";
    cJSON_Delete(root);

    size_t received_bytes = 0;
        smart_fridge::audio::tts_result_t result =
        smart_fridge::audio::tts_synthesize_stream(
            text,
            [&received_bytes](const uint8_t*, size_t length) {
                received_bytes += length;
                return true;
            },
            []() { return false; });
    httpd_resp_set_type(req, "application/json");
    if (result.success && received_bytes > 0) {
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddNumberToObject(resp, "audio_bytes", (int)received_bytes);
        cJSON_AddStringToObject(resp, "format", result.format.c_str());
        const char* json = cJSON_PrintUnformatted(resp);
        httpd_resp_send(req, json, strlen(json));
        free((void*)json);
        cJSON_Delete(resp);
        return ESP_OK;
    } else {
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "error", result.error_hint.c_str());
        const char* json = cJSON_PrintUnformatted(resp);
        httpd_resp_send(req, json, strlen(json));
        free((void*)json);
        cJSON_Delete(resp);
        return ESP_OK;
    }
}

void register_api_routes(httpd_handle_t server) {
    httpd_uri_t uri_get_inv = { .uri = "/api/inventory", .method = HTTP_GET, .handler = get_inventory_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_inv = { .uri = "/api/inventory", .method = HTTP_POST, .handler = post_inventory_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_get_hist = { .uri = "/api/history", .method = HTTP_GET, .handler = get_history_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_get_status = { .uri = "/api/status", .method = HTTP_GET, .handler = get_status_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_get_set = { .uri = "/api/settings", .method = HTTP_GET, .handler = get_settings_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_set = { .uri = "/api/settings", .method = HTTP_POST, .handler = post_settings_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_chat = { .uri = "/api/chat", .method = HTTP_POST, .handler = post_chat_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_get_recipes = { .uri = "/api/recipes/match", .method = HTTP_GET, .handler = get_recipes_match_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_recipes = { .uri = "/api/recipes", .method = HTTP_POST, .handler = post_recipes_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_get_notes = { .uri = "/api/notes", .method = HTTP_GET, .handler = get_notes_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_notes = { .uri = "/api/notes", .method = HTTP_POST, .handler = post_notes_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_get_weather = { .uri = "/api/weather", .method = HTTP_GET, .handler = get_weather_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_weather = { .uri = "/api/weather", .method = HTTP_POST, .handler = post_weather_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_weather_refresh = { .uri = "/api/weather/refresh", .method = HTTP_POST, .handler = post_weather_refresh_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_get_voice_cfg = { .uri = "/api/voice/config", .method = HTTP_GET, .handler = get_voice_config_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_voice_cfg = { .uri = "/api/voice/config", .method = HTTP_POST, .handler = post_voice_config_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_voice_cfg_del = { .uri = "/api/voice/config/delete", .method = HTTP_POST, .handler = post_voice_config_delete_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_post_voice_test_tts = { .uri = "/api/voice/test-tts", .method = HTTP_POST, .handler = post_voice_test_tts_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
    httpd_uri_t uri_ws = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .user_ctx = NULL, .is_websocket = true, .handle_ws_control_frames = false, .supported_subprotocol = NULL };

    httpd_register_uri_handler(server, &uri_get_inv);
    httpd_register_uri_handler(server, &uri_post_inv);
    httpd_register_uri_handler(server, &uri_get_hist);
    httpd_register_uri_handler(server, &uri_get_status);
    httpd_register_uri_handler(server, &uri_get_set);
    httpd_register_uri_handler(server, &uri_post_set);
    httpd_register_uri_handler(server, &uri_post_chat);
    httpd_register_uri_handler(server, &uri_get_recipes);
    httpd_register_uri_handler(server, &uri_post_recipes);
    httpd_register_uri_handler(server, &uri_get_notes);
    httpd_register_uri_handler(server, &uri_post_notes);
    httpd_register_uri_handler(server, &uri_get_weather);
    httpd_register_uri_handler(server, &uri_post_weather);
    httpd_register_uri_handler(server, &uri_post_weather_refresh);
    httpd_register_uri_handler(server, &uri_get_voice_cfg);
    httpd_register_uri_handler(server, &uri_post_voice_cfg);
    httpd_register_uri_handler(server, &uri_post_voice_cfg_del);
    httpd_register_uri_handler(server, &uri_post_voice_test_tts);
    httpd_register_uri_handler(server, &uri_ws);
}

struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
    char *msg;  // 使用 strdup 分配的独立副本，发送后需 free
    size_t length;
    bool binary;
};

static void ws_async_send(void *arg) {
    struct async_resp_arg *resp_arg = (struct async_resp_arg *)arg;
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)resp_arg->msg;
    ws_pkt.len = resp_arg->length;
    ws_pkt.type = resp_arg->binary ? HTTPD_WS_TYPE_BINARY
                                   : HTTPD_WS_TYPE_TEXT;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    free(resp_arg->msg);   // 释放 strdup 分配的消息副本
    free(resp_arg);
}

void broadcast_ws_message(httpd_handle_t server, const char* msg) {
    if (!server) return;
    
    size_t max_clients = 8;
    size_t fds = max_clients;
    int client_fds[8];
    if (httpd_get_client_list(server, &fds, client_fds) == ESP_OK) {
        for (size_t i = 0; i < fds; i++) {
            int client_info = httpd_ws_get_fd_info(server, client_fds[i]);
            if (client_info == HTTPD_WS_CLIENT_WEBSOCKET) {
                struct async_resp_arg *arg = (struct async_resp_arg *)malloc(sizeof(struct async_resp_arg));
                if (arg) {
                    arg->hd = server;
                    arg->fd = client_fds[i];
                    arg->msg = strdup(msg);  // 复制消息内容，避免异步发送时原指针已失效
                    if (arg->msg) {
                        arg->length = strlen(msg);
                        arg->binary = false;
                        if (httpd_queue_work(server, ws_async_send, arg) !=
                            ESP_OK) {
                            free(arg->msg);
                            free(arg);
                        }
                    } else {
                        free(arg);
                    }
                }
            }
        }
    }
}

void broadcast_ws_binary(httpd_handle_t server, const uint8_t* data,
                         size_t length) {
    if (!server || !data || !length) return;
    size_t count = 8;
    int client_fds[8];
    if (httpd_get_client_list(server, &count, client_fds) != ESP_OK) return;
    for (size_t i = 0; i < count; ++i) {
        if (httpd_ws_get_fd_info(server, client_fds[i]) !=
            HTTPD_WS_CLIENT_WEBSOCKET) continue;
        async_resp_arg* arg =
            (async_resp_arg*)calloc(1, sizeof(async_resp_arg));
        if (!arg) continue;
        arg->msg = (char*)malloc(length);
        if (!arg->msg) {
            free(arg);
            continue;
        }
        memcpy(arg->msg, data, length);
        arg->hd = server;
        arg->fd = client_fds[i];
        arg->length = length;
        arg->binary = true;
        if (httpd_queue_work(server, ws_async_send, arg) != ESP_OK) {
            free(arg->msg);
            free(arg);
        }
    }
}
