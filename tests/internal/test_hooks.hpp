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
    OsClose,
    ExecutionIndex,
    EntryPathBuffer,
    EntryPathText,
    EntryDiagnostic
};
namespace TestHooks {
/** @brief 区分执行索引准备及实际候选访问，计数只用于内部规模回归。 */
enum class ExecutionWork { CallCandidate, SchemaCandidate, FieldCandidate, Construction, FieldWrite };
/** @brief 保存准备期与执行期实际处理次数；不持有受管对象或借用。 */
struct ExecutionWorkCounts {
    std::size_t preparation{}, calls{}, schemas{}, fields{}, constructions{}, writes{};
};
/** @brief 清零工作量并离开准备阶段，不改变故障设置。 */
void reset_execution_work() noexcept;
/** @brief 切换索引准备阶段；仅影响内部工作量的归属，不分配或回收。 */
void preparing_execution_index(bool enabled) noexcept;
/** @brief 在候选访问、比较或槽写入的实际位置累计工作量，不分配或回收。 */
void record_execution_work(ExecutionWork kind) noexcept;
/** @brief 返回工作量的独立值副本，不改变计数。 */
ExecutionWorkCounts execution_work() noexcept;
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
/**
 * @brief 替换待触发位置，在指定次数的同位置命中时抛出一次 std::bad_alloc。
 * @param[in] occurrence 从 1 开始计数，其他位置的命中不推进计数；0 按 1 处理。
 */
void fail_next(FaultPoint point, std::size_t occurrence) noexcept;
/** @brief 清除尚未触发的故障设置，不改变强制回收开关。 */
void clear_fault() noexcept;
/** @brief 推进已设置位置的命中计数，到达指定次数时先清除设置再抛 std::bad_alloc。 */
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
