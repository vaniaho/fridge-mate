#include "api_routes.h"
#include <cJSON.h>
#include <esp_log.h>
#include <string.h>
#include "inventory.hpp"

static const char *TAG = "WebPanelApi";

#include "inventory_history.hpp"
#include "credentials_manager.h"
#include "ai_agent.hpp"
#include "esp_system.h"
#include "esp_wifi.h"

// GET /api/inventory
static esp_err_t get_inventory_handler(httpd_req_t *req) {
    auto items = smart_fridge::inventory::get_all_ingredients();
    cJSON *root = cJSON_CreateArray();
    for (const auto& item : items) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", item.id);
        cJSON_AddStringToObject(obj, "name", item.name.c_str());
        cJSON_AddStringToObject(obj, "category", item.category.c_str());
        cJSON_AddNumberToObject(obj, "quantity", item.quantity);
        cJSON_AddNumberToObject(obj, "expire_days", item.expire_days);
        cJSON_AddNumberToObject(obj, "entry_time", item.entry_time);
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
    char buf[256];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
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
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (action && action->valuestring) {
        if (strcmp(action->valuestring, "add") == 0) {
            cJSON *name = cJSON_GetObjectItem(root, "name");
            cJSON *cat = cJSON_GetObjectItem(root, "category");
            cJSON *qty = cJSON_GetObjectItem(root, "quantity");
            cJSON *exp = cJSON_GetObjectItem(root, "expire_days");
            if (name && cat && qty && exp) {
                smart_fridge::inventory::add_ingredient(name->valuestring, cat->valuestring, qty->valueint, exp->valueint);
            }
        } else if (strcmp(action->valuestring, "remove") == 0) {
            cJSON *name = cJSON_GetObjectItem(root, "name");
            cJSON *qty = cJSON_GetObjectItem(root, "quantity");
            if (name && qty) {
                smart_fridge::inventory::remove_ingredient(name->valuestring, qty->valueint);
            }
        } else if (strcmp(action->valuestring, "update") == 0) {
            cJSON *name = cJSON_GetObjectItem(root, "name");
            cJSON *qty = cJSON_GetObjectItem(root, "quantity");
            cJSON *cat = cJSON_GetObjectItem(root, "category");
            cJSON *exp = cJSON_GetObjectItem(root, "expire_days");
            cJSON *ent = cJSON_GetObjectItem(root, "entry_time");
            if (name && qty && cat && exp && ent) {
                smart_fridge::inventory::update_ingredient(
                    name->valuestring,
                    qty->valueint,
                    cat->valuestring,
                    exp->valueint,
                    (time_t)ent->valuedouble
                );
            }
        }
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    
    return ESP_OK;
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
    cJSON_AddStringToObject(root, "llm_key", "sk-********"); // Mask key
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
    if (url && key && strlen(key->valuestring) > 0 && strcmp(key->valuestring, "sk-********") != 0) {
        credentials_set_llm_api(url->valuestring, key->valuestring);
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
        std::string reply;
        bool success = smart_fridge::ai::call_llm_api(text->valuestring, reply);
        cJSON_Delete(root);
        if (success) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "reply", reply.c_str());
            const char *json_resp = cJSON_PrintUnformatted(resp);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_resp, strlen(json_resp));
            free((void*)json_resp);
            cJSON_Delete(resp);
            return ESP_OK;
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "LLM processing failed");
            return ESP_FAIL;
        }
    }
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing text");
    return ESP_FAIL;
}

// WS Handler
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // Read frame to clear buffer and avoid infinite loop
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (ws_pkt.len > 0) {
        uint8_t *buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
        if (buf) {
            ws_pkt.payload = buf;
            httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            free(buf);
        }
    }
    return ESP_OK;
}

void register_api_routes(httpd_handle_t server) {
    httpd_uri_t uri_get_inv = { .uri = "/api/inventory", .method = HTTP_GET, .handler = get_inventory_handler, .user_ctx = NULL };
    httpd_uri_t uri_post_inv = { .uri = "/api/inventory", .method = HTTP_POST, .handler = post_inventory_handler, .user_ctx = NULL };
    httpd_uri_t uri_get_hist = { .uri = "/api/history", .method = HTTP_GET, .handler = get_history_handler, .user_ctx = NULL };
    httpd_uri_t uri_get_status = { .uri = "/api/status", .method = HTTP_GET, .handler = get_status_handler, .user_ctx = NULL };
    httpd_uri_t uri_get_set = { .uri = "/api/settings", .method = HTTP_GET, .handler = get_settings_handler, .user_ctx = NULL };
    httpd_uri_t uri_post_set = { .uri = "/api/settings", .method = HTTP_POST, .handler = post_settings_handler, .user_ctx = NULL };
    httpd_uri_t uri_post_chat = { .uri = "/api/chat", .method = HTTP_POST, .handler = post_chat_handler, .user_ctx = NULL };
    httpd_uri_t uri_ws = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .user_ctx = NULL, .is_websocket = true };

    httpd_register_uri_handler(server, &uri_get_inv);
    httpd_register_uri_handler(server, &uri_post_inv);
    httpd_register_uri_handler(server, &uri_get_hist);
    httpd_register_uri_handler(server, &uri_get_status);
    httpd_register_uri_handler(server, &uri_get_set);
    httpd_register_uri_handler(server, &uri_post_set);
    httpd_register_uri_handler(server, &uri_post_chat);
    httpd_register_uri_handler(server, &uri_ws);
}

struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
    char *msg;  // 使用 strdup 分配的独立副本，发送后需 free
};

static void ws_async_send(void *arg) {
    struct async_resp_arg *resp_arg = (struct async_resp_arg *)arg;
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)resp_arg->msg;
    ws_pkt.len = strlen(resp_arg->msg);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

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
                        httpd_queue_work(server, ws_async_send, arg);
                    } else {
                        free(arg);
                    }
                }
            }
        }
    }
}
