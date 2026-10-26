#include "runtime/sequence_ops.hpp"
#include "runtime/heap.hpp"
#include "runtime/text_builder.hpp"
#include "source/utf8.hpp"
#include <algorithm>

namespace myb {
namespace {
std::int64_t integer(Value value, ErrorSite site) {
    if (auto p = std::get_if<std::int64_t>(&value.data))
        return *p;
    fail(site, FailureDomain::Runtime, ReasonCode::Type, "sequence index must be Int64");
}
/**
 * @brief 将严格 Int64 的负索引转换为现有元素位置，空序列或越界时报告 Range。
 * @pre length 已受 Heap 的 Int64 长度上界检查；不分配或触发 GC。
 */
std::size_t position(Value index, std::size_t length, ErrorSite site) {
    auto value = integer(index, site);
    const auto n = static_cast<std::int64_t>(length);
    if (value < -n || value >= n)
        fail(site, FailureDomain::Runtime, ReasonCode::Range, "sequence index out of range");
    return static_cast<std::size_t>(value < 0 ? value + n : value);
}
/** @brief 区分省略端点与显式值，并将严格 Int64 端点按当前长度限制至有效边界。 */
std::size_t endpoint(std::optional<Value> value, std::size_t length, std::size_t fallback,
                     ErrorSite site) {
    if (!value)
        return fallback;
    auto index = integer(*value, site);
    const auto n = static_cast<std::int64_t>(length);
    if (index < 0)
        index += n;
    return static_cast<std::size_t>(std::clamp(index, std::int64_t{0}, n));
}
/** @brief 在线性扫描已验证的 UTF-8 时将码点位置转换为字节偏移，不分配或回收。 */
std::size_t byte_offset(std::string_view text, std::size_t codepoint, ErrorSite site) {
    std::size_t offset = 0;
    for (std::size_t i = 0; i < codepoint; ++i) {
        const auto decoded = utf8::decode_one(text, offset);
        if (!decoded)
            fail(site, FailureDomain::Internal, ReasonCode::Invariant, "invalid String payload");
        offset += decoded->width;
    }
    return offset;
}
} // namespace
void SequenceOps::check_output(const RootedValue &out, ErrorSite site) const {
    if (!out.belongs_to(roots_))
        fail(site, FailureDomain::Internal, ReasonCode::Invariant,
             "output root belongs to another registry or is detached");
}
void SequenceOps::get_index(Value receiver, Value index, ErrorSite site, RootedValue &out) {
    check_output(out, site);
    RootedValue source(roots_, receiver);
    if (const auto array = std::get_if<ArrayRef>(&receiver.data)) {
        out.set(array->ptr->elements[position(index, array->ptr->elements.size(), site)]);
        return;
    }
    if (const auto string = std::get_if<StringRef>(&receiver.data)) {
        const auto i = position(index, string->ptr->codepoint_count, site);
        const auto &text = string->ptr->utf8;
        const auto first = byte_offset(text, i, site);
        const auto last = byte_offset(text, i + 1, site);
        TextBuilder builder(heap_);
        builder.append({text.data() + first, last - first});
        heap_.make_string(std::move(builder), out);
        return;
    }
    fail(site, FailureDomain::Runtime, ReasonCode::Type, "index requires Array or String");
}
void SequenceOps::get_slice(Value receiver, std::optional<Value> start,
                            std::optional<Value> end, ErrorSite site, RootedValue &out) {
    check_output(out, site);
    RootedValue source(roots_, receiver);
    const auto array = std::get_if<ArrayRef>(&receiver.data);
    const auto string = std::get_if<StringRef>(&receiver.data);
    if (!array && !string)
        fail(site, FailureDomain::Runtime, ReasonCode::Type, "slice requires Array or String");
    const auto length = array ? array->ptr->elements.size() : string->ptr->codepoint_count;
    const auto first = endpoint(start, length, 0, site);
    const auto last = std::max(first, endpoint(end, length, length, site));
    if (array) {
        RootedValue result(roots_);
        heap_.make_array(last - first, result);
        auto &elements = std::get<ArrayRef>(result.get().data).ptr->elements;
        for (auto i = first; i < last; ++i)
            elements.push_back(array->ptr->elements[i]);
        out.set(result.get());
        return;
    }
    const auto &text = string->ptr->utf8;
    const auto begin_byte = byte_offset(text, first, site);
    const auto end_byte = byte_offset(text, last, site);
    TextBuilder builder(heap_);
    builder.append({text.data() + begin_byte, end_byte - begin_byte});
    heap_.make_string(std::move(builder), out);
}
void SequenceOps::set_index(Value receiver, Value index, Value value, ErrorSite site) {
    const auto array = std::get_if<ArrayRef>(&receiver.data);
    if (!array)
        fail(site, FailureDomain::Runtime, ReasonCode::Type, "index assignment requires Array");
    const auto i = position(index, array->ptr->elements.size(), site);
    array->ptr->elements[i] = value;
}
} // namespace myb
