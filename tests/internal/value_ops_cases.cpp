#include "runtime/heap.hpp"
#include "runtime/text_builder.hpp"
#include "runtime/value_ops.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include <cmath>
#include <limits>
using namespace myb;

MYB_TEST(scalar_value_ops) {
    EmergencyFailure first;
    ErrorSite site{first, {}};
    auto i = [](std::int64_t n) { return Value{n}; };
    REQUIRE(std::get<std::int64_t>(ValueOps::binary(BinaryOp::Add, i(1), i(2), site).data) == 3);
    REQUIRE(std::get<double>(ValueOps::binary(BinaryOp::Divide, i(3), i(2), site).data) == 1.5);
    REQUIRE(std::get<std::int64_t>(
                ValueOps::binary(BinaryOp::FloorDivide, i(-7), i(3), site).data) == -3);
    REQUIRE(std::get<std::int64_t>(ValueOps::binary(BinaryOp::Modulo, i(-7), i(3), site).data) ==
            2);
    REQUIRE(std::get<std::int64_t>(ValueOps::binary(BinaryOp::Modulo, i(7), i(-3), site).data) ==
            -2);
    auto minimum = i(std::numeric_limits<std::int64_t>::min());
    REQUIRE(std::get<std::int64_t>(ValueOps::binary(BinaryOp::Modulo, minimum, i(-1), site).data) ==
            0);
    REQUIRE(std::signbit(std::get<double>(ValueOps::unary(UnaryOp::Minus, Value{0.0}, site).data)));
    struct Bad {
        BinaryOp op;
        Value lhs, rhs;
        ReasonCode reason;
    };
    Bad cases[] = {
        {BinaryOp::Add, i(std::numeric_limits<std::int64_t>::max()), i(1), ReasonCode::Arithmetic},
        {BinaryOp::Multiply, minimum, i(-1), ReasonCode::Arithmetic},
        {BinaryOp::FloorDivide, minimum, i(-1), ReasonCode::Arithmetic},
        {BinaryOp::Divide, i(1), Value{-0.0}, ReasonCode::Arithmetic},
        {BinaryOp::Add, Value{true}, i(1), ReasonCode::Type},
        {BinaryOp::Add, Value{std::numeric_limits<double>::max()},
         Value{std::numeric_limits<double>::max()}, ReasonCode::Arithmetic},
    };
    for (const auto &c : cases) {
        bool caught = false;
        try {
            (void)ValueOps::binary(c.op, c.lhs, c.rhs, site);
        } catch (const MybRuntimeFailure &f) {
            caught = f.header().reason == c.reason;
        }
        REQUIRE(caught);
    }
}

namespace {
Value integer(std::int64_t value) {
    return Value{value};
}
bool compare(CompareOp op, Value lhs, Value rhs, ErrorSite site) {
    return std::get<bool>(ValueOps::compare(op, lhs, rhs, site).data);
}
void make_text(Heap &heap, std::string_view text, RootedValue &out) {
    TextBuilder builder(heap);
    builder.append({text.data(), text.size()});
    heap.make_string(std::move(builder), out);
}
template <class Action> void rejects(Action action, ReasonCode reason, SourceSpan span) {
    bool caught = false;
    try {
        action();
    } catch (const MybRuntimeFailure &failure) {
        caught = true;
        REQUIRE(failure.header().reason == reason);
        REQUIRE(failure.header().span.has_value());
        REQUIRE(failure.header().span->source == span.source);
        REQUIRE(failure.header().span->begin == span.begin);
        REQUIRE(failure.header().span->end == span.end);
    }
    REQUIRE(caught);
}
} // namespace

