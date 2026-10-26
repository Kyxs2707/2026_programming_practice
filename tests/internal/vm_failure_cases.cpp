#include "bytecode/compiler.hpp"
#include "frontend/driver.hpp"
#include "runtime/decimal_codec.hpp"
#include "semantic/checker.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include "vm/vm.hpp"
#include <algorithm>
#include <memory>

using namespace myb;
namespace {
/** @brief 保存测试宿主状态，不为所观察的数组对象登记根。 */
class FailureIO final : public HostIO {
  public:
    FaultPoint point{FaultPoint::VmValues};
    std::size_t arms{}, entered{}, created{}, inspected{}, argument_collections{};
    std::vector<std::int64_t> results;
    EntryBytes read_entry(std::string_view) override {
        throw std::logic_error("unexpected entry read");
    }
    LineStatus read_line(Utf8Sink &) override {
        throw std::logic_error("unexpected input");
    }
    void read_text(std::string_view, Utf8Sink &) override {
        throw std::logic_error("unexpected file read");
    }
    void write_text(std::string_view, std::string_view) override {
        throw std::logic_error("unexpected file write");
    }
    void write_stdout(std::string_view, bool) override {
        throw std::logic_error("unexpected output");
    }
    void write_stderr(std::string_view) override {
        throw std::logic_error("unexpected diagnostic");
    }
};
/** @brief 在真实用户调用内设置下一次指定准备点失败。 */
void arm(NativeContext &context, std::span<const Value>, RootedValue &result) {
    auto &io = static_cast<FailureIO &>(context.io);
    ++io.arms;
    TestHooks::fail_next(io.point);
    result.set(Value{NullValue{}});
}
/** @brief 创建数组并在发布到结果根后回收，检验实参和结果同时可达。 */
void made(NativeContext &context, std::span<const Value>, RootedValue &result) {
    auto &io = static_cast<FailureIO &>(context.io);
    context.heap.collect();
    if (io.arms) {
        REQUIRE(TestHooks::heap_counts(context.heap).live == io.created);
        ++io.argument_collections;
    }
    context.heap.make_array(1, result);
    auto array = std::get<ArrayRef>(result.get().data);
    array.ptr->elements.push_back(Value{static_cast<std::int64_t>(++io.created)});
    context.heap.collect();
    REQUIRE(std::get<std::int64_t>(array.ptr->elements[0].data) ==
            static_cast<std::int64_t>(io.created));
}
/** @brief 标记目标函数实际进入；准备失败用例要求此函数从未执行。 */
void entered(NativeContext &context, std::span<const Value>, RootedValue &result) {
    ++static_cast<FailureIO &>(context.io).entered;
    result.set(Value{NullValue{}});
}
/** @brief 从仍受 VM 保护的参数返回同一数组，覆盖结果槽与参数槽重叠。 */
void echo(NativeContext &context, std::span<const Value> args, RootedValue &result) {
    context.heap.collect();
    REQUIRE(std::get<ArrayRef>(args[0].data).ptr->elements.size() == 1);
    result.set(args[0]);
}
/** @brief 在两个实参均可扫描时返回第二个，旧参数须先清除再放结果。 */
void second(NativeContext &context, std::span<const Value> args, RootedValue &result) {
    context.heap.collect();
    REQUIRE(TestHooks::heap_counts(context.heap).live == 2);
    REQUIRE(std::get<ArrayRef>(args[0].data).ptr != std::get<ArrayRef>(args[1].data).ptr);
    result.set(args[1]);
}
/** @brief 在消费者内强制回收，要求仅当前结果数组存活且内容完整。 */
void inspect(NativeContext &context, std::span<const Value> args, RootedValue &result) {
    context.heap.collect();
    REQUIRE(TestHooks::heap_counts(context.heap).live == 1);
    auto &io = static_cast<FailureIO &>(context.io);
    ++io.inspected;
    io.results.push_back(
        std::get<std::int64_t>(std::get<ArrayRef>(args[0].data).ptr->elements[0].data));
    result.set(Value{NullValue{}});
}
constexpr BuiltinDescriptor descriptors[] = {{"arm", 0, arm},         {"made", 0, made},
                                             {"entered", 0, entered}, {"echo", 1, echo},
                                             {"second", 2, second},   {"inspect", 1, inspect}};
/** @brief 独占测试会话，公开真实堆与 VM 供故障后的资源断言使用。 */
struct FailureFixture {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap{roots, first};
    SourceManager sources{first};
    CompiledProgram program{roots};
    FailureIO io;
    BuiltinRegistry registry{descriptors};
    std::vector<CallTraceEntry> trace;
    VM vm{heap, roots, io, first, registry, &trace};
    /** @brief 完成真实前端和全部编译阶段，随后释放 AST 与检查侧表。 */
    void compile(std::string source) {
        DecimalCodec::initialize(first);
        const auto id = sources.add("vm_failure.myb", std::move(source));
        auto ast = Driver(sources, id).parse();
        auto checked = Checker(first).check(*ast);
        BytecodeCompiler(heap, roots, first).compile(checked, program);
    }
};
/** @brief 模拟 VM 按有效状态扫描旧值区，预留槽不作为根。 */
void trace_store(void *context, Marker &marker) {
    auto &store = *static_cast<ValueStore *>(context);
    for (std::size_t i = 0; i < store.size(); ++i)
        if (store.state(i) == BindingState::Initialized)
            marker.mark(store.value(i));
}
/** @brief 在给定真实准备点失败，并核对活动链与清理后的对象数量。 */
void prepare_failure(FaultPoint point, std::int64_t arm_depth) {
    FailureFixture fixture;
    fixture.io.point = point;
    std::string source = "fn target(a,b){entered();";
    // 强制目标准备需要增长统一值区，父帧已有两个受管实参。
    for (int i = 0; i < 160; ++i)
        source += "let padding" + std::to_string(i) + ";";
    source += "return a;}fn recurse(n,p){if(n==" + std::to_string(arm_depth) +
              "){arm();return target(made(),made());}return recurse(n+1,p);}recurse(0,made());";
    fixture.compile(std::move(source));
    TestHooks::force_gc(true);
    bool failed = false;
    try {
        fixture.vm.execute(fixture.program);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    TestHooks::force_gc(false);
    REQUIRE(failed);
    REQUIRE(fixture.first.is_set());
    REQUIRE(fixture.first.header().domain == FailureDomain::Resource);
    REQUIRE(fixture.first.header().reason == ReasonCode::Allocation);
    REQUIRE(exit_code(fixture.first.header()) == 3);
    REQUIRE(fixture.first.header().span.has_value());
    REQUIRE(fixture.sources.slice(*fixture.first.header().span) == "target(made(),made())");
    REQUIRE(fixture.io.arms == 1);
    REQUIRE(fixture.io.entered == 0);
    REQUIRE(fixture.io.created == 3);
    REQUIRE(fixture.io.argument_collections == 2);
    REQUIRE(fixture.trace.size() == static_cast<std::size_t>(arm_depth + 1));
    for (const auto &entry : fixture.trace)
        REQUIRE(entry.name == "recurse");
    fixture.heap.collect();
    REQUIRE(TestHooks::heap_counts(fixture.heap).live == 0);
    fixture.vm.clear();
    fixture.heap.collect();
    REQUIRE(TestHooks::heap_counts(fixture.heap).live == 0);
}
} // namespace

MYB_TEST(vm_growth_failure_atomic_state) {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap(roots, first);
    ValueStore store;
    store.ensure(2, {first, {}});
    REQUIRE(store.size() >= 64);
    NativeRootSet provider(roots, &store, trace_store);
    ArrayObject *expected{};
    {
        RootedValue output(roots);
        heap.make_array(1, output);
        expected = std::get<ArrayRef>(output.get().data).ptr;
        expected->elements.push_back(Value{std::int64_t{42}});
        store.value(0) = output.get();
        store.state(0) = BindingState::Initialized;
    }
    const auto size = store.size();
    store.value(size - 1) = Value{std::int64_t{17}};
    store.state(size - 1) = BindingState::Initialized;
    store.state(1) = BindingState::Uninitialized;
    TestHooks::fail_next(FaultPoint::VmStates);
    bool failed = false;
    try {
        store.ensure(size + 1, {first, {}});
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    REQUIRE(failed);
    REQUIRE(first.is_set());
    REQUIRE(first.header().domain == FailureDomain::Resource);
    REQUIRE(exit_code(first.header()) == 3);
    REQUIRE(store.size() == size);
    REQUIRE(store.state(0) == BindingState::Initialized);
    REQUIRE(store.state(1) == BindingState::Uninitialized);
    REQUIRE(store.state(size - 1) == BindingState::Initialized);
    REQUIRE(std::get<ArrayRef>(store.value(0).data).ptr == expected);
    REQUIRE(std::get<std::int64_t>(store.value(size - 1).data) == 17);
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == 1);
    REQUIRE(std::get<std::int64_t>(expected->elements[0].data) == 42);
    store.ensure(size + 1, {first, {}});
    REQUIRE(store.size() >= size + 1);
    for (std::size_t i = size; i < store.size(); ++i) {
        REQUIRE(store.state(i) == BindingState::Dormant);
        REQUIRE(std::holds_alternative<NullValue>(store.value(i).data));
    }
    store.clear(0, store.size());
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == 0);
}

