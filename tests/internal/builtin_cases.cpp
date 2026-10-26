#include "builtins/functions.hpp"
#include "fixtures.hpp"
#include "runtime/text_builder.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include <array>
#include <limits>
#include <string_view>
#include <stdexcept>

using namespace myb;

MYB_TEST(builtins) {
    VmFixture fixture;
    const auto parsed = fixture.run(
        "print(try_int(\"bad\"));print(try_float(\"-1e-9999\"));print(int(-2.9));");
    REQUIRE(parsed.exit_code == 0);
    REQUIRE(parsed.stdout_bytes == "null\n-0.0\n-2\n");
    const auto assertion = fixture.run("try_int(\"bad\");assert(false,\"stop\");");
    REQUIRE(assertion.exit_code == 1);
    REQUIRE(assertion.failure->domain == FailureDomain::Runtime);
    REQUIRE(assertion.failure->reason == ReasonCode::Assertion);
    REQUIRE(assertion.stderr_bytes.find("stop") != std::string::npos);
    const auto message = fixture.run("assert(true,write(\"M\"));");
    REQUIRE(message.stdout_bytes == "M");
    REQUIRE(message.failure->reason == ReasonCode::Type);
    const auto roundtrip = fixture.run(
        "let x=-0.0;print(str(float(str(x))));"
        "print(float(\"1.00000000000000011102230246251565404236316680908203125\"));");
    REQUIRE(roundtrip.exit_code == 0);
    REQUIRE(roundtrip.stdout_bytes == "-0.0\n1.0\n");
}

MYB_TEST(builtins_registry) {
    constexpr std::array<std::string_view, 21> names{
        "print", "write", "len", "append", "insert", "pop", "remove_at", "deep_copy",
        "read_line", "int", "float", "str", "try_int", "try_float", "assert", "read_text",
        "write_text", "split", "join", "trim", "split_lines"};
    constexpr std::array<unsigned, 21> arities{1,1,1,2,3,1,2,1,0,1,1,1,1,1,2,1,2,2,2,1,1};
    const auto table = builtin_descriptors();
    REQUIRE(table.size() == names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        REQUIRE(table[i].name == names[i]);
        REQUIRE(table[i].arity == arities[i]);
        REQUIRE(table[i].invoke != nullptr);
        for (std::size_t j = 0; j < i; ++j)
            REQUIRE(table[j].name != table[i].name);
    }
}

MYB_TEST(builtins_text_gc) {
    struct Reset {
        ~Reset() { TestHooks::force_gc(false); TestHooks::clear_fault(); }
    } reset;
    TestHooks::force_gc(true);
    VmFixture fixture;
    // 使用独立程序使每个允许分配点回收时，片段和最终返回值都仍有效。
    const auto simple = fixture.run(
        "let a=split(\"a,,b,\",\",\");print(join(a,\"::\"));"
        "print(split_lines(\"a\\r\\n\\r\\nb\\r\"));print(trim(\" \\t北邮 \\t\"));");
    REQUIRE(simple.exit_code == 0);
    REQUIRE(simple.stdout_bytes == "a::::b::\n[\"a\", \"\", \"b\\r\"]\n北邮\n");
}

namespace {
class UnusedIO final : public HostIO {
  public:
    EntryBytes read_entry(std::string_view) override { throw std::logic_error("unused I/O"); }
    LineStatus read_line(Utf8Sink &) override { throw std::logic_error("unused I/O"); }
    void read_text(std::string_view, Utf8Sink &) override { throw std::logic_error("unused I/O"); }
    void write_text(std::string_view, std::string_view) override { throw std::logic_error("unused I/O"); }
    void write_stdout(std::string_view, bool) override { throw std::logic_error("unused I/O"); }
    void write_stderr(std::string_view) override { throw std::logic_error("unused I/O"); }
};
void make_text(Heap &heap, std::string_view text, RootedValue &out) {
    TextBuilder builder(heap);
    builder.append({text.data(), text.size()});
    heap.make_string(std::move(builder), out);
}
}

MYB_TEST(builtins_text_failures) {
    struct Reset {
        ~Reset() { TestHooks::force_gc(false); TestHooks::clear_fault(); }
    } reset;
    for (auto point : {FaultPoint::HeapObject, FaultPoint::Payload, FaultPoint::MarkWorklist}) {
        EmergencyFailure first;
        RootRegistry roots;
        Heap heap(roots, first);
        UnusedIO io;
        NativeContext context{heap, roots, io, first, {0, 0, 1}};
        RootedValue source(roots), separator(roots), out(roots, Value{std::int64_t{42}});
        make_text(heap, "a,b,c", source);
        make_text(heap, ",", separator);
        const Value args[]{source.get(), separator.get()};
        const auto original = TestHooks::heap_counts(heap).live;
        TestHooks::force_gc(point == FaultPoint::MarkWorklist);
        TestHooks::fail_next(point);
        bool failed = false;
        try {
            builtin_split(context, args, out);
        } catch (const Failure &failure) {
            failed = failure.header().domain == FailureDomain::Resource &&
                     failure.header().reason == ReasonCode::Allocation;
        }
        TestHooks::clear_fault();
        TestHooks::force_gc(false);
        REQUIRE(failed);
        REQUIRE(first.header().domain == FailureDomain::Resource);
        REQUIRE(std::get<std::int64_t>(out.get().data) == 42);
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == original);
        REQUIRE(std::get<StringRef>(source.get().data).ptr->utf8 == "a,b,c");
    }
}
