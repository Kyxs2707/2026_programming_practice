#pragma once
#ifndef MYB_TEST_HOOKS
#error "Test hooks must only be included by internal test builds"
#endif
#include <cstddef>
#include <new>
#include <cstdint>
#include <optional>

namespace myb {
class Heap;
class VM;
/** @brief 内部故障注入位置，仅在 MYB_TEST_HOOKS 构建中可用。 */
enum class FaultPoint {
    HeapObject,
    HeapRegistry,
    Payload,
    MarkWorklist,
    ProgramConstants,
    NativeWorkspace,
    VmValues,
    VmStates,
    VmFrames,
    IterationRecords,
    FlexAllocation,
    DiagnosticSnapshot,
    OsRead,
    OsWrite,
    OsClose
};
namespace TestHooks {
/** @brief 堆对象与回收状态的只读计数结果，不持有或保活对象。 */
struct HeapCounts {
    std::size_t live{};
    std::size_t marked{};
    bool collecting{};
    std::size_t active_arrays{};
};
/** @brief 控制 Heap 在受控增长前强制回收；进程内共享，测试结束须恢复。 */
void force_gc(bool enabled) noexcept;
/** @brief 查询进程内的强制回收开关。 */
bool gc_forced() noexcept;
/** @brief 替换待触发位置；下一次命中该位置时抛出一次 std::bad_alloc。 */
void fail_next(FaultPoint point) noexcept;
/** @brief 清除尚未触发的故障设置，不改变强制回收开关。 */
void clear_fault() noexcept;
/** @brief 若命中已设置位置，先清除设置再抛 std::bad_alloc，保证只触发一次。 */
void hit(FaultPoint point);
/** @brief 设置下一次指定宿主 I/O 调用的 Windows 错误码，仅触发一次。 */
void fail_os_next(FaultPoint point, std::uint32_t code) noexcept;
/** @brief 取出并清除匹配位置的 OS 故障；未匹配时返回空值。 */
std::optional<std::uint32_t> take_os_failure(FaultPoint point) noexcept;
/** @brief 读取堆中对象数、标记数、回收状态与具有活动遍历的数组数。 */
HeapCounts heap_counts(const Heap &heap) noexcept;
/** @brief 读取 VM 已提交活动遍历记录数，不分配或保活来源对象。 */
std::size_t active_iteration_count(const VM &vm) noexcept;
} // namespace TestHooks
} // namespace myb
