#include "web_panel.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include "api_routes.h"

static const char *TAG = "WebPanel";
static httpd_handle_t server = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start - 1);
    return ESP_OK;
}

static esp_err_t style_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)style_css_start, style_css_end - style_css_start - 1);
    return ESP_OK;
}

static esp_err_t app_js_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)app_js_start, app_js_end - app_js_start - 1);
    return ESP_OK;
}

static const httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
static const httpd_uri_t uri_style = { .uri = "/style.css", .method = HTTP_GET, .handler = style_get_handler, .user_ctx = NULL };
static const httpd_uri_t uri_app_js = { .uri = "/app.js", .method = HTTP_GET, .handler = app_js_get_handler, .user_ctx = NULL };

extern "C" esp_err_t web_panel_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 16384; // Increase stack size to accommodate LLM HTTP client and JSON parsing

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &uri_index);
        httpd_register_uri_handler(server, &uri_style);
        httpd_register_uri_handler(server, &uri_app_js);
        register_api_routes(server);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return ESP_FAIL;
}

extern "C" void web_panel_broadcast_ws(const char* msg) {
    if (server) {
        broadcast_ws_message(server, msg);
    }
}
