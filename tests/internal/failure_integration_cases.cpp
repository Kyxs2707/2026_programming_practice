#include "bytecode/compiler.hpp"
#include "frontend/driver.hpp"
#include "frontend/flex_allocations.hpp"
#include "runtime/decimal_codec.hpp"
#include "runtime/display.hpp"
#include "runtime/graph_copy.hpp"
#include "runtime/text_builder.hpp"
#include "semantic/checker.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include "vm/vm.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <utility>

using namespace myb;

namespace {
/** @brief 在测试任何出口恢复进程内故障开关，避免失败影响后续用例。 */
struct FaultScope {
    ~FaultScope() {
        TestHooks::clear_fault();
        TestHooks::force_gc(false);
    }
};
ArrayRef array(const RootedValue &root) {
    return std::get<ArrayRef>(root.get().data);
}

/** @brief 核对回收异常后所有对象仍可安全清理，且首错保持指定分类。 */
void require_resource(const EmergencyFailure &first, const Heap &heap, std::size_t live) {
    REQUIRE(first.is_set());
    REQUIRE(first.header().domain == FailureDomain::Resource);
    REQUIRE(first.header().reason == ReasonCode::Allocation);
    REQUIRE(exit_code(first.header()) == 3);
    const auto counts = TestHooks::heap_counts(heap);
    REQUIRE(counts.live == live);
    REQUIRE(counts.marked == 0);
    REQUIRE(!counts.collecting);
}

/** @brief 保存当前数组的独立根及设置故障所需的信息，寿命覆盖 VM 执行。 */
class IntegrationIO final : public HostIO {
  public:
    RootedValue *retained{};
    VM *vm{};
    FaultPoint point{FaultPoint::HeapObject};
    bool armed{true};
    std::size_t observed{}, entered{}, arm_count{};
    EntryBytes read_entry(std::string_view) override { throw std::logic_error("unexpected read"); }
    LineStatus read_line(Utf8Sink &) override { throw std::logic_error("unexpected input"); }
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

/** @brief 在活动遍历中把来源交给外部根，再安排真实后续操作失败。 */
void retain_and_arm(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    auto &io = static_cast<IntegrationIO &>(context.io);
    io.retained->set(args[0]);
    io.observed = std::get<ArrayRef>(args[0].data).ptr->active_iteration_count;
    REQUIRE(io.observed > 0);
    REQUIRE(TestHooks::active_iteration_count(*io.vm) == io.observed);
    REQUIRE(!context.failure.is_set());
    ++io.arm_count;
    if (io.armed) {
        TestHooks::fail_next(io.point);
        if (io.point == FaultPoint::MarkWorklist)
            TestHooks::force_gc(true);
    }
    out.set(Value{NullValue{}});
}

/** @brief 检验调用准备失败的目标函数尚未进入。 */
void count_entered(NativeContext &context, std::span<const Value>, RootedValue &out) {
    ++static_cast<IntegrationIO &>(context.io).entered;
    out.set(Value{NullValue{}});
}

/**
 * @brief 保留前端、编译程序、堆和外部数组根，供 VM 清理后继续观察。
 * VM 及外部根先于堆销毁，堆先于程序结构元数据销毁。
 */
struct IntegrationFixture {
    EmergencyFailure first;
    RootRegistry roots;
    SourceManager sources{first};
    CompiledProgram program{roots};
    Heap heap{roots, first};
    RootedValue retained{roots};
    IntegrationIO io;
    std::vector<BuiltinDescriptor> descriptors;
    BuiltinRegistry registry;
    std::vector<CallTraceEntry> trace;
    VM vm;
    IntegrationFixture()
        : descriptors(builtin_descriptors().begin(), builtin_descriptors().end()),
          registry(prepare()), vm(heap, roots, io, first, registry, &trace) {
        io.retained = &retained;
        io.vm = &vm;
    }
    std::span<const BuiltinDescriptor> prepare() {
        descriptors.push_back({"retain_and_arm", 1, retain_and_arm});
        descriptors.push_back({"count_entered", 0, count_entered});
        return descriptors;
    }
    /** @brief 完成真实前端与编译，在执行之前释放 AST 及检查侧表。 */
    void compile(std::string source) {
        DecimalCodec::initialize(first);
        const auto id = sources.add("failure-integration.myb", std::move(source));
        auto ast = Driver(sources, id).parse();
        auto checked = Checker(first).check(*ast);
        BytecodeCompiler(heap, roots, first).compile(checked, program);
    }
    /** @brief 在堆仍持有同一数组时验证解除计数，并核对根注销的幂等性。 */
    void require_released() {
        REQUIRE(io.arm_count == 1);
        REQUIRE(io.observed > 0);
        REQUIRE(array(retained).ptr->active_iteration_count == 0);
        REQUIRE(array(retained).ptr->elements.size() == 1);
        REQUIRE(std::get<std::int64_t>(array(retained).ptr->elements[0].data) == 7);
        REQUIRE(TestHooks::active_iteration_count(vm) == 0);
        REQUIRE(TestHooks::heap_counts(heap).active_arrays == 0);
        vm.clear();
        vm.clear();
        TestHooks::clear_fault();
        TestHooks::force_gc(false);
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 1);
        REQUIRE(array(retained).ptr->active_iteration_count == 0);
        retained.detach();
        retained.detach();
        program.detach_roots();
        program.detach_roots();
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 0);
    }
};

