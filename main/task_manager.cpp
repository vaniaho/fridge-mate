#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "system_events.h"
#include "gui_bridge.h"
#include "ai_agent.hpp"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TaskManager";

// 全局系统事件队列句柄
static QueueHandle_t sys_event_queue = NULL;
#define SYS_EVENT_QUEUE_SIZE 20

/**
 * @brief 系统总线任务 (System Bus Task)
 * 作为系统的主轴，负责监听队列中的事件，并路由分发给对应的业务处理逻辑
 */
static void system_bus_task(void *pvParameters) {
    sys_event_t evt;
    ESP_LOGI(TAG, "System bus task started. Listening for events...");

    while (1) {
        // 阻塞等待队列中的事件
        if (xQueueReceive(sys_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Received event type: %d", evt.type);

            switch (evt.type) {
                case EVT_SYS_INIT_DONE:
                    ESP_LOGI(TAG, "[Event] System initialization complete. Ready for operations.");
                    break;

                case EVT_WAKE_WORD_DETECTED:
                    ESP_LOGI(TAG, "[Event] Wake word detected!");
                    gui_bridge_wake();
                    gui_bridge_show_listening_indicator(true);
                    break;

                case EVT_VOICE_CMD_RCVD:
                    if (evt.payload) {
                        voice_cmd_payload_t* voice_payload = (voice_cmd_payload_t*)evt.payload;
                        ESP_LOGI(TAG, "[Event] Voice command received: %s", voice_payload->command_text);
                        
                        // 关闭聆听指示器，交给云端大模型处理
                        gui_bridge_show_listening_indicator(false);
                        
                        // 调用 ai_agent 的 LLM API (阻塞式)
                        // 内部会自动解析响应、执行库存操作、投递 EVT_LLM_RESPONSE_READY
                        std::string voice_text(voice_payload->command_text);
                        std::string dummy_reply;
                        if (!smart_fridge::ai::call_llm_api(voice_text, dummy_reply)) {
                            ESP_LOGE(TAG, "LLM API call failed for: %s", voice_payload->command_text);
                            gui_bridge_show_notification("错误", "大模型调用失败，请检查网络连接");
                        }
                        
                        free(evt.payload);
                    }
                    break;

                case EVT_VISION_INFER_DONE:
                    if (evt.payload) {
                        vision_infer_payload_t* vision_payload = (vision_infer_payload_t*)evt.payload;
                        ESP_LOGI(TAG, "[Event] Vision inference done. Top1 ID: %d, Confidence: %.2f", 
                                 vision_payload->top1_id, vision_payload->confidence);
                        
                        // TODO (A): 待 A 模块提供类别 ID → 食材名的映射表后，
                        // 在此将识别结果转为 add_ingredient 调用
                        
                        free(evt.payload);
                    }
                    break;

                case EVT_LLM_RESPONSE_READY:
                    if (evt.payload) {
                        llm_response_payload_t* llm_payload = (llm_response_payload_t*)evt.payload;
                        ESP_LOGI(TAG, "[Event] LLM response ready. TTS: %s, UI Action: %d",
                                 llm_payload->tts_text, llm_payload->ui_action_id);
                        
                        // 通知 A 模块(GUI) 显示 TTS 文本
                        gui_bridge_show_tts_text(llm_payload->tts_text);
                        
                        // 根据 ui_action_id 执行差异化 GUI 操作
                        switch (llm_payload->ui_action_id) {
                            case UI_ACTION_REFRESH_LIST:
                                gui_bridge_refresh_inventory();
                                break;
                            case UI_ACTION_SHOW_RECIPE:
                                // TODO (A): 显示食谱推荐卡片，待 GUI 框架接入后实现
                                gui_bridge_show_notification("菜品推荐", "已为您推荐菜品，请查看屏幕");
                                break;
                            case UI_ACTION_SHOW_ALERT:
                                gui_bridge_show_notification("提醒", llm_payload->tts_text);
                                break;
                            default:
                                break;
                        }
                        
                        // TODO (B): 通知 B 模块进行语音播报 (当 audio_hal 就绪后接入)
                        // audio_api_play_tts(llm_payload->tts_text);
                        
                        free(evt.payload);
                    }
                    break;

                case EVT_INVENTORY_UPDATED:
                    ESP_LOGI(TAG, "[Event] Inventory updated. Requesting GUI refresh.");
                    gui_bridge_refresh_inventory();
                    break;

                case EVT_PIR_TRIGGERED:
                    ESP_LOGI(TAG, "[Event] PIR sensor triggered.");
                    gui_bridge_wake();
                    break;

                case EVT_GOTO_SLEEP:
                    ESP_LOGI(TAG, "[Event] Idle timeout reached. Going to sleep.");
                    gui_bridge_show_standby();
                    break;

                default:
                    ESP_LOGW(TAG, "Unknown event type: %d", evt.type);
                    if (evt.payload) free(evt.payload);
                    break;
            }
        }
    }
}

/**
 * @brief 初始化系统总线及任务调度
 */
extern "C" void task_manager_init(void) {
    // 1. 创建全局事件队列
    sys_event_queue = xQueueCreate(SYS_EVENT_QUEUE_SIZE, sizeof(sys_event_t));
    if (sys_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create system event queue");
        return;
    }

    // 2. 创建系统总线监听任务 (优先级设为 5，高于一般计算任务，确保事件及时响应)
    xTaskCreate(system_bus_task, "sys_bus_task", 8192, NULL, 5, NULL);
    
    // 3. 投递系统初始化完成事件
    sys_event_t init_evt;
    init_evt.type = EVT_SYS_INIT_DONE;
    init_evt.payload = NULL;
    init_evt.payload_size = 0;
    xQueueSend(sys_event_queue, &init_evt, 0);
}

/**
 * @brief 供各模块投递事件的统一接口
 * 
 * @param type 事件类型
 * @param payload 事件附加数据指针（需发送方使用 malloc 分配堆内存，总线处理完毕后自动 free）
 * @param payload_size 附加数据大小
 * @return esp_err_t ESP_OK 表示投递成功
 */
extern "C" esp_err_t send_system_event(sys_event_type_t type, void* payload, size_t payload_size) {
    if (sys_event_queue == NULL) return ESP_FAIL;

    sys_event_t evt;
    evt.type = type;
    evt.payload = payload; 
    evt.payload_size = payload_size;

    // 阻塞 10 个 Tick 来投递，如果队列满则失败
    if (xQueueSend(sys_event_queue, &evt, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send event %d to queue", type);
        // 为了防止内存泄漏，如果发送失败，由发送方或这里决定是否释放 payload。
        // 这里约定：通过此接口发送的指针，如果不成功，调用者自己释放；如果成功，总线释放。
        return ESP_FAIL;
    }
    return ESP_OK;
}
