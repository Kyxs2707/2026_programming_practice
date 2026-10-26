#include "runtime/display.hpp"
#include "runtime/graph_copy.hpp"
#include "runtime/heap.hpp"
#include "runtime/text_builder.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include <string>
#include <string_view>

using namespace myb;

namespace {
ArrayRef array(const RootedValue &root) {
    return std::get<ArrayRef>(root.get().data);
}
std::string shown(Heap &heap, EmergencyFailure &failure, Value value) {
    TextBuilder text(heap);
    display(value, text, {failure, {}});
    return std::string(text.bytes());
}
void make_string(Heap &heap, std::string_view input, RootedValue &out) {
    TextBuilder text(heap);
    text.append(std::span<const char>(input.data(), input.size()));
    heap.make_string(std::move(text), out);
}
} // namespace

MYB_TEST(graph_display) {
    EmergencyFailure failure;
    RootRegistry roots;
    Heap heap(roots, failure);
    TestHooks::force_gc(true);
    RootedValue source(roots), child(roots), copy(roots);
    heap.make_array(2, source);
    heap.make_array(1, child);
    array(child).ptr->elements.push_back(Value{std::int64_t{1}});
    array(source).ptr->elements.push_back(child.get());
    array(source).ptr->elements.push_back(child.get());
    REQUIRE(shown(heap, failure, source.get()) == "[[1], [1]]");
    deep_copy(heap, roots, source.get(), copy);
    const auto copied_outer = array(copy);
    const auto copied_child = std::get<ArrayRef>(copied_outer.ptr->elements[0].data);
    REQUIRE(copied_outer.ptr != array(source).ptr);
    REQUIRE(copied_child.ptr != array(child).ptr);
    REQUIRE(std::get<ArrayRef>(copied_outer.ptr->elements[1].data).ptr == copied_child.ptr);
    copied_child.ptr->elements[0] = Value{std::int64_t{9}};
    REQUIRE(shown(heap, failure, source.get()) == "[[1], [1]]");
    REQUIRE(shown(heap, failure, copy.get()) == "[[9], [9]]");
    array(child).ptr->elements[0] = source.get();
    REQUIRE(shown(heap, failure, source.get()) == "[[<cycle>], [<cycle>]]");
    array(source).ptr->elements.clear();
    array(source).ptr->elements.push_back(source.get());
    REQUIRE(shown(heap, failure, source.get()) == "[<cycle>]");
    deep_copy(heap, roots, source.get(), copy);
    REQUIRE(array(copy).ptr != array(source).ptr);
    REQUIRE(std::get<ArrayRef>(array(copy).ptr->elements[0].data).ptr == array(copy).ptr);
    REQUIRE(shown(heap, failure, copy.get()) == "[<cycle>]");
    TestHooks::force_gc(false);
}

MYB_TEST(graph_display_exact) {
    EmergencyFailure failure;
    RootRegistry roots;
    StructSchema marker{1, "Marker", {}};
    StructSchema point{
        2, "Point", {{"x", {TypeKind::Int64}}, {"y", {TypeKind::Int64}}}};
    StructSchema link{
        3, "Link", {{"items", {TypeKind::Array}}, {"next", {TypeKind::Struct, 3, true}}}};
    Heap heap(roots, failure);
    TestHooks::force_gc(true);
    RootedValue text(roots), value(roots), outer(roots), copied(roots);
    heap.make_struct(marker, value);
    REQUIRE(shown(heap, failure, value.get()) == "Marker {}");
    heap.make_struct(point, value);
    auto fields = std::get<StructRef>(value.get().data);
    fields.ptr->fields[0] = Value{std::int64_t{1}};
    fields.ptr->fields[1] = Value{std::int64_t{2}};
    REQUIRE(shown(heap, failure, value.get()) == "Point { x: 1, y: 2 }");
    deep_copy(heap, roots, value.get(), copied);
    const auto copied_point = std::get<StructRef>(copied.get().data);
    REQUIRE(copied_point.ptr != fields.ptr);
    REQUIRE(copied_point.ptr->schema == &point);
    copied_point.ptr->fields[0] = Value{std::int64_t{9}};
    REQUIRE(shown(heap, failure, value.get()) == "Point { x: 1, y: 2 }");
    const std::string input = std::string("\"\\\n\r\t\0\x1f\x7f", 8) +
                              "\xc2\x80\xc2\x9f\xe2\x80\xa8\xe2\x80\xa9北邮😀'";
    make_string(heap, input, text);
    REQUIRE(shown(heap, failure, text.get()) == input);
    deep_copy(heap, roots, text.get(), copied);
    REQUIRE(std::get<StringRef>(copied.get().data).ptr == std::get<StringRef>(text.get().data).ptr);
    heap.make_array(1, outer);
    array(outer).ptr->elements.push_back(text.get());
    REQUIRE(shown(heap, failure, outer.get()) ==
            "[\"\\\"\\\\\\n\\r\\t\\u{0}\\u{1f}\\u{7f}\\u{80}\\u{9f}\\u{2028}\\u{2029}北邮😀'\"]");
    const Value numbers[] = {Value{0.0}, Value{-0.0}, Value{1e-4}, Value{1e-5},
                             Value{1e15}, Value{1e16}, Value{true}, Value{NullValue{}}};
    heap.make_array(8, outer);
    for (const auto number : numbers)
        array(outer).ptr->elements.push_back(number);
    REQUIRE(shown(heap, failure, outer.get()) ==
            "[0.0, -0.0, 0.0001, 1e-5, 1000000000000000.0, 1e+16, true, null]");
    heap.make_struct(link, value);
    auto linked = std::get<StructRef>(value.get().data);
    linked.ptr->fields[0] = outer.get();
    linked.ptr->fields[1] = value.get();
    deep_copy(heap, roots, value.get(), copied);
    const auto copied_link = std::get<StructRef>(copied.get().data);
    REQUIRE(copied_link.ptr->schema == &link);
    REQUIRE(copied_link.ptr != linked.ptr);
    REQUIRE(std::get<StructRef>(copied_link.ptr->fields[1].data).ptr == copied_link.ptr);
    REQUIRE(std::get<ArrayRef>(copied_link.ptr->fields[0].data).ptr != array(outer).ptr);
    REQUIRE(shown(heap, failure, copied.get()) ==
            "Link { items: [0.0, -0.0, 0.0001, 1e-5, 1000000000000000.0, 1e+16, true, null], next: <cycle> }");
    TextBuilder formatted(heap);
    format_value(heap, copied.get(), formatted);
    REQUIRE(formatted.bytes() == shown(heap, failure, copied.get()));
    TestHooks::force_gc(false);
}