/** @brief 构造共享循环链，保存原地址仅作比较；head 是整个图的保活根。 */
std::vector<ArrayObject *> copy_graph(Heap &heap, RootRegistry &roots, RootedValue &head) {
    std::vector<ArrayObject *> nodes;
    RootedValue tail(roots), next(roots);
    heap.make_array(2, head);
    tail.set(head.get());
    for (std::int64_t index = 0; index < 6; ++index) {
        nodes.push_back(array(tail).ptr);
        array(tail).ptr->elements.push_back(Value{index});
        if (index == 5) {
            array(tail).ptr->elements.push_back(head.get());
        } else {
            heap.make_array(2, next);
            array(tail).ptr->elements.push_back(next.get());
            tail.set(next.get());
        }
    }
    return nodes;
}

/** @brief 检验失败没有修改原图的全部边与标量，遍历只借用 head 保活的地址。 */
void require_original_graph(const std::vector<ArrayObject *> &nodes) {
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        REQUIRE(nodes[index]->elements.size() == 2);
        REQUIRE(std::get<std::int64_t>(nodes[index]->elements[0].data) ==
                static_cast<std::int64_t>(index));
        REQUIRE(std::get<ArrayRef>(nodes[index]->elements[1].data).ptr ==
                nodes[(index + 1) % nodes.size()]);
    }
}
} // namespace

