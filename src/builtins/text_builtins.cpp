#include "builtins/functions.hpp"
#include "runtime/text_builder.hpp"
#include <limits>

namespace myb {
namespace {
/** @brief 检查实参为 String，并借用由调用者保活的不可变 UTF-8 字节。 */
std::string_view text_arg(Value value, NativeContext &context) {
    if (const auto text = std::get_if<StringRef>(&value.data))
        return text->ptr->utf8;
    fail({context.failure, context.span}, FailureDomain::Runtime, ReasonCode::Type,
         "expected String");
}
/**
 * @brief 为有根数组追加独立片段，字符串与数组扩容期间分别保持根有效。
 * @pre array 属于 context.heap 且已有根；text 的源对象由调用者保活。
 */
void append_part(NativeContext &context, ArrayRef array, std::string_view text) {
    const auto size = array.ptr->elements.size();
    if (size == std::numeric_limits<std::size_t>::max())
        fail({context.failure, context.span}, FailureDomain::Resource, ReasonCode::Capacity,
             "too many text fragments");
    context.heap.reserve_array(array, size + 1);
    TextBuilder builder(context.heap);
    builder.append(std::span<const char>(text.data(), text.size()));
    RootedValue part(context.roots);
    context.heap.make_string(std::move(builder), part);
    array.ptr->elements.push_back(part.get());
}
/** @brief 将 UTF-8 字节构造成有根结果；输入的受管所有者由调用者保活。 */
void result_text(NativeContext &context, std::string_view text, RootedValue &out) {
    TextBuilder builder(context.heap);
    builder.append(std::span<const char>(text.data(), text.size()));
    context.heap.make_string(std::move(builder), out);
}
bool ascii_space(unsigned char c) noexcept {
    return c == 32 || (c >= 9 && c <= 13);
}
} // namespace

void builtin_split(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    const auto text = text_arg(args[0], context);
    const auto separator = text_arg(args[1], context);
    if (separator.empty())
        fail({context.failure, context.span}, FailureDomain::Runtime, ReasonCode::Range,
             "separator must not be empty");
    RootedValue result(context.roots);
    context.heap.make_array(0, result);
    const auto array = std::get<ArrayRef>(result.get().data);
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find(separator, start);
        append_part(context, array,
                    text.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos)
            break;
        start = end + separator.size();
    }
    out.set(result.get());
}

void builtin_join(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    const auto parts = std::get_if<ArrayRef>(&args[0].data);
    if (!parts)
        fail({context.failure, context.span}, FailureDomain::Runtime, ReasonCode::Type,
             "expected Array");
    const auto separator = text_arg(args[1], context);
    // 全部元素检查完成后才构造结果，混合数组在首个非 String 处失败。
    for (const auto value : parts->ptr->elements)
        (void)text_arg(value, context);
    TextBuilder builder(context.heap);
    bool first = true;
    for (const auto value : parts->ptr->elements) {
        if (!first)
            builder.append(std::span<const char>(separator.data(), separator.size()));
        const std::string_view text = std::get<StringRef>(value.data).ptr->utf8;
        builder.append(std::span<const char>(text.data(), text.size()));
        first = false;
    }
    context.heap.make_string(std::move(builder), out);
}

void builtin_trim(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    auto text = text_arg(args[0], context);
    while (!text.empty() && ascii_space(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && ascii_space(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    result_text(context, text, out);
}

void builtin_split_lines(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    const auto text = text_arg(args[0], context);
    RootedValue result(context.roots);
    context.heap.make_array(0, result);
    const auto array = std::get<ArrayRef>(result.get().data);
    std::size_t start = 0;
    while (start < text.size()) {
        const auto newline = text.find('\n', start);
        auto end = newline == std::string_view::npos ? text.size() : newline;
        if (newline != std::string_view::npos && end > start && text[end - 1] == '\r')
            --end;
        append_part(context, array, text.substr(start, end - start));
        if (newline == std::string_view::npos)
            break;
        start = newline + 1;
    }
    out.set(result.get());
}
} // namespace myb
