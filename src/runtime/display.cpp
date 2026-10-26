#include "runtime/display.hpp"
#include "runtime/decimal_codec.hpp"
#include "runtime/object.hpp"
#include "runtime/text_builder.hpp"
#include "source/utf8.hpp"
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#ifdef MYB_TEST_HOOKS
#include "test_hooks.hpp"
#endif

namespace myb {
namespace {
void append(TextBuilder &output, std::string_view text) {
    output.append(std::span<const char>(text.data(), text.size()));
}
/** @brief 输出嵌套字符串的控制字符转义，其他 Unicode 标量保持原字节。 */
void quoted_string(StringRef string, TextBuilder &output, ErrorSite site) {
    append(output, "\"");
    const std::string_view text = string.ptr->utf8;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto decoded = utf8::decode_one(text, offset);
        if (!decoded)
            fail(site, FailureDomain::Internal, ReasonCode::Invariant,
                 "display encountered invalid managed UTF-8");
        const auto codepoint = decoded->value;
        switch (codepoint) {
        case '"': append(output, "\\\""); break;
        case '\\': append(output, "\\\\"); break;
        case '\n': append(output, "\\n"); break;
        case '\r': append(output, "\\r"); break;
        case '\t': append(output, "\\t"); break;
        default:
            if (codepoint <= 0x1f || (codepoint >= 0x7f && codepoint <= 0x9f) ||
                codepoint == 0x2028 || codepoint == 0x2029) {
                char hex[8];
                const auto converted = std::to_chars(hex, hex + sizeof hex, codepoint, 16);
                append(output, "\\u{");
                output.append(std::span<const char>(
                    hex, static_cast<std::size_t>(converted.ptr - hex)));
                append(output, "}");
            } else {
                append(output, text.substr(offset, decoded->width));
            }
        }
        offset += decoded->width;
    }
    append(output, "\"");
}
/** @brief 保存当前复合对象的下一个元素位置，所有值均由输入图根间接保活。 */
struct DisplayFrame {
    Value value;
    std::size_t next{};
    bool entered{};
};
/** @brief 按至少所需容量和约 1.5 倍增长显式工作栈，不进入 MYB GC。 */
void push_frame(std::vector<DisplayFrame> &stack, Value value) {
    if (stack.size() == stack.capacity()) {
#ifdef MYB_TEST_HOOKS
        TestHooks::hit(FaultPoint::NativeWorkspace);
#endif
        const auto capacity = stack.capacity();
        const auto extra = std::max<std::size_t>(capacity / 2, 1);
        const auto next = capacity > stack.max_size() - extra ? stack.max_size()
                                                             : capacity + extra;
        if (stack.size() == stack.max_size())
            throw std::length_error("display workspace capacity exceeded");
        stack.reserve(next);
    }
    stack.push_back({value});
}

void display_graph(Value value, TextBuilder &output, ErrorSite site) {
    std::vector<DisplayFrame> stack;
    std::unordered_set<const HeapObject *> path;
    push_frame(stack, value);
    while (!stack.empty()) {
        const DisplayFrame frame = stack.back();
        if (auto string = std::get_if<StringRef>(&frame.value.data)) {
            if (stack.size() == 1)
                append(output, string->ptr->utf8);
            else
                quoted_string(*string, output, site);
            stack.pop_back();
            continue;
        }
        auto array = std::get_if<ArrayRef>(&frame.value.data);
        auto structure = std::get_if<StructRef>(&frame.value.data);
        if (array || structure) {
            const HeapObject *object = array ? static_cast<HeapObject *>(array->ptr)
                                             : static_cast<HeapObject *>(structure->ptr);
            const auto count = array ? array->ptr->elements.size() : structure->ptr->fields.size();
            if (!frame.entered) {
                if (path.contains(object)) {
                    append(output, "<cycle>");
                    stack.pop_back();
                    continue;
                }
#ifdef MYB_TEST_HOOKS
                TestHooks::hit(FaultPoint::NativeWorkspace);
#endif
                path.insert(object);
                if (array)
                    append(output, "[");
                else {
                    append(output, structure->ptr->schema->name);
                    append(output, count ? " { " : " {");
                }
                stack.back().entered = true;
            }
            if (frame.next < count) {
                if (frame.next)
                    append(output, ", ");
                Value child;
                if (array)
                    child = array->ptr->elements[frame.next];
                else {
                    append(output, structure->ptr->schema->fields[frame.next].name);
                    append(output, ": ");
                    child = structure->ptr->fields[frame.next];
                }
                ++stack.back().next;
                push_frame(stack, child);
            } else {
                append(output, array ? "]" : count ? " }" : "}");
                path.erase(object);
                stack.pop_back();
            }
            continue;
        }
        std::visit(
            [&](auto data) {
                using T = decltype(data);
                if constexpr (std::is_same_v<T, NullValue>)
                    append(output, "null");
                else if constexpr (std::is_same_v<T, bool>)
                    append(output, data ? "true" : "false");
                else if constexpr (std::is_same_v<T, std::int64_t>) {
                    char buffer[32];
                    const auto converted = std::to_chars(buffer, buffer + sizeof buffer, data);
                    if (converted.ec != std::errc{})
                        fail(site, FailureDomain::Internal, ReasonCode::Invariant,
                             "integer display failed");
                    output.append(std::span<const char>(
                        buffer, static_cast<std::size_t>(converted.ptr - buffer)));
                } else if constexpr (std::is_same_v<T, double>)
                    append(output, DecimalCodec::format_float(data));
            },
            frame.value.data);
        stack.pop_back();
    }
}
} // namespace

void display(Value value, TextBuilder &output, ErrorSite site) {
    try {
        display_graph(value, output, site);
    } catch (const std::bad_alloc &) {
        fail(site, FailureDomain::Resource, ReasonCode::Allocation,
             "display workspace allocation failed");
    } catch (const std::length_error &) {
        fail(site, FailureDomain::Resource, ReasonCode::Capacity,
             "display workspace capacity exceeded");
    }
}
void format_value(Heap &, Value value, TextBuilder &output) {
    EmergencyFailure failure;
    display(value, output, {failure, {}});
}
} // namespace myb