MYB_TEST(failure_selected_hit) {
    FaultScope scope;
    TestHooks::fail_next(FaultPoint::NativeWorkspace, 3);
    TestHooks::hit(FaultPoint::Payload);
    bool early = false;
    try {
        TestHooks::hit(FaultPoint::NativeWorkspace);
        TestHooks::hit(FaultPoint::NativeWorkspace);
    } catch (const std::bad_alloc &) {
        early = true;
    }
    REQUIRE(!early);
    bool failed = false;
    try {
        TestHooks::hit(FaultPoint::NativeWorkspace);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    REQUIRE(failed);
    TestHooks::hit(FaultPoint::NativeWorkspace);
}

MYB_TEST(failure_integration) {
    FaultScope scope;
    for (const auto point : {FaultPoint::HeapObject, FaultPoint::HeapRegistry,
                             FaultPoint::Payload}) {
        EmergencyFailure first;
        RootRegistry roots;
        Heap heap(roots, first);
        RootedValue out(roots, Value{std::int64_t{42}});
        TestHooks::fail_next(point);
        bool failed = false;
        try {
            heap.make_array(4, out);
        } catch (const HostFailure &failure) {
            failed = failure.header().reason == ReasonCode::Allocation;
        }
        REQUIRE(failed);
        REQUIRE(std::get<std::int64_t>(out.get().data) == 42);
        require_resource(first, heap, 0);
        out.detach();
        out.detach();
        heap.collect();
        require_resource(first, heap, 0);
    }
    {
        EmergencyFailure first;
        RootRegistry roots;
        Heap heap(roots, first);
        RootedValue head(roots), child(roots);
        heap.make_array(4, head);
        for (int index = 0; index < 4; ++index) {
            heap.make_array(0, child);
            array(head).ptr->elements.push_back(child.get());
        }
        // 最后创建的对象没有根；不完整标记不得顺便清除这个对象。
        heap.make_array(0, child);
        child.set(Value{NullValue{}});
        TestHooks::fail_next(FaultPoint::MarkWorklist, 2);
        bool failed = false;
        try {
            heap.collect();
        } catch (const HostFailure &) {
            failed = true;
        }
        REQUIRE(failed);
        require_resource(first, heap, 6);
        heap.collect();
        require_resource(first, heap, 5);
        head.detach();
        head.detach();
        heap.collect();
        require_resource(first, heap, 0);
    }
    for (std::size_t occurrence : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
        EmergencyFailure first;
        SourceManager sources(first);
        const auto id = sources.add("partial-scanner.myb", "let a=[1,2];");
        TestHooks::fail_next(FaultPoint::FlexAllocation, occurrence);
        bool failed = false;
        try {
            Driver(sources, id).parse();
        } catch (const HostFailure &failure) {
            failed = failure.header().domain == FailureDomain::Resource &&
                     failure.header().reason == ReasonCode::Allocation;
        }
        REQUIRE(failed);
        REQUIRE(FlexAllocationLedger::live_total() == 0);
        REQUIRE(first.header().domain == FailureDomain::Resource);
        REQUIRE(exit_code(first.header()) == 3);
    }
    {
        EmergencyFailure first;
        RootRegistry roots;
        SourceManager sources(first);
        CompiledProgram program(roots);
        Heap heap(roots, first);
        DecimalCodec::initialize(first);
        const auto id = sources.add("partial-constants.myb", "\"kept\";\"pending\";");
        auto ast = Driver(sources, id).parse();
        auto checked = Checker(first).check(*ast);
        TestHooks::force_gc(true);
        TestHooks::fail_next(FaultPoint::ProgramConstants, 2);
        bool failed = false;
        try {
            BytecodeCompiler(heap, roots, first).compile(checked, program);
        } catch (const std::bad_alloc &) {
            failed = true;
        }
        REQUIRE(failed);
        REQUIRE(program.units[0].constants.size() == 1);
        REQUIRE(std::get<StringRef>(program.units[0].constants[0].data).ptr->utf8 == "kept");
        ast.reset();
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 1);
        program.detach_roots();
        program.detach_roots();
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 0);
    }
}

