#include "cli/session.hpp"
#include "fixtures.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include "vm/execution_index.hpp"
#include "vm/vm.hpp"
#include <functional>
#include <string>

using namespace myb;
namespace {
/** @brief 拥有查询测试的原始表，不填 name_ids，检验准备入口独立处理名称池。 */
struct IndexFixture {
    EmergencyFailure first;
    RootRegistry roots;
    CompiledProgram program{roots};
    BuiltinRegistry registry;
    ExecutionIndex index;
    IndexFixture() {
        program.names = {"f", "len", "Point", "missing", "Int64", "x", "y"};
        program.units.resize(3);
        program.functions = {{0, 0, 1, {}}};
        program.schemas.push_back(std::make_unique<StructSchema>(
            StructSchema{0, "Point", {{"x", {TypeKind::Int64}}, {"y", {TypeKind::Float64}}}}));
        program.units[0].constructions = {{2, {6, 5}}};
        program.units[1].constructions = {{2, {5, 6}}};
    }
    void prepare() { index.prepare(program, registry, first); }
};
/** @brief 为真实 Session 收集副作用和诊断；服务寿命覆盖全部借用。 */
class IndexIO final : public HostIO {
  public:
    std::string source, out, err;
    EntryBytes read_entry(std::string_view path) override { return {std::string(path), source}; }
    LineStatus read_line(Utf8Sink &) override { return LineStatus::End; }
    void read_text(std::string_view, Utf8Sink &) override { throw std::logic_error("unused"); }
    void write_text(std::string_view, std::string_view) override {
        throw std::logic_error("unused");
    }
    void write_stdout(std::string_view text, bool newline) override {
        out += text;
        if (newline)
            out += '\n';
    }
    void write_stderr(std::string_view text) override { err += text; }
};
/** @brief 通过已有输出根交付固定标量，不分配或触发 GC。 */
void eleven(NativeContext &, std::span<const Value>, RootedValue &out) {
    out.set(Value{std::int64_t{11}});
}
/** @brief 提供不同的内置身份，供重复登记的首项优先断言。 */
void twelve(NativeContext &, std::span<const Value>, RootedValue &out) {
    out.set(Value{std::int64_t{12}});
}
/** @brief 创建无关函数数量可变、实际循环工作量固定的调用程序。 */
std::string call_program(std::size_t unused, bool builtin) {
    std::string source;
    for (std::size_t i = 0; i < unused; ++i)
        source += "fn unused_" + std::to_string(i) + "(){}";
    source += "fn hot(){return 1;}let a=[1];let i=0;let total=0;while(i<1000){total=total+";
    source += builtin ? "len(a)" : "hot()";
    return source + ";i=i+1;}print(total);";
}
/** @brief 创建字段数量可变、重复构造次数固定的程序，字段名按固定宽度填写。 */
std::string field_program(std::size_t count) {
    std::string source = "struct Record{";
    for (std::size_t i = 0; i < count; ++i)
        source += "f" + std::to_string(i) + ":Int64;";
    source += "}let i=0;let total=0;while(i<16){let value=Record{";
    for (std::size_t i = 0; i < count; ++i) {
        if (i)
            source += ',';
        source += "f" + std::to_string(i) + ":1";
    }
    return source + "};total=total+value.f0;i=i+1;}print(total);";
}
} // namespace

MYB_TEST(execution_index_lookup) {
    IndexFixture fixture;
    fixture.prepare();
    const ErrorSite site{fixture.first, {}};
    REQUIRE(fixture.index.call(0, site).kind == CallTargetKind::User);
    REQUIRE(fixture.index.call(0, site).function_index == 0);
    REQUIRE(fixture.index.call(1, site).kind == CallTargetKind::Builtin);
    REQUIRE(fixture.index.call(1, site).builtin == fixture.registry.find("len"));
    REQUIRE(fixture.index.call(3, site).kind == CallTargetKind::Missing);
    REQUIRE(&fixture.index.schema(2, site) == fixture.program.schemas[0].get());

    const BuiltinDescriptor custom[] = {
        {"custom", 0, eleven}, {"custom", 0, twelve}, {"f", 0, twelve}};
    BuiltinRegistry registry(custom);
    fixture.program.names.push_back("custom");
    fixture.index.prepare(fixture.program, registry, fixture.first);
    REQUIRE(fixture.index.call(7, site).builtin->invoke == eleven);
    REQUIRE(fixture.index.call(0, site).kind == CallTargetKind::User);

    IndexIO io;
    EmergencyFailure first;
    auto descriptors =
        std::vector<BuiltinDescriptor>(builtin_descriptors().begin(), builtin_descriptors().end());
    for (auto &descriptor : descriptors)
        if (descriptor.name == "len")
            descriptor.invoke = nullptr;
    BuiltinRegistry empty_callback(descriptors);
    io.source = "len(write(\"argument\"));";
    Session session(first, io, empty_callback);
    REQUIRE(session.run({RunMode::Run, "index.myb"}) == 3);
    REQUIRE(io.out == "argument" && first.header().reason == ReasonCode::Invariant);
}

