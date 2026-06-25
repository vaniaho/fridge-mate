#include "web_panel.h"
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <esp_log.h>
#include "api_routes.h"

static const char *TAG = "WebPanel";
static httpd_handle_t http_server = NULL;
static httpd_handle_t https_server = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");
extern const uint8_t server_cert_pem_start[]
    asm("_binary_server_cert_pem_start");
extern const uint8_t server_cert_pem_end[]
    asm("_binary_server_cert_pem_end");
extern const uint8_t server_key_pem_start[]
    asm("_binary_server_key_pem_start");
extern const uint8_t server_key_pem_end[]
    asm("_binary_server_key_pem_end");

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

static esp_err_t certificate_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/x-pem-file");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=smart-fridge-cert.pem");
    httpd_resp_send(
        req, (const char *)server_cert_pem_start,
        server_cert_pem_end - server_cert_pem_start - 1);
    return ESP_OK;
}

static esp_err_t favicon_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
static const httpd_uri_t uri_style = { .uri = "/style.css", .method = HTTP_GET, .handler = style_get_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
static const httpd_uri_t uri_app_js = { .uri = "/app.js", .method = HTTP_GET, .handler = app_js_get_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
static const httpd_uri_t uri_certificate = { .uri = "/device-cert.pem", .method = HTTP_GET, .handler = certificate_get_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };
static const httpd_uri_t uri_favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler, .user_ctx = NULL, .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = NULL };

static void register_routes(httpd_handle_t server) {
    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_style);
    httpd_register_uri_handler(server, &uri_app_js);
    httpd_register_uri_handler(server, &uri_certificate);
    httpd_register_uri_handler(server, &uri_favicon);
    register_api_routes(server);
}

extern "C" esp_err_t web_panel_start(void) {
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.max_uri_handlers = 28;
    http_config.stack_size = 16384;
    http_config.max_open_sockets = 3;
    http_config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d",
             http_config.server_port);
    if (httpd_start(&http_server, &http_config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting HTTP server");
        return ESP_FAIL;
    }
    register_routes(http_server);

    httpd_ssl_config_t https_config = HTTPD_SSL_CONFIG_DEFAULT();
    https_config.httpd.max_uri_handlers = 28;
    https_config.httpd.stack_size = 16384;
    https_config.httpd.max_open_sockets = 2;
    https_config.httpd.lru_purge_enable = true;
    https_config.servercert = server_cert_pem_start;
    https_config.servercert_len =
        server_cert_pem_end - server_cert_pem_start;
    https_config.prvtkey_pem = server_key_pem_start;
    https_config.prvtkey_len =
        server_key_pem_end - server_key_pem_start;

    ESP_LOGI(TAG, "Starting HTTPS server on port %d",
             https_config.port_secure);
    if (httpd_ssl_start(&https_server, &https_config) == ESP_OK) {
        register_routes(https_server);
        ESP_LOGI(TAG, "HTTPS voice capture endpoint ready");
    } else {
        ESP_LOGW(TAG,
                 "HTTPS server unavailable; browser microphone requires HTTPS");
    }
    return ESP_OK;
}

extern "C" void web_panel_broadcast_ws(const char* msg) {
    if (http_server) broadcast_ws_message(http_server, msg);
    if (https_server) broadcast_ws_message(https_server, msg);
}

extern "C" void web_panel_broadcast_ws_binary(const uint8_t* data,
                                                size_t length) {
    if (!data || !length) return;
    if (http_server) broadcast_ws_binary(http_server, data, length);
    if (https_server) broadcast_ws_binary(https_server, data, length);
}
