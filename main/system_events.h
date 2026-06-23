#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 系统全局事件类型
 * 用于在A、B、C各开发者的模块间进行通信，解耦系统模块
 */
typedef enum {
    EVT_SYS_INIT_DONE,         // 系统初始化完成 (C触发)
    EVT_WAKE_WORD_DETECTED,    // 检测到语音唤醒词 (B -> C/A)
    EVT_VOICE_CMD_RCVD,        // 收到完整的语音指令 (B -> C)
    EVT_VISION_INFER_DONE,     // 图像识别完成 (A -> C)
    EVT_LLM_RESPONSE_READY,    // 大模型处理完成，需播报或更新UI (C -> A/B)
    EVT_INVENTORY_UPDATED,     // 食材库存已更新 (C -> A)
    EVT_PIR_TRIGGERED,         // 人体红外检测触发 (C -> A/系统)
    EVT_GOTO_SLEEP             // 触发休眠模式 (C -> 全局)
} sys_event_type_t;

/**
 * @brief 通用系统事件结构体
 * 投递至系统总线队列的格式
 */
typedef struct {
    sys_event_type_t type;
    void* payload;             // 指向具体事件数据的指针 (需发送方动态分配，接收方释放)
    size_t payload_size;       // 数据大小
} sys_event_t;

// ======== 具体事件的 Payload 结构体定义 (供各模块参考) ========

// EVT_VOICE_CMD_RCVD 携带的 payload
typedef struct {
    char command_text[256];    // 识别出的语音文本
} voice_cmd_payload_t;

// EVT_VISION_INFER_DONE 携带的 payload
typedef struct {
    int top1_id;               // 识别出的最高置信度类别ID
    float confidence;          // 置信度 (0~1.0)
} vision_infer_payload_t;

// EVT_LLM_RESPONSE_READY 携带的 payload
#define TTS_TEXT_MAX_LEN 2048
typedef struct {
    char tts_text[TTS_TEXT_MAX_LEN];  // 需要语音播报的文本
    int ui_action_id;                 // 需要UI执行的动作ID (由C解析后给出)
} llm_response_payload_t;

// UI 动作 ID 常量（ui_action_id 取值）
#define UI_ACTION_NONE          0   // 无特殊 UI 操作
#define UI_ACTION_SHOW_RECIPE   1   // 显示食谱推荐卡片
#define UI_ACTION_REFRESH_LIST  2   // 刷新食材列表
#define UI_ACTION_SHOW_ALERT    3   // 显示临期/过期警告

// ======== 系统事件投递接口 (定义在 task_manager.cpp 中) ========

/**
 * @brief 向系统事件队列投递一个事件
 * @param type 事件类型
 * @param payload 事件附加数据指针 (需 malloc 分配，总线处理完毕后自动 free)
 * @param payload_size 附加数据大小
 * @return ESP_OK 成功, ESP_FAIL 失败 (队列满或未初始化)
 */
esp_err_t send_system_event(sys_event_type_t type, void* payload, size_t payload_size);

#ifdef __cplusplus
}
#endif

