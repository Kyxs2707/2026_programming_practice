#include "builtins/functions.hpp"
#include <limits>

namespace myb {
namespace {
ArrayRef array_value(Value value, ErrorSite site) {
    if (const auto array = std::get_if<ArrayRef>(&value.data))
        return *array;
    fail(site, FailureDomain::Runtime, ReasonCode::Type, "array operation requires Array");
}
/**
 * @brief 转换负索引，读取仅接受现有元素，插入额外接受末尾非负位置。
 * @pre length 可表示为 Int64；空数组仅允许插入位置零。
 */
std::size_t array_position(Value value, std::size_t length, bool insertion, ErrorSite site) {
    const auto integer = std::get_if<std::int64_t>(&value.data);
    if (!integer)
        fail(site, FailureDomain::Runtime, ReasonCode::Type, "array index must be Int64");
    const auto n = static_cast<std::int64_t>(length);
    if (*integer < -n || *integer > n || (!insertion && *integer == n))
        fail(site, FailureDomain::Runtime, ReasonCode::Range, "array index out of range");
    return static_cast<std::size_t>(*integer < 0 ? *integer + n : *integer);
}
void check_mutation(ArrayRef array, ErrorSite site) {
    if (array.ptr->active_iteration_count)
        fail(site, FailureDomain::Runtime, ReasonCode::IterationMutation,
             "cannot change array length during iteration");
}
/**
 * @brief 完成容量检查和受保护扩容后插入值，成功才发布长度变化。
 * @pre 数组和值在调用期间有效；位置已检查且活动遍历计数为零。
 * 扩容可能触发 GC，临时根同时保护数组和值；扩容后重新获取元素位置。
 */
void insert_value(NativeContext &context, ArrayRef array, std::size_t index, Value value,
                  RootedValue &out) {
    const ErrorSite site{context.failure, context.span};
    RootedValue receiver(context.roots, Value{array}), item(context.roots, value);
    if (array.ptr->elements.size() ==
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        fail(site, FailureDomain::Resource, ReasonCode::Capacity, "array length exceeds Int64");
    context.heap.reserve_array(array, array.ptr->elements.size() + 1);
    auto &elements = array.ptr->elements;
    elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(index), item.get());
    out.set(Value{NullValue{}});
}
} // namespace
void builtin_len(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    std::size_t length;
    if (const auto array = std::get_if<ArrayRef>(&args[0].data))
        length = array->ptr->elements.size();
    else if (const auto string = std::get_if<StringRef>(&args[0].data))
        length = string->ptr->codepoint_count;
    else
        fail({context.failure, context.span}, FailureDomain::Runtime, ReasonCode::Type,
             "len requires Array or String");
    out.set(Value{static_cast<std::int64_t>(length)});
}
void builtin_append(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    const ErrorSite site{context.failure, context.span};
    const auto array = array_value(args[0], site);
    check_mutation(array, site);
    insert_value(context, array, array.ptr->elements.size(), args[1], out);
}
void builtin_insert(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    const ErrorSite site{context.failure, context.span};
    const auto array = array_value(args[0], site);
    const auto index = array_position(args[1], array.ptr->elements.size(), true, site);
    check_mutation(array, site);
    insert_value(context, array, index, args[2], out);
}
void builtin_pop(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    const ErrorSite site{context.failure, context.span};
    const auto array = array_value(args[0], site);
    if (array.ptr->elements.empty())
        fail(site, FailureDomain::Runtime, ReasonCode::Range, "pop requires nonempty Array");
    check_mutation(array, site);
    // 结果根先接管移出对象，再撤销原数组槽位的可达边。
    out.set(array.ptr->elements.back());
    array.ptr->elements.pop_back();
}
void builtin_remove_at(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    const ErrorSite site{context.failure, context.span};
    const auto array = array_value(args[0], site);
    const auto index = array_position(args[1], array.ptr->elements.size(), false, site);
    check_mutation(array, site);
    out.set(array.ptr->elements[index]);
    array.ptr->elements.erase(array.ptr->elements.begin() + static_cast<std::ptrdiff_t>(index));
}
} // namespace myb