MYB_TEST(failure_native_workspace) {
    FaultScope scope;
    // 第2/4/8次为映射已登记后的队列增长，第7次已有三个副本完成登记。
    for (const auto &[occurrence, copies] :
         std::array<std::pair<std::size_t, std::size_t>, 4>{{{2, 1}, {4, 2}, {7, 4}, {8, 4}}}) {
        EmergencyFailure first;
        RootRegistry roots;
        Heap heap(roots, first);
        RootedValue source(roots), out(roots, Value{std::int64_t{42}});
        const auto nodes = copy_graph(heap, roots, source);
        first.record_first({FailureDomain::Runtime, ReasonCode::Type, SourceSpan{0, 3, 4}, 77});
        TestHooks::force_gc(true);
        TestHooks::fail_next(FaultPoint::NativeWorkspace, occurrence);
        bool failed = false;
        try {
            deep_copy(heap, roots, source.get(), out);
        } catch (const std::bad_alloc &) {
            failed = true;
        }
        REQUIRE(failed);
        REQUIRE(std::get<std::int64_t>(out.get().data) == 42);
        REQUIRE(TestHooks::heap_counts(heap).live == nodes.size() + copies);
        REQUIRE(first.header().domain == FailureDomain::Runtime);
        REQUIRE(first.header().reason == ReasonCode::Type);
        REQUIRE(first.header().span->begin == 3 && first.header().os_code == 77);
        REQUIRE(exit_code(first.header()) == 1);
        require_original_graph(nodes);
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == nodes.size());
        require_original_graph(nodes);
        source.detach();
        source.detach();
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 0);
    }
    // 第7次在三层已进入后增长栈，第8次在同样深度登记路径。
    for (const auto &[occurrence, opened] :
         std::array<std::pair<std::size_t, std::size_t>, 3>{{{7, 3}, {8, 3}, {10, 4}}}) {
        for (bool prior_failure : {false, true}) {
            EmergencyFailure first;
            RootRegistry roots;
            Heap heap(roots, first);
            RootedValue head(roots), tail(roots), next(roots);
            heap.make_array(1, head);
            tail.set(head.get());
            for (int depth = 1; depth < 6; ++depth) {
                heap.make_array(1, next);
                array(tail).ptr->elements.push_back(next.get());
                tail.set(next.get());
            }
            array(tail).ptr->elements.push_back(Value{std::int64_t{17}});
            tail.set(Value{NullValue{}});
            next.set(Value{NullValue{}});
            if (prior_failure)
                first.record_first({FailureDomain::Runtime, ReasonCode::Type, {}, 91});
            {
                TextBuilder output(heap);
                constexpr std::string_view prefix = "previous:";
                output.append({prefix.data(), prefix.size()});
                TestHooks::force_gc(true);
                TestHooks::fail_next(FaultPoint::NativeWorkspace, occurrence);
                bool failed = false;
                try {
                    display(head.get(), output, {first, SourceSpan{0, 2, 3}});
                } catch (const HostFailure &failure) {
                    failed = failure.header().domain == FailureDomain::Resource &&
                             failure.header().reason == ReasonCode::Allocation;
                }
                REQUIRE(failed);
                REQUIRE(output.bytes() == std::string(prefix) + std::string(opened, '['));
                REQUIRE(first.header().domain ==
                        (prior_failure ? FailureDomain::Runtime : FailureDomain::Resource));
                REQUIRE(first.header().reason ==
                        (prior_failure ? ReasonCode::Type : ReasonCode::Allocation));
                REQUIRE(exit_code(first.header()) == (prior_failure ? 1 : 3));
                heap.collect();
                REQUIRE(TestHooks::heap_counts(heap).live == 6);
            }
            // 失败的输出 Builder 和路径工作区均已销毁，仍可完整显示原图。
            {
                TextBuilder output(heap);
                display(head.get(), output, {first, {}});
                REQUIRE(output.bytes() == "[[[[[[17]]]]]]");
            }
            head.detach();
            head.detach();
            heap.collect();
            REQUIRE(TestHooks::heap_counts(heap).live == 0);
        }
    }
}

MYB_TEST(failure_iteration_external_root) {
    FaultScope scope;
    for (const auto point : {FaultPoint::HeapObject, FaultPoint::HeapRegistry,
                             FaultPoint::Payload, FaultPoint::MarkWorklist,
                             FaultPoint::NativeWorkspace}) {
        IntegrationFixture fixture;
        fixture.io.point = point;
        fixture.compile(std::string("fn broken(a){for(let x in a){for(let y in a){") +
                        "retain_and_arm(a);" +
                        (point == FaultPoint::NativeWorkspace ? "deep_copy(a);" : "let b=[x];") +
                        "}}}broken([7]);");
        bool failed = false;
        try {
            fixture.vm.execute(fixture.program);
        } catch (const std::bad_alloc &) {
            failed = true;
        } catch (const HostFailure &failure) {
            failed = failure.header().domain == FailureDomain::Resource;
        }
        REQUIRE(failed);
        REQUIRE(fixture.io.observed == 2);
        REQUIRE(fixture.first.header().domain == FailureDomain::Resource);
        REQUIRE(fixture.first.header().reason == ReasonCode::Allocation);
        REQUIRE(fixture.first.header().span.has_value());
        REQUIRE(fixture.sources.slice(*fixture.first.header().span) ==
                (point == FaultPoint::NativeWorkspace ? "deep_copy(a)" : "[x]"));
        fixture.require_released();
        REQUIRE(fixture.first.header().reason == ReasonCode::Allocation);
    }
}

