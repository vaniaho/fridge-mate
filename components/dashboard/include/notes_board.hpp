#pragma once

#include <time.h>
#include <string>
#include <vector>

namespace smart_fridge {
namespace dashboard {

/**
 * @brief 单条留言
 */
struct Note {
    time_t timestamp;   // 创建时间戳
    std::string text;   // 留言内容
};

/**
 * @brief 初始化留言板（从 SD 卡加载，SD 卡不可用时降级纯内存模式）
 * @return true 成功
 */
bool notes_init();

/**
 * @brief 获取所有留言（按时间倒序，最新在前）
 */
std::vector<Note> notes_get_all();

/**
 * @brief 添加一条留言（超出上限 MAX_NOTES 时自动丢弃最早的）
 * @return 新增留言的 timestamp，0 表示失败
 */
time_t notes_add(const std::string& text);

/**
 * @brief 按 timestamp 删除指定留言
 */
bool notes_delete(time_t timestamp);

/**
 * @brief 清空所有留言
 */
bool notes_clear();

/**
 * @brief SD 卡是否可用（用于提示持久化状态）
 */
bool notes_is_persistent();

} // namespace dashboard
} // namespace smart_fridge
