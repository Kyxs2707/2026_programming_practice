#include "bytecode/compiler.hpp"
#include "fixtures.hpp"
#include "frontend/driver.hpp"
#include "runtime/decimal_codec.hpp"
#include "semantic/checker.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include "vm/vm.hpp"

using namespace myb;
namespace {
/** @brief 保存实际执行时的输出和观察次数，借用 VM 至该次测试结束。 */
class IterationIO final : public HostIO {
  public:
    VM *vm{};
    FaultPoint point{FaultPoint::IterationRecords};
    std::size_t arms{}, observations{};
    std::string out;
    EntryBytes read_entry(std::string_view) override { throw std::logic_error("unexpected read"); }
    LineStatus read_line(Utf8Sink &) override { throw std::logic_error("unexpected input"); }
    void read_text(std::string_view, Utf8Sink &) override { throw std::logic_error("unexpected read"); }
    void write_text(std::string_view, std::string_view) override {
        throw std::logic_error("unexpected write");
    }
    void write_stdout(std::string_view text, bool newline) override {
        out += text;
        if (newline)
            out += '\n';
    }
    void write_stderr(std::string_view) override { throw std::logic_error("unexpected diagnostic"); }
};
/** @brief 在循环体已提交状态下安排后续真实分配失败，不修改已有首错。 */
void arm_iteration(NativeContext &context, std::span<const Value>, RootedValue &out) {
    auto &io = static_cast<IterationIO &>(context.io);
    ++io.arms;
    REQUIRE(TestHooks::active_iteration_count(*io.vm) > 0);
    REQUIRE(!context.failure.is_set());
    TestHooks::fail_next(io.point);
    out.set(Value{NullValue{}});
}
/** @brief 在 native 内回收并核对来源数组计数；输入与来源记录均由 VM 保活。 */
void inspect_iteration(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    context.heap.collect();
    auto &io = static_cast<IterationIO &>(context.io);
    ++io.observations;
    const auto expected = static_cast<std::size_t>(std::get<std::int64_t>(args[1].data));
    REQUIRE(std::get<ArrayRef>(args[0].data).ptr->active_iteration_count == expected);
    REQUIRE(TestHooks::active_iteration_count(*io.vm) >= expected);
    out.set(Value{NullValue{}});
}
/** @brief 回收已退出轮次的局部对象，按调用参数核对仍可达的数组数量。 */
void inspect_iteration_roots(NativeContext &context, std::span<const Value> args, RootedValue &out) {
    context.heap.collect();
    REQUIRE(TestHooks::heap_counts(context.heap).live ==
            static_cast<std::size_t>(std::get<std::int64_t>(args[0].data)));
    out.set(Value{NullValue{}});
}
/** @brief 拥有真实前端、堆和 VM，执行后继续检查活动记录和数组状态。 */
struct IterationFixture {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap{roots, first};
    SourceManager sources{first};
    CompiledProgram program{roots};
    IterationIO io;
    std::vector<BuiltinDescriptor> descriptors;
    BuiltinRegistry registry;
    std::vector<CallTraceEntry> trace;
    VM vm;
    IterationFixture()
        : descriptors(builtin_descriptors().begin(), builtin_descriptors().end()),
          registry(prepare()), vm(heap, roots, io, first, registry, &trace) { io.vm = &vm; }
    /** @brief 完成描述表后交给只读登记表，返回视图的寿命覆盖 registry。 */
    std::span<const BuiltinDescriptor> prepare() {
        descriptors.push_back({"arm_iteration", 0, arm_iteration});
        descriptors.push_back({"inspect_iteration", 2, inspect_iteration});
        descriptors.push_back({"inspect_iteration_roots", 1, inspect_iteration_roots});
        return descriptors;
    }
    /** @brief 编译后释放语法与检查对象，仅保留完整拥有的编译程序。 */
    void compile(std::string source) {
        DecimalCodec::initialize(first);
        auto id = sources.add("vm_iteration.myb", std::move(source));
        auto ast = Driver(sources, id).parse();
        auto checked = Checker(first).check(*ast);
        BytecodeCompiler(heap, roots, first).compile(checked, program);
    }
    /** @brief 核对所有解除操作幂等，回收后只剩编译程序保护的常量。 */
    void require_clean(std::size_t constants = 0) {
        REQUIRE(TestHooks::active_iteration_count(vm) == 0);
        REQUIRE(TestHooks::heap_counts(heap).active_arrays == 0);
        vm.clear();
        REQUIRE(TestHooks::active_iteration_count(vm) == 0);
        REQUIRE(TestHooks::heap_counts(heap).active_arrays == 0);
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == constants);
    }
};
} // namespace

