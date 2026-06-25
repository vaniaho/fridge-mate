#pragma once
#include <esp_http_server.h>

void register_api_routes(httpd_handle_t server);
void broadcast_ws_message(httpd_handle_t server, const char* msg);
void broadcast_ws_binary(httpd_handle_t server, const uint8_t* data,
                         size_t length);