// 有损提升、边界截断、隐式真假转换和对象内容比较均会破坏这些结果。
MYB_TEST(value_ops) {
    EmergencyFailure first;
    const SourceSpan span{3, 7, 9};
    ErrorSite site{first, span};
    RootRegistry roots;
    StructSchema schema{0, "Empty", {}};
    Heap heap(roots, first);
    ValueOps ops(heap, roots);
    const auto minimum = integer(std::numeric_limits<std::int64_t>::min());
    const auto maximum = integer(std::numeric_limits<std::int64_t>::max());
    struct Ordered {
        Value lhs, rhs;
        int order;
    };
    const Ordered ordered[] = {
        {integer(9007199254740993LL), Value{9007199254740992.0}, 1},
        {integer(-9007199254740993LL), Value{-9007199254740992.0}, -1},
        {maximum, Value{0x1p63}, -1},
        {minimum, Value{-0x1p63}, 0},
        {minimum, Value{std::nextafter(-0x1p63, -INFINITY)}, 1},
        {maximum, Value{std::nextafter(0x1p63, 0.0)}, 1},
        {minimum, Value{std::nextafter(-0x1p63, 0.0)}, -1},
        {integer(2), Value{2.5}, -1},
        {integer(-2), Value{-2.5}, 1},
        {integer(0), Value{std::numeric_limits<double>::denorm_min()}, -1},
        {integer(0), Value{-std::numeric_limits<double>::denorm_min()}, 1},
        {integer(0), Value{-0.0}, 0},
        {Value{0.0}, Value{-0.0}, 0},
        {integer(9007199254740992LL), Value{9007199254740992.0}, 0},
        {minimum, maximum, -1},
        {Value{-std::numeric_limits<double>::max()}, minimum, -1},
        {Value{std::numeric_limits<double>::max()}, maximum, 1},
        {Value{2.0}, Value{std::nextafter(2.0, 3.0)}, -1},
    };
    for (const auto &test : ordered) {
        REQUIRE(compare(CompareOp::Equal, test.lhs, test.rhs, site) == (test.order == 0));
        REQUIRE(compare(CompareOp::NotEqual, test.lhs, test.rhs, site) == (test.order != 0));
        REQUIRE(compare(CompareOp::Less, test.lhs, test.rhs, site) == (test.order < 0));
        REQUIRE(compare(CompareOp::LessEqual, test.lhs, test.rhs, site) == (test.order <= 0));
        REQUIRE(compare(CompareOp::Greater, test.lhs, test.rhs, site) == (test.order > 0));
        REQUIRE(compare(CompareOp::GreaterEqual, test.lhs, test.rhs, site) == (test.order >= 0));
        REQUIRE(compare(CompareOp::Less, test.rhs, test.lhs, site) == (test.order > 0));
        REQUIRE(compare(CompareOp::Greater, test.rhs, test.lhs, site) == (test.order < 0));
    }

    RootedValue lhs(roots), rhs(roots), result(roots);
    make_text(heap, "\xee\x80\x80", lhs);     // U+E000
    make_text(heap, "\xf0\x90\x80\x80", rhs); // U+10000
    REQUIRE(compare(CompareOp::Less, lhs.get(), rhs.get(), site));
    make_text(heap, "a", lhs);
    // string_view 的显式长度保留内嵌 NUL。
    make_text(heap, std::string_view("a\0b", 3), rhs);
    REQUIRE(compare(CompareOp::Less, lhs.get(), rhs.get(), site));
    make_text(heap, "e\xcc\x81", lhs);
    make_text(heap, "\xc3\xa9", rhs);
    REQUIRE(!compare(CompareOp::Equal, lhs.get(), rhs.get(), site));
    REQUIRE(compare(CompareOp::Less, lhs.get(), rhs.get(), site));
    make_text(heap, "same", lhs);
    make_text(heap, "same", rhs);
    REQUIRE(std::get<StringRef>(lhs.get().data).ptr != std::get<StringRef>(rhs.get().data).ptr);
    REQUIRE(compare(CompareOp::Equal, lhs.get(), rhs.get(), site));

    RootedValue array_a(roots), array_b(roots), struct_a(roots), struct_b(roots);
    heap.make_array(0, array_a);
    heap.make_array(0, array_b);
    heap.make_struct(schema, struct_a);
    heap.make_struct(schema, struct_b);
    REQUIRE(compare(CompareOp::Equal, array_a.get(), array_a.get(), site));
    REQUIRE(!compare(CompareOp::Equal, array_a.get(), array_b.get(), site));
    REQUIRE(compare(CompareOp::Equal, struct_a.get(), struct_a.get(), site));
    REQUIRE(!compare(CompareOp::Equal, struct_a.get(), struct_b.get(), site));
    const Value categories[] = {Value{NullValue{}}, Value{true},   integer(1),    Value{1.0},
                                lhs.get(),          array_a.get(), struct_a.get()};
    for (std::size_t a = 0; a < std::size(categories); ++a) {
        for (std::size_t b = 0; b < std::size(categories); ++b) {
            const bool equal = a == b || (a == 2 && b == 3) || (a == 3 && b == 2);
            REQUIRE(compare(CompareOp::Equal, categories[a], categories[b], site) == equal);
            REQUIRE(compare(CompareOp::NotEqual, categories[a], categories[b], site) == !equal);
            const bool ordered_types = (a == 2 || a == 3) && (b == 2 || b == 3);
            if (!ordered_types && !(a == 4 && b == 4)) {
                for (auto op : {CompareOp::Less, CompareOp::LessEqual, CompareOp::Greater,
                                CompareOp::GreaterEqual}) {
                    rejects([&] { (void)ops.compare(op, categories[a], categories[b], site); },
                            ReasonCode::Type, span);
                }
            }
        }
    }
    REQUIRE(!compare(CompareOp::Equal, Value{false}, Value{true}, site));
    REQUIRE(!std::get<bool>(ops.unary(UnaryOp::Not, Value{true}, site).data));
    for (const auto &value : categories) {
        if (!std::holds_alternative<bool>(value.data))
            rejects([&] { (void)ops.unary(UnaryOp::Not, value, site); }, ReasonCode::Type, span);
    }

    for (const auto op : {BinaryOp::Add, BinaryOp::Subtract, BinaryOp::Multiply, BinaryOp::Divide,
                          BinaryOp::FloorDivide, BinaryOp::Modulo}) {
        rejects([&] { ops.binary(op, Value{true}, integer(1), site, result); }, ReasonCode::Type,
                span);
        rejects([&] { ops.binary(op, lhs.get(), integer(1), site, result); }, ReasonCode::Type,
                span);
        rejects([&] { ops.binary(op, integer(1), lhs.get(), site, result); }, ReasonCode::Type,
                span);
        rejects([&] { ops.binary(op, array_a.get(), array_b.get(), site, result); },
                ReasonCode::Type, span);
        rejects([&] { ops.binary(op, integer(1), Value{false}, site, result); }, ReasonCode::Type,
                span);
    }
    for (const auto op : {BinaryOp::FloorDivide, BinaryOp::Modulo}) {
        rejects([&] { ops.binary(op, integer(1), Value{1.0}, site, result); }, ReasonCode::Type,
                span);
        rejects([&] { ops.binary(op, Value{1.0}, integer(1), site, result); }, ReasonCode::Type,
                span);
    }
    rejects([&] { ops.binary(BinaryOp::Add, maximum, integer(1), site, result); },
            ReasonCode::Arithmetic, span);
    rejects([&] { ops.binary(BinaryOp::Subtract, minimum, integer(1), site, result); },
            ReasonCode::Arithmetic, span);
    rejects([&] { ops.binary(BinaryOp::Multiply, minimum, integer(-1), site, result); },
            ReasonCode::Arithmetic, span);
    rejects([&] { (void)ops.unary(UnaryOp::Minus, minimum, site); }, ReasonCode::Arithmetic, span);
    rejects([&] { ops.binary(BinaryOp::FloorDivide, minimum, integer(-1), site, result); },
            ReasonCode::Arithmetic, span);
    for (const auto zero : {Value{0.0}, Value{-0.0}, integer(0)})
        rejects([&] { ops.binary(BinaryOp::Divide, integer(1), zero, site, result); },
                ReasonCode::Arithmetic, span);
    for (const auto op : {BinaryOp::FloorDivide, BinaryOp::Modulo})
        rejects([&] { ops.binary(op, integer(1), integer(0), site, result); },
                ReasonCode::Arithmetic, span);
    for (const auto &test :
         {Ordered{integer(-7), integer(3), -3}, Ordered{integer(7), integer(-3), -3},
          Ordered{integer(-7), integer(-3), 2}}) {
        ops.binary(BinaryOp::FloorDivide, test.lhs, test.rhs, site, result);
        REQUIRE(std::get<std::int64_t>(result.get().data) == test.order);
    }
    ops.binary(BinaryOp::Modulo, minimum, integer(-1), site, result);
    REQUIRE(std::get<std::int64_t>(result.get().data) == 0);
    ops.binary(BinaryOp::Modulo, minimum, integer(3), site, result);
    REQUIRE(std::get<std::int64_t>(result.get().data) == 1);
    ops.binary(BinaryOp::Modulo, integer(7), integer(-3), site, result);
    REQUIRE(std::get<std::int64_t>(result.get().data) == -2);
    ops.binary(BinaryOp::Divide, integer(3), integer(2), site, result);
    REQUIRE(std::get<double>(result.get().data) == 1.5);
    ops.binary(BinaryOp::Add, integer(2), Value{0.5}, site, result);
    REQUIRE(std::get<double>(result.get().data) == 2.5);
    ops.binary(BinaryOp::Subtract, maximum, integer(1), site, result);
    REQUIRE(std::get<std::int64_t>(result.get().data) == 9223372036854775806LL);
    ops.binary(BinaryOp::Multiply, integer(-3), integer(7), site, result);
    REQUIRE(std::get<std::int64_t>(result.get().data) == -21);
    REQUIRE(std::get<std::int64_t>(ops.unary(UnaryOp::Plus, maximum, site).data) ==
            9223372036854775807LL);
    REQUIRE(std::signbit(std::get<double>(ops.unary(UnaryOp::Plus, Value{-0.0}, site).data)));
    ops.binary(BinaryOp::Divide, Value{-std::numeric_limits<double>::denorm_min()}, Value{2.0},
               site, result);
    REQUIRE(std::get<double>(result.get().data) == 0.0);
    REQUIRE(std::signbit(std::get<double>(result.get().data)));
    rejects(
        [&] {
            ops.binary(BinaryOp::Multiply, Value{std::numeric_limits<double>::max()}, Value{2.0},
                       site, result);
        },
        ReasonCode::Arithmetic, span);
}

