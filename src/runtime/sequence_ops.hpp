#pragma once
#include "diagnostic/failure.hpp"
#include "runtime/value.hpp"
#include <optional>

namespace myb {
class Heap;
class RootRegistry;
class RootedValue;
/**
 * @brief 执行 Array 与 String 的索引和浅切片，以及 Array 元素替换。
 * 借用同一会话的 Heap 与根表；服务须覆盖本对象寿命。
 * 结果引用由输出根保活，根注销后的回收或 Heap 清空使引用失效。
 */
class SequenceOps final {
  public:
    /** @brief 借用受管堆及其所属根表，不分配或回收对象。 */
    SequenceOps(Heap &heap, RootRegistry &roots) noexcept : heap_(heap), roots_(roots) {}
    /**
     * @brief 以严格 Int64 索引读取原序列的当前元素，String 按 Unicode 码点读取。
     * @param[in,out] out 同会话已登记的结果根，失败保留原值。
     * 输入先登记临时根；String 结果分配可触发 GC，成功后由 out 保活。
     * @pre 输入对象仍有效，接收对象与索引的求值已经完成。
     * @throws Failure 接收对象或索引类型错误、索引越界、根归属或资源错误。
     */
    void get_index(Value receiver, Value index, ErrorSite, RootedValue &out);
    /**
     * @brief 使用原序列当前长度处理端点，创建 Array 浅切片或 String 码点切片。
     * @param[in] start 省略时为空；显式 null 仍作为端点接受类型检查。
     * @param[in] end 省略时为空；端点先处理负数，再限制到有效边界。
     * @param[in,out] out 同会话已登记的结果根，失败保留原值。
     * 分配可触发 GC，接收对象由临时根保活，成功后由 out 保护新对象。
     * @pre 接收对象和所有显式端点已按源码顺序求值，输入对象仍有效。
     * @throws Failure 接收对象或端点类型错误、根归属或资源错误。
     */
    void get_slice(Value receiver, std::optional<Value> start, std::optional<Value> end,
                   ErrorSite, RootedValue &out);
    /**
     * @brief 检查 Array 与严格 Int64 索引后替换已有元素，不改变长度或触发 GC。
     * @pre 完整右值先于接收对象和索引求值，所有输入对象仍有效。
     * @throws Failure 接收对象或索引类型错误、索引越界。
     */
    void set_index(Value receiver, Value index, Value value, ErrorSite);

  private:
    /** @brief 验证输出槽登记于本会话根表，不分配或触发 GC。 */
    void check_output(const RootedValue &, ErrorSite) const;
    Heap &heap_;
    RootRegistry &roots_;
};
} // namespace myb
