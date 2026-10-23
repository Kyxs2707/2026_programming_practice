#pragma once
#include "diagnostic/failure.hpp"
#include "runtime/value.hpp"
#include <vector>
namespace myb {
/**
 * @brief 拥有按索引访问的 VM 值区和平行绑定状态，不自行登记根。
 * VM 依据活动帧枚举根；预留容量及未发布区域均不决定可达性。
 * ensure 可能移动宿主数组，调用方不得跨调用保存元素地址或视图。
 */
class ValueStore final {
  public:
    /**
     * @brief 同时准备所需值和状态，至少预留64项，之后按1.5倍增长。
     * @param[in] required 所需槽位总数。
     * @param[in] site 容量或分配失败的归属位置。
     * @throws Failure 容量或内存不足；旧有效槽与状态仍可按索引扫描。
     * 两个 reserve 均成功后才提交新的可用范围，不触发 MYB GC。
     */
    void ensure(std::size_t required, ErrorSite site);
    /** @brief 借用已准备槽位；下一次 ensure 增长可能使引用失效。 */
    Value &value(std::size_t index) noexcept {
        return values_[index];
    }
    const Value &value(std::size_t index) const noexcept {
        return values_[index];
    }
    /** @brief 借用已准备槽位的绑定状态，操作数不依赖该状态。 */
    BindingState &state(std::size_t index) noexcept {
        return states_[index];
    }
    BindingState state(std::size_t index) const noexcept {
        return states_[index];
    }
    /** @brief 返回两个数组均已准备完成的槽位数量。 */
    std::size_t size() const noexcept {
        return values_.size();
    }
    /** @brief 清除指定半开区间的值与绑定状态，不分配或触发 GC。 */
    void clear(std::size_t begin, std::size_t end) noexcept;

  private:
    std::vector<Value> values_;
    std::vector<BindingState> states_;
};
} // namespace myb