MYB_TEST(vm_call_failure_values) {
    prepare_failure(FaultPoint::VmValues, 2);
}
MYB_TEST(vm_call_failure_states) {
    prepare_failure(FaultPoint::VmStates, 2);
}
MYB_TEST(vm_call_failure_frames) {
    prepare_failure(FaultPoint::VmFrames, 14);
}

MYB_TEST(vm_return_roots) {
    FailureFixture fixture;
    fixture.compile(
        "fn relay(x){return "
        "echo(x);}inspect(relay(echo(made())));inspect(second(made(),made()));inspect(made());");
    TestHooks::force_gc(true);
    fixture.vm.execute(fixture.program);
    TestHooks::force_gc(false);
    REQUIRE(!fixture.first.is_set());
    REQUIRE(fixture.trace.empty());
    REQUIRE(fixture.io.results == std::vector<std::int64_t>({1, 3, 4}));
    REQUIRE(fixture.io.inspected == 3);
    fixture.heap.collect();
    REQUIRE(TestHooks::heap_counts(fixture.heap).live == 0);
}

MYB_TEST(vm_diagnostic_failure_preserves_first) {
    for (const auto reason : {ReasonCode::Name, ReasonCode::Arithmetic}) {
        FailureFixture fixture;
        fixture.io.point = FaultPoint::DiagnosticSnapshot;
        fixture.compile(reason == ReasonCode::Name
                            ? "fn outer(){return inner();}fn inner(){return missing;}arm();outer();"
                            : "fn outer(){return inner();}fn inner(){return 1//0;}arm();outer();");
        bool failed = false;
        try {
            fixture.vm.execute(fixture.program);
        } catch (const MybRuntimeFailure &failure) {
            failed = true;
            REQUIRE(failure.header().reason == reason);
        }
        REQUIRE(failed);
        REQUIRE(fixture.first.header().domain == FailureDomain::Runtime);
        REQUIRE(fixture.first.header().reason == reason);
        REQUIRE(exit_code(fixture.first.header()) == 1);
        REQUIRE(fixture.trace.empty());
        const auto location = fixture.sources.slice(*fixture.first.header().span);
        REQUIRE(location == (reason == ReasonCode::Name ? "missing" : "//"));
        fixture.heap.collect();
        REQUIRE(TestHooks::heap_counts(fixture.heap).live == 0);
    }
}
