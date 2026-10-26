#include "builtins/functions.hpp"
#include "runtime/display.hpp"
#include "runtime/text_builder.hpp"

namespace myb {
namespace {
/**
 * @brief 显示单个参数并写入标准输出，成功时将 null 写入输出根。
 * @pre args 含一个由 VM 操作数区保活的值，result 已登记于 context.roots。
 * 文本缓冲增长可能触发 GC，参数根须覆盖显示及写入；宿主写入失败直接传播异常。
 */
void output(NativeContext &context, std::span<const Value> args, RootedValue &result, bool newline) {
    TextBuilder text(context.heap);
    display(args[0], text, {context.failure, context.span});
    context.io.write_stdout(text.bytes(), newline);
    result.set(Value{NullValue{}});
}
/** @brief 检查并借用 VM 实参保护的 String 字节；借用持续至回调结束。 */
std::string_view string_argument(const Value &value, NativeContext &context) {
    const auto string = std::get_if<StringRef>(&value.data);
    if (!string)
        fail({context.failure, context.span}, FailureDomain::Runtime, ReasonCode::Type,
             "text I/O requires String arguments");
    return string->ptr->utf8;
}
/** @brief 将宿主 I/O 原因映射为当前 MYB 调用处的运行失败。 */
[[noreturn]] void runtime_io(NativeContext &context, const IoFailure &failure) {
    fail({context.failure, context.span}, FailureDomain::Runtime, failure.reason(),
         failure.what(), failure.os_code());
}
} // namespace
void builtin_print(NativeContext &context, std::span<const Value> args, RootedValue &result) {
    output(context, args, result, true);
}
void builtin_write(NativeContext &context, std::span<const Value> args, RootedValue &result) {
    output(context, args, result, false);
}
void builtin_read_line(NativeContext &context, std::span<const Value>, RootedValue &result) {
    TextBuilder text(context.heap);
    try {
        if (context.io.read_line(text) == LineStatus::End) {
            result.set(Value{NullValue{}});
            return;
        }
    } catch (const IoFailure &failure) {
        runtime_io(context, failure);
    }
    context.heap.make_string(std::move(text), result);
}
void builtin_read_text(NativeContext &context, std::span<const Value> args, RootedValue &result) {
    const auto path = string_argument(args[0], context);
    TextBuilder text(context.heap);
    try {
        context.io.read_text(path, text);
    } catch (const IoFailure &failure) {
        runtime_io(context, failure);
    }
    context.heap.make_string(std::move(text), result);
}
void builtin_write_text(NativeContext &context, std::span<const Value> args, RootedValue &result) {
    const auto path = string_argument(args[0], context);
    const auto text = string_argument(args[1], context);
    try {
        context.io.write_text(path, text);
    } catch (const IoFailure &failure) {
        runtime_io(context, failure);
    }
    result.set(Value{NullValue{}});
}
} // namespace myb