MYB_TEST(execution_index_manual_program) {
    IndexFixture fixture;
    fixture.program.names = {"f", "f", "T", "T"};
    fixture.program.functions = {{1, 0, 1, {}}, {1, 0, 2, {}}};
    fixture.program.schemas.clear();
    fixture.program.schemas.push_back(std::make_unique<StructSchema>(StructSchema{0, "T", {}}));
    fixture.program.schemas.push_back(std::make_unique<StructSchema>(StructSchema{1, "T", {}}));
    fixture.program.units[0].constructions.clear();
    fixture.program.units[1].constructions.clear();
    fixture.prepare();
    const ErrorSite site{fixture.first, {}};
    REQUIRE(fixture.program.name_ids.empty());
    REQUIRE(fixture.index.call(0, site).kind == CallTargetKind::Missing);
    REQUIRE(fixture.index.call(1, site).function_index == 0);
    REQUIRE(fixture.index.schema(2, site).id == 0 && fixture.index.schema(3, site).id == 0);
    fixture.index.clear();
    fixture.index.clear();
}

MYB_TEST(execution_index_invalid_metadata) {
    const std::function<void(CompiledProgram &)> damage[] = {
        [](auto &p) { p.functions[0].name = 999; },
        [](auto &p) { p.functions[0].unit = 999; },
        [](auto &p) { p.schemas[0].reset(); },
        [](auto &p) { p.schemas[0]->id = 99; },
        [](auto &p) { p.units[0].constructions[0].type = 999; },
        [](auto &p) { p.units[0].constructions[0].fields[0] = 999; },
        [](auto &p) { p.units[0].calls.push_back({999, 0}); },
        [](auto &p) { p.schemas[0]->fields.push_back(p.schemas[0]->fields[0]); }};
    for (const auto &action : damage) {
        IndexFixture fixture;
        action(fixture.program);
        bool failed = false;
        try {
            fixture.prepare();
        } catch (const Failure &failure) {
            failed = failure.header().domain == FailureDomain::Internal &&
                     failure.header().reason == ReasonCode::Invariant;
        }
        REQUIRE(failed);
    }
    IndexFixture fixture;
    fixture.prepare();
    const auto rejects = [&](auto operation) {
        bool failed = false;
        try {
            operation();
        } catch (const Failure &failure) {
            failed = failure.header().domain == FailureDomain::Internal;
        }
        REQUIRE(failed);
    };
    rejects([&] { (void)fixture.index.call(999, {fixture.first, {}}); });
    rejects([&] { (void)fixture.index.construction(999, 0, {fixture.first, {}}); });
    StructSchema foreign{0, "Point", {}};
    rejects([&] { (void)fixture.index.field_slot(foreign, 5, {fixture.first, {}}); });
}