MYB_TEST(failure_iteration_vm_prepare) {
    FaultScope scope;
    for (const auto point : {FaultPoint::VmValues, FaultPoint::VmStates, FaultPoint::VmFrames,
                             FaultPoint::IterationRecords}) {
        IntegrationFixture fixture;
        fixture.io.point = point;
        if (point == FaultPoint::VmValues || point == FaultPoint::VmStates) {
            std::string source = "fn target(a){count_entered();";
            for (int index = 0; index < 160; ++index)
                source += "let padding" + std::to_string(index) + ";";
            source += "}fn outer(a){for(let x in a){retain_and_arm(a);target(a);}}outer([7]);";
            fixture.compile(std::move(source));
        } else {
            const auto arm_depth = point == FaultPoint::VmFrames ? "14" : "15";
            const auto final_depth = point == FaultPoint::VmFrames ? "15" : "16";
            fixture.compile(std::string("fn walk(n,a){for(let x in a){if(n==") + arm_depth +
                            "){retain_and_arm(a);}if(n<" + final_depth +
                            "){walk(n+1,a);}}}walk(0,[7]);");
        }
        bool failed = false;
        try {
            fixture.vm.execute(fixture.program);
        } catch (const std::bad_alloc &) {
            failed = true;
        }
        REQUIRE(failed);
        REQUIRE(fixture.io.entered == 0);
        REQUIRE(fixture.first.header().domain == FailureDomain::Resource);
        REQUIRE(fixture.first.header().reason == ReasonCode::Allocation);
        REQUIRE(fixture.first.header().span.has_value());
        const auto expected_site = point == FaultPoint::IterationRecords
                                       ? "a"
                                       : point == FaultPoint::VmFrames ? "walk(n+1,a)" : "target(a)";
        REQUIRE(fixture.sources.slice(*fixture.first.header().span) == expected_site);
        REQUIRE(fixture.trace.size() == (point == FaultPoint::VmFrames          ? 15u
                                         : point == FaultPoint::IterationRecords ? 17u : 1u));
        REQUIRE(std::none_of(fixture.trace.begin(), fixture.trace.end(),
                             [](const auto &entry) { return entry.name == "target"; }));
        fixture.require_released();
        REQUIRE(fixture.first.header().reason == ReasonCode::Allocation);
    }
}

MYB_TEST(failure_iteration_diagnostic) {
    FaultScope scope;
    for (const bool diagnostic_failure : {false, true}) {
        IntegrationFixture fixture;
        fixture.io.point = FaultPoint::DiagnosticSnapshot;
        fixture.io.armed = diagnostic_failure;
        fixture.compile("fn broken(a){for(let x in a){for(let y in a){"
                        "retain_and_arm(a);1/0;}}}broken([7]);");
        bool failed = false;
        try {
            fixture.vm.execute(fixture.program);
        } catch (const MybRuntimeFailure &failure) {
            failed = failure.header().reason == ReasonCode::Arithmetic;
        }
        REQUIRE(failed);
        REQUIRE(fixture.first.header().domain == FailureDomain::Runtime);
        REQUIRE(fixture.first.header().reason == ReasonCode::Arithmetic);
        REQUIRE(fixture.first.header().span.has_value());
        REQUIRE(fixture.sources.slice(*fixture.first.header().span) == "/");
        REQUIRE(exit_code(fixture.first.header()) == 1);
        REQUIRE(fixture.trace.size() == (diagnostic_failure ? 0u : 1u));
        fixture.require_released();
        REQUIRE(fixture.first.header().reason == ReasonCode::Arithmetic);
    }
}