MYB_TEST(vm_iteration) {
    TestHooks::force_gc(true);
    VmFixture fixture;
    auto result = fixture.run(
        "fn pick(a){for(let x in a){let r=[x];if(x==1){continue;}return r;}return [];}"
        "let a=[1,2];let r=pick(a);append(a,3);print(r[0]);print(len(a));");
    REQUIRE(result.exit_code == 0 && result.stdout_bytes == "2\n3\n");
    result = fixture.run("let a=[1];for(let x in a){for(let y in a){break;}append(a,2);}");
    REQUIRE(result.exit_code == 1 && result.failure->reason == ReasonCode::IterationMutation);
    result = fixture.run("for(let x in [1,2]){let y;if(x==1){y=7;continue;}print(y);}");
    REQUIRE(result.exit_code == 1 && result.failure->reason == ReasonCode::Uninitialized);
    result = fixture.run("let a=[1,2];for(let x in a){a=[];print(x);}"
                         "for(let c in \"北邮😀\"){print(c);}");
    REQUIRE(result.exit_code == 0 && result.stdout_bytes == "1\n2\n北\n邮\n😀\n");
    TestHooks::force_gc(false);
}

MYB_TEST(vm_iteration_roots) {
    IterationFixture fixture;
    fixture.compile(
        "let a=[1,2];for(let x in a){inspect_iteration(a,1);"
        "for(let y in a){inspect_iteration(a,2);break;}inspect_iteration(a,1);"
        "inspect_iteration_roots(1);let local=[x];"
        "while(true){let w=[x];break;}if(x==1){continue;}break;}"
        "append(a,3);inspect_iteration_roots(1);"
        "fn one(){return [1,2];}for(let x in one()){inspect_iteration_roots(2);}"
        "inspect_iteration_roots(1);");
    TestHooks::force_gc(true);
    fixture.vm.execute(fixture.program);
    TestHooks::force_gc(false);
    REQUIRE(fixture.io.observations == 6);
    fixture.require_clean();
    IterationFixture strings;
    strings.compile("let s=\"北\"+\"邮\";for(let c in s){s=null;"
                    "inspect_iteration_roots(4);}inspect_iteration_roots(2);");
    TestHooks::force_gc(true);
    strings.vm.execute(strings.program);
    TestHooks::force_gc(false);
    strings.require_clean(2);
}

MYB_TEST(vm_iteration_failures) {
    // 第17次同时活动的记录触发向量增长，之前16个数组计数须一并解除。
    {
        IterationFixture fixture;
        fixture.compile("fn walk(n){let a=[n];for(let x in a){if(n==15){arm_iteration();}"
                        "if(n<16){walk(n+1);}}}walk(0);");
        TestHooks::force_gc(true);
        bool caught = false;
        try { fixture.vm.execute(fixture.program); }
        catch (const std::bad_alloc &) { caught = true; }
        TestHooks::force_gc(false);
        TestHooks::clear_fault();
        REQUIRE(caught && fixture.io.arms == 1);
        REQUIRE(fixture.first.header().reason == ReasonCode::Allocation);
        REQUIRE(fixture.first.header().domain == FailureDomain::Resource);
        REQUIRE(fixture.first.header().span.has_value());
        REQUIRE(fixture.sources.slice(*fixture.first.header().span) == "a");
        REQUIRE(fixture.trace.size() == 17);
        fixture.require_clean();
        REQUIRE(fixture.first.header().reason == ReasonCode::Allocation);
    }
    for (auto point : {FaultPoint::HeapObject, FaultPoint::HeapRegistry}) {
        IterationFixture fixture;
        fixture.io.point = point;
        fixture.compile("fn run(){let a=[1];for(let x in a){let s=\"北邮\"+\"😀e\";"
                        "let armed=false;for(let c in s){s=null;if(!armed){"
                        "arm_iteration();armed=true;}}}}run();");
        // 登记表故障须让未回收对象填满原容量，才能抵达真实的下一次登记表增长。
        TestHooks::force_gc(point == FaultPoint::HeapObject);
        bool caught = false;
        std::optional<SourceSpan> failure_span;
        try { fixture.vm.execute(fixture.program); }
        catch (const Failure &failure) {
            caught = failure.header().reason == ReasonCode::Allocation;
            failure_span = failure.header().span;
        }
        TestHooks::force_gc(false);
        TestHooks::clear_fault();
        REQUIRE(caught && fixture.io.arms == 1);
        REQUIRE(fixture.first.header().reason == ReasonCode::Allocation);
        REQUIRE(failure_span.has_value());
        REQUIRE(fixture.trace.size() == 1 && fixture.trace[0].name == "run");
        fixture.require_clean(2);
        REQUIRE(fixture.first.header().reason == ReasonCode::Allocation);
    }
    for (auto diagnostic_failure : {false, true}) {
        IterationFixture fixture;
        fixture.io.point = FaultPoint::DiagnosticSnapshot;
        fixture.compile(std::string("fn broken(a){for(let x in a){for(let y in a){") +
                        (diagnostic_failure ? "arm_iteration();" : "") +
                        "let z=1/0;}}}broken([1]);");
        bool caught = false;
        try { fixture.vm.execute(fixture.program); }
        catch (const MybRuntimeFailure &failure) {
            caught = failure.header().reason == ReasonCode::Arithmetic;
        }
        TestHooks::clear_fault();
        REQUIRE(caught && fixture.first.header().reason == ReasonCode::Arithmetic);
        REQUIRE(fixture.first.header().domain == FailureDomain::Runtime);
        REQUIRE(fixture.first.header().span.has_value());
        REQUIRE(fixture.trace.size() == (diagnostic_failure ? 0u : 1u));
        fixture.require_clean();
        REQUIRE(fixture.first.header().reason == ReasonCode::Arithmetic);
    }
}
