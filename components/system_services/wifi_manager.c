#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi_remote.h"
#include "esp_event.h"
#include "esp_log.h"
#include <string.h>

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     10

static const char *TAG = "WiFiHelper";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_wifi_manager_retry_enabled = true;
static esp_event_handler_instance_t s_instance_any_id = NULL;
static esp_event_handler_instance_t s_instance_got_ip = NULL;

void wifi_manager_disable_retry(void) {
    s_wifi_manager_retry_enabled = false;
    ESP_LOGI(TAG, "WiFi manager retry disabled, SystemManager will take over.");
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_config_t cfg;
        esp_wifi_remote_get_config(WIFI_IF_STA, &cfg);
        if (strlen((char*)cfg.sta.ssid) > 0) {
            esp_wifi_remote_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_manager_retry_enabled && s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_remote_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP via Co-Processor (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Failed to connect after %d retries or retry disabled", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool wifi_init_sta(const char* ssid, const char* pass) {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { ESP_ERROR_CHECK(err); }
    
    // 初始化默认的虚拟 STA 接口
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    // 初始化 ESP-Hosted 驱动 (底层会配置 SDIO 引脚并唤醒 C6)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_remote_init(&cfg));

    // 使用原生的 WIFI_EVENT，底层会被宏定义自动接管
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &s_instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &s_instance_got_ip));

    if (ssid && strlen(ssid) > 0) {
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config));
    }

    // 将后续的指令通过 SDIO 透传给 C6 协处理器
    ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_remote_start());

    if (ssid && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Hosted WiFi init finished. Waiting for C6 to connect to AP...");
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "successfully connected to ap SSID:%s via C6 Co-Processor", ssid);
            return true;
        } else {
            ESP_LOGE(TAG, "Failed to connect to SSID:%s", ssid);
            return false;
        }
    } else {
        ESP_LOGI(TAG, "Hosted WiFi started in idle mode. Waiting for user configuration...");
        return false;
    }
}

void wifi_manager_unregister_handlers(void) {
    if (s_instance_any_id) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_instance_any_id);
        s_instance_any_id = NULL;
        ESP_LOGI(TAG, "Unregistered boot WiFi event handler (any_id)");
    }
    if (s_instance_got_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_instance_got_ip);
        s_instance_got_ip = NULL;
        ESP_LOGI(TAG, "Unregistered boot WiFi event handler (got_ip)");
    }
}
