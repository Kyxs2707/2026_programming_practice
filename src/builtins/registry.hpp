#pragma once
#include "platform/host_io.hpp"
#include "runtime/heap.hpp"
#include <cstdint>
#include <span>
#include <string_view>
namespace myb {
/**
 * @brief 借用一次内置调用所需的会话服务，并携带当前调用源码位置。
 *
 * 上下文只在回调期间有效，不取得服务所有权，也不提供 VM 重入入口。
 * Heap 和 RootRegistry 属于同一会话；Heap 分配及 TextBuilder 增长可能触发 GC。
 */
struct NativeContext {
    Heap &heap;                ///< 受管对象分配及 GC 服务。
    RootRegistry &roots;       ///< 登记跨分配仍须使用的临时值。
    HostIO &io;                ///< 当前会话的宿主输入输出服务。
    EmergencyFailure &failure; ///< 固定首错记录。
    SourceSpan span;           ///< 当前 MYB 调用的源码范围。
};
/**
 * @brief 内置回调接口，借用实参数组并通过已登记的稳定输出根交接结果。
 *
 * 参数 span 借用 VM 有效操作数区，仅在本次回调期间有效；回调不得保存该视图或
 * 元素地址，不得重入 VM 或引发 VM 值区、帧区增长。原实参由 VM 保活，
 * 回调仍可通过 Heap 分配及 GC；新增的中间受管值须另行登记根。
 * 输出根与上下文使用同一 RootRegistry，成功回调须写入结果，无返回值时写 null。
 * VM 在结果写入有效操作数槽后才撤销输出根；失败通过异常传播。
 */
using NativeFn = void (*)(NativeContext &, std::span<const Value>, RootedValue &);
/** @brief 描述一个固定实参数量的内置入口，名称存储由登记表提供者维持。 */
struct BuiltinDescriptor {
    std::string_view name; ///< 借用的内置名称。
    std::uint8_t arity;    ///< 必须精确匹配的实参数量。
    NativeFn invoke;       ///< 由 VM 在调用前检查非空的回调入口。
};
/**
 * @brief 返回21个静态名称及固定数量的描述表，供结构保护和调用预检共用。
 * 21 个回调均按 NativeFn 协议执行，名称、数量与回调在运行期间保持不变。
 */
std::span<const BuiltinDescriptor> builtin_descriptors() noexcept;
/**
 * @brief 在已完成实参求值后检查回调协议并执行内置，调试构建验证结果已写入。
 * @pre 描述数量与 args 相同；args 和 out 由同一会话根保活，回调期间不重入 VM。
 * @throws Failure 数量、回调、输出根契约失败，或回调报告动态及资源故障。
 */
void invoke_builtin(const BuiltinDescriptor &, NativeContext &, std::span<const Value>,
                    RootedValue &out);
/**
 * @brief 借用内置描述表，提供名称查找和实参数量预检。
 *
 * 自定义表及其名称内容须覆盖 Registry 使用期，执行期间保持不变。
 */
class BuiltinRegistry final {
  public:
    /** @brief 借用 table；默认借用具有静态存储期的描述表。 */
    explicit BuiltinRegistry(
        std::span<const BuiltinDescriptor> table = builtin_descriptors()) noexcept
        : table_(table) {}
    /**
     * @brief 按名称线性查找内置描述，不分配内存或触发 GC。
     * @return 首个同名描述的借用指针，未找到时为 nullptr；有效期取决于描述表。
     */
    const BuiltinDescriptor *find(std::string_view) const noexcept;
    /**
     * @brief 先检查目标存在，再检查固定实参数量，不检查实参值或回调指针。
     * @param[in] name 本次调用的目标名称。
     * @param[in] count 源码给出的实参数量。
     * @param[in] site 记录失败所用的首错记录及当前调用位置。
     * @return 借用的内置描述，描述表保持有效期间可使用。
     * @throws Failure 未找到目标时报告名称错误；数量不符时报告类型错误。
     *
     * PRECALL 在实参求值前调用本接口，CALL 在回调前再次调用。
     */
    const BuiltinDescriptor &precheck(std::string_view, std::uint32_t, ErrorSite) const;

  private:
    std::span<const BuiltinDescriptor> table_;
};
} // namespace myb
