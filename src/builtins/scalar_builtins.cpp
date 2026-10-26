#include "builtins/functions.hpp"
#include "runtime/display.hpp"
#include "runtime/decimal_codec.hpp"
#include "runtime/graph_copy.hpp"
#include "runtime/text_builder.hpp"
#include <stdexcept>
#include <cmath>

namespace myb {
namespace {
/** @brief 将受控解析错误映射为转换错误，try 入口仅把这两类结果转为 null。 */
template <class T>
void parsed_result(NativeContext &context, const NumericParseResult<T> &parsed,
                   bool optional, RootedValue &out) {
    if (parsed.value) {
        out.set(Value{*parsed.value});
        return;
    }
    if (optional) {
        out.set(Value{NullValue{}});
        return;
    }
    fail({context.failure, context.span}, FailureDomain::Runtime,
         parsed.error == NumericParseError::Range ? ReasonCode::Range : ReasonCode::Type,
         "invalid numeric conversion");
}
/** @brief 借用字符串实参的不可变字节；调用期间由 VM 参数根保活。 */
std::string_view numeric_text(Value value, NativeContext &context) {
    if (const auto text = std::get_if<StringRef>(&value.data))
        return text->ptr->utf8;
    fail({context.failure, context.span}, FailureDomain::Runtime, ReasonCode::Type,
         "expected String");
}
} // namespace

void builtin_int(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    if (std::holds_alternative<std::int64_t>(args[0].data)) {
        out.set(args[0]);
        return;
    }
    if (const auto number = std::get_if<double>(&args[0].data)) {
        const double truncated = std::trunc(*number);
        // 上界 2^63 必须排除；Int64 最大值转成 double 已经舍入至该上界。
        if (!std::isfinite(truncated) || truncated < -0x1p63 || truncated >= 0x1p63)
            fail({context.failure, context.span}, FailureDomain::Runtime, ReasonCode::Range,
                 "Float64 is outside Int64 range");
        out.set(Value{static_cast<std::int64_t>(truncated)});
        return;
    }
    parsed_result(context, DecimalCodec::parse_int(numeric_text(args[0], context)), false, out);
}

void builtin_float(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    if (std::holds_alternative<double>(args[0].data)) {
        out.set(args[0]);
        return;
    }
    if (const auto number = std::get_if<std::int64_t>(&args[0].data)) {
        out.set(Value{static_cast<double>(*number)});
        return;
    }
    parsed_result(context,
                  DecimalCodec::parse_float(numeric_text(args[0], context), ParseMode::Conversion),
                  false, out);
}

void builtin_try_int(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    parsed_result(context, DecimalCodec::parse_int(numeric_text(args[0], context)), true, out);
}

void builtin_try_float(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    parsed_result(context,
                  DecimalCodec::parse_float(numeric_text(args[0], context), ParseMode::Conversion),
                  true, out);
}

void builtin_assert(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    const auto condition = std::get_if<bool>(&args[0].data);
    const auto message = std::get_if<StringRef>(&args[1].data);
    if (!condition || !message)
        fail({context.failure, context.span}, FailureDomain::Runtime, ReasonCode::Type,
             "assert expects Bool and String");
    if (!*condition)
        fail({context.failure, context.span}, FailureDomain::Runtime, ReasonCode::Assertion,
             message->ptr->utf8);
    out.set(Value{NullValue{}});
}

void builtin_deep_copy(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    try {
        deep_copy(context.heap, context.roots, args[0], out);
    } catch (const std::bad_alloc &) {
        fail({context.failure, context.span}, FailureDomain::Resource, ReasonCode::Allocation,
             "deep copy workspace allocation failed");
    } catch (const std::length_error &) {
        fail({context.failure, context.span}, FailureDomain::Resource, ReasonCode::Capacity,
             "deep copy workspace capacity exceeded");
    }
}
void builtin_str(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    TextBuilder text(context.heap);
    display(args[0], text, {context.failure, context.span});
    context.heap.make_string(std::move(text), out);
}
} // namespace myb