MYB_TEST(execution_index_failure_cleanup) {
    for (const auto occurrence : {1u, 5u, 10u, 13u}) {
        IndexFixture old, next;
        old.prepare();
        TestHooks::fail_next(FaultPoint::ExecutionIndex, occurrence);
        bool failed = false;
        try {
            old.index.prepare(next.program, next.registry, next.first);
        } catch (const std::bad_alloc &) {
            failed = true;
        }
        TestHooks::clear_fault();
        REQUIRE(failed && next.first.header().domain == FailureDomain::Resource);
        REQUIRE(&old.index.schema(2, {old.first, {}}) == old.program.schemas[0].get());
    }
    IndexFixture fixture;
    Heap heap(fixture.roots, fixture.first);
    IndexIO io;
    std::vector<CallTraceEntry> trace;
    VM vm(heap, fixture.roots, io, fixture.first, fixture.registry, &trace);
    TestHooks::fail_next(FaultPoint::ExecutionIndex, 5);
    bool failed = false;
    try {
        vm.execute(fixture.program);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    REQUIRE(failed && exit_code(fixture.first.header()) == 3);
    REQUIRE(!fixture.first.header().span && trace.empty() && io.out.empty());
    TestHooks::clear_fault();
    fixture.first = EmergencyFailure{};
    CompiledProgram second(fixture.roots);
    second.names = {"print"};
    second.units.resize(1);
    auto &unit = second.units[0];
    unit.calls = {{0, 1}};
    unit.constants = {Value{std::int64_t{7}}};
    unit.instructions = {{Op::PRECALL, 0, 0},
                         {Op::CONST, 0, 0},
                         {Op::CALL, 0, 0},
                         {Op::DROP, 0, 0},
                         {Op::HALT, 0, 0}};
    unit.locations.resize(unit.instructions.size());
    unit.max_operand = 1;
    vm.execute(second);
    REQUIRE(io.out == "7\n" && !fixture.first.is_set());
}

MYB_TEST(execution_index_construction) {
    IndexFixture fixture;
    fixture.prepare();
    const ErrorSite site{fixture.first, {}};
    REQUIRE(fixture.index.construction(0, 0, site).status == ConstructionStatus::Ready);
    REQUIRE(fixture.index.construction(0, 0, site).schema == SchemaId{0});
    REQUIRE(fixture.index.construction(0, 0, site).slots == std::vector<std::uint32_t>({1, 0}));
    REQUIRE(fixture.index.construction(1, 0, site).slots == std::vector<std::uint32_t>({0, 1}));
    REQUIRE(fixture.index.field_slot(*fixture.program.schemas[0], 5, site) == 0);

    VmFixture vm;
    TestHooks::force_gc(true);
    const auto result =
        vm.run("struct A{left:Int64;right:Float64;}struct B{right:Int64;left:Int64;}"
               "struct Wrap{a:A;b:B;}fn first(){return A{right:2,left:1};}"
               "fn second(){return B{left:4,right:3};}"
               "let a=first();let b=second();a.right=5;b.left=6;print(Wrap{b:b,a:a});"
               "struct Marker{}print(Marker{}==Marker{});");
    TestHooks::force_gc(false);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_bytes ==
            "Wrap { a: A { left: 1, right: 5.0 }, b: B { right: 3, left: 6 } }\nfalse\n");
}

MYB_TEST(execution_index_deferred_errors) {
    const std::string prefix = "struct Pair{left:Int64;right:Float64;}";
    const std::pair<std::string, ReasonCode> cases[] = {
        {"Unknown{left:write(\"field\")};", ReasonCode::Name},
        {"Int64{left:write(\"field\")};", ReasonCode::Type},
        {"Pair{left:write(\"field\")};", ReasonCode::Type},
        {"Pair{left:write(\"field\"),left:1};", ReasonCode::Type},
        {"Pair{left:write(\"field\"),other:1};", ReasonCode::Type},
        {"Pair{left:\"bad\",right:write(\"field\")};", ReasonCode::Type}};
    for (const auto &[expression, reason] : cases) {
        VmFixture fixture;
        const auto checked = fixture.check(prefix + "write(\"before\");" + expression);
        REQUIRE(checked.exit_code == 0 && checked.stdout_bytes.empty());
        const auto run = fixture.run(prefix + "write(\"before\");" + expression);
        REQUIRE(run.exit_code == 1 && run.stdout_bytes == "before" &&
                run.failure->reason == reason);
        const auto skipped = fixture.run(prefix + "if(false){" + expression + "}write(\"ok\");");
        REQUIRE(skipped.exit_code == 0 && skipped.stdout_bytes == "ok");
    }
}

MYB_TEST(execution_index_call_scaling) {
    for (const bool builtin : {false, true}) {
        for (const auto unused : {0u, 128u, 1024u}) {
            TestHooks::reset_execution_work();
            VmFixture fixture;
            const auto result = fixture.run(call_program(unused, builtin));
            const auto work = TestHooks::execution_work();
            REQUIRE(result.exit_code == 0 && result.stdout_bytes == "1000\n");
            REQUIRE(work.calls > 0 && work.calls < 8000);
        }
    }
}

MYB_TEST(execution_index_field_scaling) {
    for (const auto fields : {64u, 128u, 256u}) {
        TestHooks::reset_execution_work();
        VmFixture fixture;
        const auto result = fixture.run(field_program(fields));
        const auto work = TestHooks::execution_work();
        REQUIRE(result.exit_code == 0 && result.stdout_bytes == "16\n");
        REQUIRE(work.fields > 0 && work.fields < 16 * 16 * fields);
        REQUIRE(work.writes == 16 * fields);
    }
}
