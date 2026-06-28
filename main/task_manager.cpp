#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "system_events.h"
#include "gui_bridge.h"
#include "gui_app.h"
#include "audio_api.h"
#include "ai_agent.hpp"
#include "web_panel.h"
#include "system_manager.hpp"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TaskManager";

// 全局系统事件队列句柄
static QueueHandle_t sys_event_queue = NULL;
#define SYS_EVENT_QUEUE_SIZE 40

static bool s_voice_session_active = false;
static bool s_voice_external_input = false;
static bool s_voice_auto_relisten_after_tts = false;
static int s_voice_followup_timeout_ms = 0;

static void broadcast_voice_json(const char* type, const char* state,
                                 const char* text) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", type ? type : "voice.event");
    if (state) cJSON_AddStringToObject(root, "state", state);
    if (text) cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(
        root, "mode",
        audio_hal_get_voice_mode() == AUDIO_VOICE_MODE_REALTIME
            ? "realtime" : "cascade");
    const char* json = cJSON_PrintUnformatted(root);
    if (json) {
        web_panel_broadcast_ws(json);
        free((void*)json);
    }
    cJSON_Delete(root);
}

static void set_voice_state(voice_assist_state_t state, const char* text) {
    gui_bridge_voice_set_state((int)state, text);
    const char* state_name = "idle";
    if (state == VOICE_STATE_LISTENING) state_name = "listening";
    else if (state == VOICE_STATE_THINKING) state_name = "thinking";
    else if (state == VOICE_STATE_SPEAKING) state_name = "speaking";
    broadcast_voice_json("voice.state", state_name, text);
}

