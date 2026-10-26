#pragma once
#include "runtime/value.hpp"
#include <cstddef>
#include <cstdint>

namespace myb {
/** @brief 所属代码单元中遍历站点表的索引，帧身份另由记录保存。 */
using IterationSiteId = std::uint32_t;
/**
 * @brief 保存一条已提交活动遍历及其来源根，不持有值区或帧表元素地址。
 * VM 拥有记录表，并在根枚举时标记 source；表增长使记录借用失效。
 * 数组游标为元素位置，字符串游标为下一码点的字节偏移。
 * registered 表示本记录拥有一次数组计数；结束记录先解除计数，再撤销来源根。
 * 来源保护持续到正常出口、所属帧返回或错误清理；解除过程不分配或触发 GC。
 */
struct IterationRecord {
    std::size_t frame_index{};
    IterationSiteId site{};
    Value source;
    std::size_t cursor{};
    bool registered{};
};
} // namespace myb
