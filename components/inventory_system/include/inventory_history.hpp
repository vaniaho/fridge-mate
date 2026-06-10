#pragma once

#include <stdint.h>
#include <time.h>
#include <string>
#include <vector>

namespace smart_fridge {
namespace inventory {

    // ======== 历史记录 ========

    /**
     * @brief 操作类型枚举
     */
    enum class HistoryAction {
        ADD,        // 存入
        REMOVE      // 取出
    };

    /**
     * @brief 单条操作历史记录
     */
    struct HistoryRecord {
        time_t timestamp;           // 操作时间戳
        HistoryAction action;       // ADD 或 REMOVE
        std::string item_name;      // 食材名称
        int quantity;               // 操作数量 (始终为正数)
    };

    // ======== 消耗速率 ========

    /**
     * @brief 某种食材的消耗速率统计结果
     */
    struct ConsumptionRate {
        std::string item_name;      // 食材名称
        float daily_rate;           // 日均消耗量 (过去 window_days 天)
        int total_consumed;         // 统计窗口内的总消耗量
        int window_days;            // 统计窗口天数
    };

    // ======== 历史记录接口 ========

    /**
     * @brief 初始化历史记录模块
     * 从 SD 卡加载 /sdcard/history.json，如果文件不存在则从空记录开始
     * @return true 初始化成功
     */
    bool history_init();

    /**
     * @brief 追加一条操作记录并持久化
     * 在每次 add_ingredient / remove_ingredient 后自动调用
     * @param action 操作类型
     * @param item_name 食材名称
     * @param quantity 操作数量
     */
    void history_append(HistoryAction action, const std::string& item_name, int quantity);

    /**
     * @brief 获取最近 N 天的所有操作记录
     * @param days 天数，0 表示返回全部
     * @return 时间倒序排列的记录列表
     */
    std::vector<HistoryRecord> history_get_recent(int days = 0);

    /**
     * @brief 获取某种食材最近 N 天的操作记录
     * @param item_name 食材名称
     * @param days 天数，0 表示返回全部
     * @return 时间倒序排列的记录列表
     */
    std::vector<HistoryRecord> history_get_by_item(const std::string& item_name, int days = 0);

    // ======== 消耗速率接口 ========

    /**
     * @brief 计算某种食材的消耗速率
     * 基于历史记录中的 REMOVE 事件，在指定天数窗口内统计
     * @param item_name 食材名称
     * @param window_days 统计窗口天数 (默认 7 天)
     * @return 消耗速率统计结果
     */
    ConsumptionRate compute_consumption_rate(const std::string& item_name, int window_days = 7);

    /**
     * @brief 计算所有有消耗记录的食材的消耗速率
     * @param window_days 统计窗口天数 (默认 7 天)
     * @return 按 daily_rate 降序排列的速率列表
     */
    std::vector<ConsumptionRate> compute_all_consumption_rates(int window_days = 7);

} // namespace inventory
} // namespace smart_fridge