// 缺少输入临时根、提前改写输出或按 C 字符串追加，会破坏受控回收下的拼接。
MYB_TEST(value_ops_string_roots) {
    EmergencyFailure first;
    ErrorSite site{first, {}};
    RootRegistry roots;
    Heap heap(roots, first);
    ValueOps ops(heap, roots);
    RootedValue lhs(roots), rhs(roots), out(roots);
    const std::string left(64, 'L');
    const std::string right("\xe5\x8c\x97\0\xf0\x9f\x98\x80", 8);
    make_text(heap, left, lhs);
    make_text(heap, right, rhs);
    const auto left_value = lhs.get(), right_value = rhs.get();
    lhs.detach();
    rhs.detach();
    TestHooks::force_gc(true);
    ops.binary(BinaryOp::Add, left_value, right_value, site, out);
    auto text = std::get<StringRef>(out.get().data);
    REQUIRE(text.ptr->utf8 == left + right);
    REQUIRE(text.ptr->codepoint_count == 67);
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == 1);
    ops.binary(BinaryOp::Add, out.get(), out.get(), site, out);
    text = std::get<StringRef>(out.get().data);
    REQUIRE(text.ptr->utf8 == left + right + left + right);
    REQUIRE(text.ptr->codepoint_count == 134);
    RootedValue empty(roots);
    make_text(heap, "", empty);
    ops.binary(BinaryOp::Add, empty.get(), out.get(), site, out);
    REQUIRE(std::get<StringRef>(out.get().data).ptr->utf8 == left + right + left + right);
    ops.binary(BinaryOp::Add, out.get(), empty.get(), site, out);
    REQUIRE(std::get<StringRef>(out.get().data).ptr->utf8 == left + right + left + right);
    empty.detach();
    heap.collect();
    const auto previous = out.get();
    for (auto point : {FaultPoint::Payload, FaultPoint::HeapObject}) {
        TestHooks::fail_next(point);
        bool caught = false;
        try {
            ops.binary(BinaryOp::Add, previous, previous, site, out);
        } catch (const HostFailure &failure) {
            caught = true;
            REQUIRE(failure.header().domain == FailureDomain::Resource);
            REQUIRE(failure.header().reason == ReasonCode::Allocation);
        }
        REQUIRE(caught);
        REQUIRE(std::get<StringRef>(out.get().data).ptr == std::get<StringRef>(previous.data).ptr);
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 1);
    }
    TestHooks::force_gc(false);
}

// 首个对象已占满登记表；若失败发布提前替换输出，原字符串将不可再用。
MYB_TEST(value_ops_string_publish_failure) {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap(roots, first);
    ValueOps ops(heap, roots);
    RootedValue out(roots);
    make_text(heap, "same", out);
    const auto before = out.get();
    TestHooks::force_gc(true);
    TestHooks::fail_next(FaultPoint::HeapRegistry);
    bool caught = false;
    try {
        ops.binary(BinaryOp::Add, before, before, {first, {}}, out);
    } catch (const HostFailure &failure) {
        caught = true;
        REQUIRE(failure.header().domain == FailureDomain::Resource);
        REQUIRE(failure.header().reason == ReasonCode::Allocation);
    }
    REQUIRE(caught);
    REQUIRE(std::get<StringRef>(out.get().data).ptr == std::get<StringRef>(before.data).ptr);
    REQUIRE(std::get<StringRef>(out.get().data).ptr->utf8 == "same");
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == 1);
    TestHooks::force_gc(false);
}
