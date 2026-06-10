#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "inventory.hpp"
#include "ai_agent.hpp"
#include "credentials_manager.h"
#include "wifi_manager.h"
#include "web_panel.h"

// 声明 task_manager 暴露出来的初始化函数
extern "C" void task_manager_init(void);

static const char *TAG = "MainTest";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting Smart Fridge C-Module Test   ");
    ESP_LOGI(TAG, "========================================");

    // ----------------------------------------------------
    // 0. 初始化 NVS 和凭证管理器
    // ----------------------------------------------------
    ESP_LOGI(TAG, "\n--- 0. Initializing NVS & Credentials ---");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(credentials_init());
    ESP_LOGI(TAG, "==> Credentials loaded. WiFi SSID: %s, LLM Model: %s",
             credentials_get_wifi_ssid(), credentials_get_llm_model());

    // ----------------------------------------------------
    // 1. 启动系统事件总线
    // ----------------------------------------------------
    ESP_LOGI(TAG, "\n--- 1. Testing System Bus ---");
    task_manager_init();
    vTaskDelay(pdMS_TO_TICKS(500)); // 稍微等待总线任务启动就绪

    // ----------------------------------------------------
    // 2. 测试本地食材管理系统 (Inventory System)
    // ----------------------------------------------------
    ESP_LOGI(TAG, "\n--- 2. Testing Inventory System ---");
    if (smart_fridge::inventory::init_database()) {
        ESP_LOGI(TAG, "==> Inventory DB initialized successfully.");
        
        // 模拟：存入 2 瓶牛奶，保质期 7 天
        smart_fridge::inventory::add_ingredient("牛奶", "饮品", 2, 7);
        // 模拟：存入 5 个西红柿，保质期 5 天
        smart_fridge::inventory::add_ingredient("西红柿", "蔬菜", 5, 5);
        
        // 模拟：取出了 2 个西红柿
        smart_fridge::inventory::remove_ingredient("西红柿", 2);

        // 获取并打印当前库存清单，验证 CRUD 是否正确
        auto items = smart_fridge::inventory::get_all_ingredients();
        ESP_LOGI(TAG, "==> Current Inventory List:");
        for (const auto& item : items) {
            ESP_LOGI(TAG, "  - [%d] %s: 数量 %d (分类: %s, 保质期 %d 天)", 
                     item.id, item.name.c_str(), item.quantity, item.category.c_str(), item.expire_days);
        }

        // 测试临期检查功能 (阈值设为 <= 7 天)
        auto expiring = smart_fridge::inventory::check_expiring_ingredients(7);
        ESP_LOGI(TAG, "==> Items expiring within 7 days: %d", expiring.size());

    } else {
        ESP_LOGE(TAG, "==> Failed to initialize Inventory DB.");
    }

    // ----------------------------------------------------
    // 3. 测试网络和云端大模型中枢 (AI Agent)
    // ----------------------------------------------------
    ESP_LOGI(TAG, "\n--- 3. Testing Network & AI Agent ---");
    
    const char* wifi_ssid = credentials_get_wifi_ssid();
    const char* wifi_pass = credentials_get_wifi_pass();
    
    if (strlen(wifi_ssid) == 0) {
        ESP_LOGW(TAG, "WiFi SSID is empty! Configure via 'idf.py menuconfig' -> Smart Fridge Configuration.");
        ESP_LOGW(TAG, "Skipping network tests.");
    } else {
        ESP_LOGW(TAG, "Connecting to WiFi: %s ...", wifi_ssid);
        // 连接 WiFi，此函数是阻塞的，连不上会一直等
        wifi_init_sta(wifi_ssid, wifi_pass);
        
        ESP_LOGI(TAG, "==> WiFi Connected. Starting Web Panel...");
        web_panel_start();

        ESP_LOGI(TAG, "==> Now testing LLM API...");
        std::string dummy_reply;
        bool llm_success = smart_fridge::ai::call_llm_api("我刚买了一把香蕉，帮我放进冰箱，大概5个。", dummy_reply);
        
        if (llm_success) {
            ESP_LOGI(TAG, "==> LLM API Call Success! Let's check inventory again.");
            auto items_after = smart_fridge::inventory::get_all_ingredients();
            for (const auto& item : items_after) {
                ESP_LOGI(TAG, "  - [%d] %s: 数量 %d", item.id, item.name.c_str(), item.quantity);
            }
        } else {
            ESP_LOGE(TAG, "==> LLM API Call Failed. Configure API Key via 'idf.py menuconfig' or GUI.");
        }
    }

    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "       Test sequence completed.         ");
    ESP_LOGI(TAG, "========================================");

    // 主循环保持运行，防止 app_main 退出
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
