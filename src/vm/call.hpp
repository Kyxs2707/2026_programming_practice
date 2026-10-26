#pragma once
#include "source/span.hpp"
#include <cstddef>
#include <string>
namespace myb {
/** @brief 拥有一次真实活动函数的名称及错误时该帧执行位置，不持有受管对象。 */
struct CallTraceEntry {
    std::string name;
    SourceSpan span;
};
/**
 * @brief 检查 VM 容量的加法，溢出或超过上限时报告受控资源错误。
 * @return 不超过 limit 的 left + right。
 */
std::size_t checked_region_end(std::size_t left, std::size_t right, std::size_t limit,
                               class EmergencyFailure &first);
} // namespace myb