static esp_err_t start_voice_capture(bool external_input) {
    s_voice_session_active = true;
    s_voice_external_input = external_input;
    s_voice_auto_relisten_after_tts = false;
    smart_fridge::ai::cancel_llm_api();
    audio_hal_interrupt();
    gui_bridge_wake();
    gui_bridge_show_voice_assist();
    gui_bridge_show_listening_indicator(true);
    set_voice_state(VOICE_STATE_LISTENING, external_input
        ? "正在接收浏览器麦克风..." : "正在聆听...");
    const esp_err_t result =
        external_input ? audio_hal_start_external_listening()
                       : audio_hal_start_listening();
    if (result != ESP_OK) {
        s_voice_session_active = false;
        gui_bridge_show_listening_indicator(false);
        set_voice_state(VOICE_STATE_IDLE, "语音输入启动失败");
        broadcast_voice_json("voice.error", "idle",
                             "语音输入启动失败");
    }
    return result;
}

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
                    start_voice_capture(false);
                    break;

                case EVT_VOICE_SESSION_START:
                    start_voice_capture(false);
                    break;

                case EVT_VOICE_SESSION_START_EXTERNAL:
                    start_voice_capture(true);
                    break;

                case EVT_VOICE_SESSION_STOP:
                    s_voice_session_active = false;
                    s_voice_auto_relisten_after_tts = false;
                    s_voice_followup_timeout_ms = 0;
                    smart_fridge::ai::cancel_llm_api();
                    audio_hal_interrupt();
                    gui_bridge_show_listening_indicator(false);
                    set_voice_state(VOICE_STATE_IDLE, "语音会话已结束");
                    break;

                case EVT_VOICE_SESSION_INTERRUPT:
                    s_voice_auto_relisten_after_tts = false;
                    s_voice_followup_timeout_ms = 0;
                    smart_fridge::ai::cancel_llm_api();
                    audio_hal_interrupt();
                    broadcast_voice_json("voice.interrupted", "listening",
                                         "已打断");
                    if (s_voice_session_active) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                        start_voice_capture(s_voice_external_input);
                    }
                    break;

                case EVT_VOICE_MODE_SET:
                    if (evt.payload) {
                        int mode = *(int*)evt.payload;
                        free(evt.payload);
                        smart_fridge::ai::cancel_llm_api();
                        audio_hal_interrupt();
                        vTaskDelay(pdMS_TO_TICKS(100));
                        if (audio_hal_set_voice_mode(
                                mode == AUDIO_VOICE_MODE_REALTIME
                                    ? AUDIO_VOICE_MODE_REALTIME
                                    : AUDIO_VOICE_MODE_CASCADE) == ESP_OK &&
                            s_voice_session_active) {
                            start_voice_capture(s_voice_external_input);
                        }
                    }
                    break;

                case EVT_VOICE_TEXT_INPUT:
                    if (evt.payload) {
                        auto* text_payload =
                            (llm_stream_text_payload_t*)evt.payload;
                        gui_bridge_show_voice_assist();
                        gui_bridge_voice_add_message(text_payload->text, true);
                        broadcast_voice_json("voice.user_text", "thinking",
                                             text_payload->text);
                        set_voice_state(VOICE_STATE_THINKING, "正在思考...");
                        if (!smart_fridge::ai::call_llm_api_async(
                                text_payload->text)) {
                            gui_bridge_show_notification(
                                "错误", "大模型请求入队失败");
                        }
                        free(evt.payload);
                    }
                    break;

                case EVT_AUDIO_HAL_EVENT:
                    if (evt.payload) {
                        auto* audio_evt =
                            (audio_hal_event_payload_t*)evt.payload;
                        switch ((audio_hal_event_t)audio_evt->event) {
                            case AUDIO_EVT_LISTENING_START:
                                set_voice_state(VOICE_STATE_LISTENING,
                                                "正在聆听...");
                                break;
                            case AUDIO_EVT_LISTENING_STOP:
                                gui_bridge_show_listening_indicator(false);
                                break;
                            case AUDIO_EVT_ASR_PARTIAL:
                                set_voice_state(VOICE_STATE_LISTENING,
                                                audio_evt->text);
                                broadcast_voice_json("voice.asr.partial",
                                                     "listening",
                                                     audio_evt->text);
                                break;
                            case AUDIO_EVT_ASR_RESULT:
                                gui_bridge_voice_add_message(audio_evt->text,
                                                             true);
                                set_voice_state(VOICE_STATE_THINKING,
                                                "正在思考...");
                                broadcast_voice_json("voice.asr.final",
                                                     "thinking",
                                                     audio_evt->text);
                                break;
                            case AUDIO_EVT_TTS_START:
                                set_voice_state(VOICE_STATE_SPEAKING,
                                                audio_evt->text);
                                break;
                            case AUDIO_EVT_TTS_DONE:
                                set_voice_state(VOICE_STATE_IDLE,
                                                "播报完成");
                                break;
                            case AUDIO_EVT_TTS_INTERRUPTED:
                                set_voice_state(VOICE_STATE_LISTENING,
                                                "已打断，继续聆听...");
                                break;
                            case AUDIO_EVT_REALTIME_TEXT:
                                gui_bridge_show_tts_text(audio_evt->text);
                                set_voice_state(VOICE_STATE_SPEAKING,
                                                audio_evt->text);
                                broadcast_voice_json("voice.assistant.delta",
                                                     "speaking",
                                                     audio_evt->text);
                                break;
                            case AUDIO_EVT_REALTIME_TURN_DONE:
                                if (audio_evt->text[0]) {
                                    gui_bridge_voice_add_message(
                                        audio_evt->text, false);
                                    broadcast_voice_json(
                                        "voice.assistant.final",
                                        s_voice_external_input
                                            ? "idle" : "listening",
                                        audio_evt->text);
                                }
                                set_voice_state(
                                    s_voice_external_input
                                        ? VOICE_STATE_IDLE
                                        : VOICE_STATE_LISTENING,
                                    s_voice_external_input
                                        ? "按麦克风继续"
                                        : "请继续说...");
                                break;
                            case AUDIO_EVT_ASR_ERROR:
                            case AUDIO_EVT_TTS_ERROR:
                                s_voice_session_active = false;
                                s_voice_auto_relisten_after_tts = false;
                                s_voice_followup_timeout_ms = 0;
                                set_voice_state(VOICE_STATE_IDLE,
                                                audio_evt->text);
                                broadcast_voice_json("voice.error", "idle",
                                                     audio_evt->text);
                                break;
                            default:
                                break;
                        }
                        free(evt.payload);
                    }
                    break;

                case EVT_VOICE_CMD_RCVD:
                    if (evt.payload) {
                        voice_cmd_payload_t* voice_payload = (voice_cmd_payload_t*)evt.payload;
                        ESP_LOGI(TAG, "[Event] Voice command received: %s", voice_payload->command_text);
                        s_voice_auto_relisten_after_tts = false;
                        s_voice_followup_timeout_ms = 0;
                        
                        // 关闭聆听指示器，交给云端大模型异步处理
                        gui_bridge_show_listening_indicator(false);
                        
                        // 将 LLM 调用投递到独立 worker task，避免阻塞系统事件总线
                        // worker 内部会自动解析响应、执行库存操作、投递 EVT_LLM_RESPONSE_READY
                        std::string voice_text(voice_payload->command_text);
                        set_voice_state(VOICE_STATE_THINKING, "正在思考...");
                        if (!smart_fridge::ai::call_llm_api_async(voice_text)) {
                            ESP_LOGE(TAG, "Failed to enqueue LLM request for: %s", voice_payload->command_text);
                            gui_bridge_show_notification("错误", "大模型请求入队失败");
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

                case EVT_LLM_STREAM_TEXT:
                    if (evt.payload) {
                        auto* stream_payload =
                            (llm_stream_text_payload_t*)evt.payload;
                        gui_bridge_show_tts_text(stream_payload->text);
                        broadcast_voice_json("voice.assistant.delta",
                                             "speaking",
                                             stream_payload->text);
                        free(evt.payload);
                    }
                    break;

                case EVT_TTS_STREAM_BEGIN:
                    if (audio_hal_tts_begin_response() != ESP_OK) {
                        ESP_LOGW(TAG, "Unable to start streamed TTS response");
                    }
                    break;

                case EVT_TTS_STREAM_SENTENCE:
                    if (evt.payload) {
                        auto* sentence =
                            (llm_stream_text_payload_t*)evt.payload;
                        if (audio_hal_tts_enqueue_sentence(sentence->text) !=
                            ESP_OK) {
                            ESP_LOGW(TAG, "Unable to enqueue TTS sentence");
                        }
                        free(evt.payload);
                    }
                    break;

                case EVT_TTS_STREAM_END:
                    if (audio_hal_tts_end_response() != ESP_OK) {
                        ESP_LOGW(TAG, "Unable to finish streamed TTS response");
                    }
                    break;

                case EVT_TTS_STREAM_CANCEL:
                    audio_hal_stop_tts();
                    break;

                case EVT_LLM_RESPONSE_READY:
                    if (evt.payload) {
                        llm_response_payload_t* llm_payload = (llm_response_payload_t*)evt.payload;
                        ESP_LOGI(TAG, "[Event] LLM response ready. TTS: %s, UI Action: %d",
                                 llm_payload->tts_text, llm_payload->ui_action_id);
                        const int continuous_ms =
                            smart_fridge::system::SystemManager::
                                get_voice_continuous_ms();
                        s_voice_followup_timeout_ms =
                            llm_payload->keep_listening
                                ? (continuous_ms > 0 ? continuous_ms : 10000)
                                : continuous_ms;
                        s_voice_auto_relisten_after_tts =
                            s_voice_session_active &&
                            !s_voice_external_input &&
                            s_voice_followup_timeout_ms > 0;
                        
                        // 通知 A 模块(GUI) 显示 TTS 文本
                        gui_bridge_show_tts_text(llm_payload->tts_text);
                        gui_bridge_voice_add_message(llm_payload->tts_text,
                                                     false);
                        broadcast_voice_json("voice.assistant.final",
                                             "speaking",
                                             llm_payload->tts_text);
                        
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
                        
                        // 通知 B 模块（audio_hal）进行语音播报
                        if (!llm_payload->tts_already_queued) {
                            audio_hal_play_tts(llm_payload->tts_text);
                        }
                        
                        free(evt.payload);
                    }
                    break;

                case EVT_TTS_PLAY_DONE:
                    ESP_LOGI(TAG, "[Event] TTS playback done");
                    // TTS 播报完成后可关闭语音指示器或返回待机
                    gui_bridge_show_listening_indicator(false);
                    if (s_voice_auto_relisten_after_tts) {
                        s_voice_auto_relisten_after_tts = false;
                        if (s_voice_followup_timeout_ms <= 0) {
                            s_voice_followup_timeout_ms = 10000;
                        }
                        audio_hal_set_next_listening_timeout_ms(
                            s_voice_followup_timeout_ms);
                        s_voice_followup_timeout_ms = 0;
                        gui_bridge_show_listening_indicator(true);
                        set_voice_state(VOICE_STATE_LISTENING,
                                        "请继续说...");
                        vTaskDelay(pdMS_TO_TICKS(100));
                        audio_hal_start_listening();
                    } else {
                        s_voice_session_active = false;
                        s_voice_auto_relisten_after_tts = false;
                        s_voice_followup_timeout_ms = 0;
                        set_voice_state(VOICE_STATE_IDLE, "语音会话已结束");
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

extern "C" esp_err_t voice_session_start(bool external_input) {
    return send_system_event(
        external_input ? EVT_VOICE_SESSION_START_EXTERNAL
                       : EVT_VOICE_SESSION_START,
        NULL, 0);
}

extern "C" esp_err_t voice_session_stop(void) {
    return send_system_event(EVT_VOICE_SESSION_STOP, NULL, 0);
}

extern "C" esp_err_t voice_session_interrupt(void) {
    return send_system_event(EVT_VOICE_SESSION_INTERRUPT, NULL, 0);
}

extern "C" esp_err_t voice_session_set_mode(int mode) {
    int* payload = (int*)malloc(sizeof(int));
    if (!payload) return ESP_ERR_NO_MEM;
    *payload = mode;
    esp_err_t err = send_system_event(EVT_VOICE_MODE_SET, payload,
                                      sizeof(*payload));
    if (err != ESP_OK) free(payload);
    return err;
}

extern "C" esp_err_t voice_session_submit_text(const char* text) {
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    auto* payload = (llm_stream_text_payload_t*)calloc(
        1, sizeof(llm_stream_text_payload_t));
    if (!payload) return ESP_ERR_NO_MEM;
    strncpy(payload->text, text, sizeof(payload->text) - 1);
    esp_err_t err = send_system_event(EVT_VOICE_TEXT_INPUT, payload,
                                      sizeof(*payload));
    if (err != ESP_OK) free(payload);
    return err;
}

extern "C" esp_err_t voice_session_report_audio_event(
    int event, const char* text) {
    auto* payload = (audio_hal_event_payload_t*)calloc(
        1, sizeof(audio_hal_event_payload_t));
    if (!payload) return ESP_ERR_NO_MEM;
    payload->event = event;
    if (text) strncpy(payload->text, text, sizeof(payload->text) - 1);
    esp_err_t err = send_system_event(EVT_AUDIO_HAL_EVENT, payload,
                                      sizeof(*payload));
    if (err != ESP_OK) free(payload);
    return err;
}
