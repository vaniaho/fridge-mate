#include "ai_agent.hpp"
#include "system_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string>
#include <cstring>
#include <cstdlib>

static const char *TAG = "AIWorker";

namespace smart_fridge {
namespace ai {

// 异步 LLM 请求队列（常驻 worker task 使用）
static QueueHandle_t s_ai_request_queue = NULL;
static TaskHandle_t s_ai_worker_task = NULL;

#define AI_WORKER_QUEUE_LEN 4

struct ai_request_t {
    char* text;  // 由调用方 strdup 分配，worker 负责释放
};

/**
 * @brief AI Worker 后台任务
 * 从队列中取出语音文本，阻塞式调用大模型，避免卡死系统事件总线。
 * 调用成功时，parse_and_execute_llm_response() 内部会投递 EVT_LLM_RESPONSE_READY；
 * 调用失败时，本任务投递一个带错误提示的 EVT_LLM_RESPONSE_READY。
 */
static void ai_worker_task(void *pvParameters) {
    ai_request_t req;
    while (1) {
        if (xQueueReceive(s_ai_request_queue, &req, portMAX_DELAY) == pdTRUE) {
            if (!req.text) {
                continue;
            }

            ESP_LOGI(TAG, "Worker processing: %s", req.text);
            std::string input(req.text);
            free(req.text);
            req.text = NULL;

            std::string out_reply;
            bool ok = call_llm_api(input, out_reply);

            if (!ok) {
                ESP_LOGE(TAG, "LLM API call failed for: %s", input.c_str());
                llm_response_payload_t* payload = (llm_response_payload_t*)malloc(sizeof(llm_response_payload_t));
                if (payload) {
                    memset(payload, 0, sizeof(llm_response_payload_t));
                    strncpy(payload->tts_text,
                            "大模型调用失败，请检查网络或 API 配置",
                            TTS_TEXT_MAX_LEN - 1);
                    payload->ui_action_id = UI_ACTION_NONE;

                    if (send_system_event(EVT_LLM_RESPONSE_READY, payload, sizeof(llm_response_payload_t)) != ESP_OK) {
                        free(payload);
                    }
                }
            }
            // 调用成功时，parse_and_execute_llm_response() 已经投递了 EVT_LLM_RESPONSE_READY
        }
    }
}

/**
 * @brief 异步调用大模型
 * 将请求放入 worker 队列后立刻返回，不会阻塞调用者。
 * @return true 成功入队；false 队列满或内存不足
 */
bool call_llm_api_async(const std::string& user_voice_text) {
    if (s_ai_request_queue == NULL) {
        s_ai_request_queue = xQueueCreate(AI_WORKER_QUEUE_LEN, sizeof(ai_request_t));
        if (s_ai_request_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create AI request queue");
            return false;
        }
    }

    if (s_ai_worker_task == NULL) {
        BaseType_t ret = xTaskCreate(ai_worker_task,
                                     "ai_worker",
                                     12288,
                                     NULL,
                                     4,
                                     &s_ai_worker_task);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create AI worker task");
            return false;
        }
    }

    ai_request_t req;
    req.text = strdup(user_voice_text.c_str());
    if (req.text == NULL) {
        return false;
    }

    if (xQueueSend(s_ai_request_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "AI request queue full");
        free(req.text);
        return false;
    }

    return true;
}

} // namespace ai
} // namespace smart_fridge