MYB_TEST(graph_display_failures) {
    for (const auto point : {FaultPoint::NativeWorkspace, FaultPoint::HeapObject,
                             FaultPoint::HeapRegistry, FaultPoint::Payload,
                             FaultPoint::MarkWorklist}) {
        EmergencyFailure failure;
        RootRegistry roots;
        Heap heap(roots, failure);
        RootedValue source(roots), child(roots), out(roots, Value{std::int64_t{42}});
        heap.make_array(2, source);
        heap.make_array(1, child);
        array(child).ptr->elements.push_back(source.get());
        array(source).ptr->elements.push_back(child.get());
        array(source).ptr->elements.push_back(child.get());
        TestHooks::force_gc(true);
        TestHooks::fail_next(point);
        bool threw = false;
        try {
            deep_copy(heap, roots, source.get(), out);
        } catch (const std::bad_alloc &) {
            REQUIRE(point == FaultPoint::NativeWorkspace);
            threw = true;
        } catch (const HostFailure &error) {
            REQUIRE(error.header().domain == FailureDomain::Resource);
            threw = true;
        }
        REQUIRE(threw);
        REQUIRE(std::get<std::int64_t>(out.get().data) == 42);
        REQUIRE(std::get<ArrayRef>(array(source).ptr->elements[0].data).ptr == array(child).ptr);
        REQUIRE(std::get<ArrayRef>(array(child).ptr->elements[0].data).ptr == array(source).ptr);
        TestHooks::clear_fault();
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 2);
        REQUIRE(TestHooks::heap_counts(heap).marked == 0);
        REQUIRE(!TestHooks::heap_counts(heap).collecting);
        TestHooks::force_gc(false);
    }
    for (const auto point : {FaultPoint::NativeWorkspace, FaultPoint::Payload,
                             FaultPoint::MarkWorklist}) {
        EmergencyFailure failure;
        RootRegistry roots;
        Heap heap(roots, failure);
        RootedValue source(roots), text(roots);
        make_string(heap, std::string(512, 'x'), text);
        heap.make_array(1, source);
        array(source).ptr->elements.push_back(text.get());
        TextBuilder output(heap);
        TestHooks::force_gc(true);
        TestHooks::fail_next(point);
        bool threw = false;
        try {
            display(source.get(), output, {failure, {}});
        } catch (const HostFailure &error) {
            REQUIRE(error.header().domain == FailureDomain::Resource);
            threw = true;
        }
        REQUIRE(threw);
        TestHooks::clear_fault();
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 2);
        REQUIRE(std::get<StringRef>(array(source).ptr->elements[0].data).ptr ==
                std::get<StringRef>(text.get().data).ptr);
        TestHooks::force_gc(false);
    }
}

MYB_TEST(graph_display_deep) {
    constexpr std::size_t depth = 50000;
    EmergencyFailure failure;
    RootRegistry roots;
    Heap heap(roots, failure);
    RootedValue head(roots), tail(roots), next(roots), copy(roots);
    TestHooks::force_gc(false);
    heap.make_array(1, head);
    tail.set(head.get());
    for (std::size_t i = 1; i < depth; ++i) {
        heap.make_array(1, next);
        array(tail).ptr->elements.push_back(next.get());
        tail.set(next.get());
    }
    array(tail).ptr->elements.push_back(Value{std::int64_t{17}});
    deep_copy(heap, roots, head.get(), copy);
    Value original = head.get();
    Value copied = copy.get();
    for (std::size_t i = 0; i < depth; ++i) {
        const auto a = std::get<ArrayRef>(original.data);
        const auto b = std::get<ArrayRef>(copied.data);
        REQUIRE(a.ptr != b.ptr);
        REQUIRE(a.ptr->elements.size() == 1);
        REQUIRE(b.ptr->elements.size() == 1);
        original = a.ptr->elements[0];
        copied = b.ptr->elements[0];
    }
    REQUIRE(std::get<std::int64_t>(original.data) == 17);
    REQUIRE(std::get<std::int64_t>(copied.data) == 17);
    const auto output = shown(heap, failure, copy.get());
    REQUIRE(output.size() == depth * 2 + 2);
    REQUIRE(output.substr(0, depth) == std::string(depth, '['));
    REQUIRE(output.substr(depth, 2) == "17");
    REQUIRE(output.substr(depth + 2) == std::string(depth, ']'));
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == depth * 2);
    head.set(Value{NullValue{}});
    tail.set(Value{NullValue{}});
    next.set(Value{NullValue{}});
    copy.set(Value{NullValue{}});
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == 0);
}
