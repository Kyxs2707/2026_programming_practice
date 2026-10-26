#pragma once
#include "builtins/registry.hpp"

namespace myb {
/**
 * @brief 按 NativeFn 协议执行固定内置，实参由调用者保活，成功时写入已登记的输出根。
 * 各回调借用 context 和 args 至返回；分配与文本增长可能触发 GC，
 * 中间结果由回调登记临时根，输出交接前调用者保持参数和输出根有效。
 * @throws Failure 参数检查、程序 I/O、资源或内部契约失败。
 * @throws std::bad_alloc 宿主工作区分配失败，由执行边界分类。
 * @{
 */
void builtin_print(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_write(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_len(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_append(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_insert(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_pop(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_remove_at(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_deep_copy(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_read_line(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_int(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_float(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_str(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_try_int(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_try_float(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_assert(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_read_text(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_write_text(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_split(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_join(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_trim(NativeContext &, std::span<const Value>, RootedValue &);
void builtin_split_lines(NativeContext &, std::span<const Value>, RootedValue &);
/** @} */
} // namespace myb
