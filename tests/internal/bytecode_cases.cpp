#include "bytecode/compiler.hpp"
#include "bytecode/verify.hpp"
#include "frontend/driver.hpp"
#include "runtime/object.hpp"
#include "semantic/checker.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
using namespace myb;

namespace {
/** @brief 构造具有各类副表的单元，供测试分别核对指令和协议约束。 */
CodeUnit unit_with(std::vector<Instruction> instructions) {
    CodeUnit unit;
    unit.constants = {Value{true}};
    unit.local_count = 3;
    unit.calls = {{0, 0}, {0, 1}, {0, 2}};
    unit.constructions = {{0, {}}, {0, {0}}, {0, {0, 1}}};
    unit.iterations = {{0}, {1}};
    unit.instructions = std::move(instructions);
    unit.locations.resize(unit.instructions.size());
    return unit;
}
/** @brief 要求损坏产物报告 Internal/Invariant，并保留宿主故障退出码。 */
void rejects(CodeUnit unit) {
    EmergencyFailure first;
    bool rejected = false;
    try {
        verify(unit, first);
    } catch (const HostFailure &ex) {
        rejected = ex.header().domain == FailureDomain::Internal &&
                   ex.header().reason == ReasonCode::Invariant && exit_code(ex.header()) == 3;
    }
    REQUIRE(rejected);
}
/** @brief 同时检查入口可达和不可达的损坏指令。 */
void rejects_both(std::vector<Instruction> instructions) {
    rejects(unit_with(instructions));
    // 在前方添加终止指令后，跳转编号仍须引用原指令。
    for (auto &ins : instructions) {
        if (ins.op == Op::JUMP || ins.op == Op::JUMP_FALSE || ins.op == Op::JUMP_TRUE)
            ++ins.a;
        if (ins.op == Op::ITER_NEXT)
            ++ins.b;
    }
    instructions.insert(instructions.begin(), {Op::HALT, 0, 0});
    rejects(unit_with(std::move(instructions)));
}
/** @brief 核对预期最大操作数深度，并确认同一片段不可达时不增加容量。 */
void accepts_both(CodeUnit unit, std::uint32_t maximum) {
    EmergencyFailure first;
    verify(unit, first);
    REQUIRE(unit.max_operand == maximum);
    for (auto &ins : unit.instructions) {
        if (ins.op == Op::JUMP || ins.op == Op::JUMP_FALSE || ins.op == Op::JUMP_TRUE)
            ++ins.a;
        if (ins.op == Op::ITER_NEXT)
            ++ins.b;
    }
    unit.instructions.insert(unit.instructions.begin(), {Op::HALT, 0, 0});
    unit.locations.resize(unit.instructions.size());
    verify(unit, first);
    REQUIRE(unit.max_operand == 0);
}
} // namespace

MYB_TEST(bytecode_layout) {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap(roots, first);
    SourceManager sources(first);
    const auto source =
        sources.add("layout.myb",
                    "let x=3; fn f(p){let before=x;let x=x+1;if(false){let y=unknown;}return p;}");
    auto ast = Driver(sources, source).parse();
    auto checked = Checker(first).check(*ast);
    CompiledProgram program(roots);
    BytecodeCompiler(heap, roots, first).compile(checked, program);
    REQUIRE(program.units.size() == 2);
    const auto &fn = program.units[1];
    REQUIRE(fn.local_count == 4);
    REQUIRE(fn.instructions[0].op == Op::DECL_LOCAL);
    REQUIRE(fn.instructions[0].a == 1);
    REQUIRE(fn.instructions[1].op == Op::LOAD_GLOBAL);
    REQUIRE(program.names[fn.instructions[1].a] == "x");
    bool self = false, unknown = false;
    for (std::size_t i = 0; i + 1 < fn.instructions.size(); ++i) {
        if (fn.instructions[i].op == Op::DECL_LOCAL && fn.instructions[i].a == 2)
            self = fn.instructions[i + 1].op == Op::LOAD_LOCAL && fn.instructions[i + 1].a == 2;
        if (fn.instructions[i].op == Op::LOAD_GLOBAL)
            unknown |= program.names[fn.instructions[i].a] == "unknown";
    }
    REQUIRE(self);
    REQUIRE(unknown);

    const auto nested_source =
        sources.add("nested-layout.myb", "fn nested(a,b){let outside=a;while(b){let loop=a;"
                                         "if(a){let inside=b;continue;}else{let other=b;break;}}"
                                         "{let tail=b;}return a;}");
    auto nested_ast = Driver(sources, nested_source).parse();
    auto nested_checked = Checker(first).check(*nested_ast);
    auto layout = BindingLayout::build(nested_checked);
    const auto &decl = dynamic_cast<const FunctionDecl &>(nested_ast->items()[0]);
    const auto &body = decl.body();
    const auto &loop = dynamic_cast<const WhileStmt &>(body.statements()[1]);
    const auto &branch = dynamic_cast<const IfStmt &>(loop.body().statements()[1]);
    const auto &tail = dynamic_cast<const BlockStmt &>(body.statements()[2]);
    const auto &slots = layout.units[1];
    REQUIRE(slots.parameter_count == 2);
    REQUIRE(slots.local_count == 7);
    REQUIRE(slots.slots.at(&body.statements()[0]) == 2);
    REQUIRE(slots.slots.at(&loop.body().statements()[0]) == 3);
    REQUIRE(slots.slots.at(&branch.then_body().statements()[0]) == 4);
    REQUIRE(slots.slots.at(&dynamic_cast<const BlockStmt &>(*branch.else_body()).statements()[0]) ==
            5);
    REQUIRE(slots.slots.at(&tail.statements()[0]) == 6);
    REQUIRE(slots.ranges.at(&body).first == 2 && slots.ranges.at(&body).count == 5);
    REQUIRE(slots.ranges.at(&loop).first == 3 && slots.ranges.at(&loop).count == 3);
    REQUIRE(slots.ranges.at(&branch).first == 4 && slots.ranges.at(&branch).count == 2);
    CompiledProgram nested(roots);
    BytecodeCompiler(heap, roots, first).compile(nested_checked, nested);
    const auto &code = nested.units[1];
    REQUIRE(code.parameter_count == 2 && code.local_count == 7);
    bool parameter0 = false, parameter1 = false;
    std::size_t loop_clears = 0;
    for (std::size_t i = 0; i < code.instructions.size(); ++i) {
        const auto ins = code.instructions[i];
        if (ins.op == Op::LOAD_LOCAL) {
            parameter0 |= ins.a == 0;
            parameter1 |= ins.a == 1;
        }
        if (ins.op == Op::CLEAR_LOCALS && ins.a == 3 && ins.b == 3) {
            ++loop_clears;
            REQUIRE(code.instructions.at(i + 1).op == Op::JUMP);
        }
        REQUIRE(code.locations[i].source == nested_source);
    }
    REQUIRE(parameter0 && parameter1);
    REQUIRE(loop_clears == 3); // continue、break 和正常循环末尾均清除完整循环区间。
    REQUIRE(nested.units[0].instructions.back().op == Op::HALT);
    REQUIRE(code.instructions[code.instructions.size() - 2].op == Op::CONST);
    REQUIRE(std::holds_alternative<NullValue>(code.constants.back().data));
    REQUIRE(code.instructions.back().op == Op::RETURN);

    // 完整语法和动态错误均生成产物，编译过程不读取运行值。
    const auto syntax_source =
        sources.add("complete-syntax.myb",
                    "struct Pair{left:Int64;right:String;}let x=x;missing(1,2);"
                    "Pair{left:unknown,left:wrong()};Pair{};"
                    "let a=[1,2];a[0]=3;a[0];a[:];a[1:];a[:2];a[0:2];"
                    "let p=Pair{left:1,right:\"s\"};p.left=2;p.left;"
                    "fn f(a,b){return a;}f();"
                    "fn dead(){return;for(let item in [1]){let local=item;"
                    "if(true&&false||true){continue;}else{break;}}return 2;}"
                    "for(let item in a){while(false){break;}if(item==1){continue;}break;}");
    auto syntax_ast = Driver(sources, syntax_source).parse();
    auto syntax_checked = Checker(first).check(*syntax_ast);
    CompiledProgram syntax(roots);
    BytecodeCompiler(heap, roots, first).compile(syntax_checked, syntax);
    REQUIRE(syntax.units.size() == 3);
    const auto &plans = syntax.units[0].constructions;
    REQUIRE(plans[0].fields.size() == 2);
    REQUIRE(plans[0].fields[0] == plans[0].fields[1]);
    REQUIRE(syntax.names[plans[0].fields[0]] == "left");
    REQUIRE(plans[1].fields.empty());
    REQUIRE(syntax.units[0].calls[0].arity == 2);
}

MYB_TEST(bytecode_verify) {
    EmergencyFailure first;
    CodeUnit unit;
    unit.constants = {Value{true}};
    unit.instructions = {{Op::CONST, 0, 0},
                         {Op::JUMP_FALSE, 4, 0},
                         {Op::CONST, 0, 0},
                         {Op::DROP, 0, 0},
                         {Op::HALT, 0, 0}};
    unit.locations.resize(unit.instructions.size());
    verify(unit, first);
    REQUIRE(unit.max_operand == 1);
    rejects_both({{Op::JUMP, 99, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::DROP, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::CONST, 0, 0}, {Op::JUMP_FALSE, 3, 0}, {Op::CONST, 0, 0}, {Op::HALT, 0, 0}});

    // 每个 Op 至少有一个合法路径；GET_SLICE 的四个端点组合分别核对。
    auto all =
        unit_with({{Op::DECL_LOCAL, 0, 0},  {Op::DECL_GLOBAL, 0, 0},  {Op::CONST, 0, 0},
                   {Op::DUP, 0, 0},         {Op::STORE_LOCAL, 0, 0},  {Op::STORE_GLOBAL, 0, 0},
                   {Op::LOAD_LOCAL, 0, 0},  {Op::LOAD_GLOBAL, 0, 0},  {Op::BINARY, 0, 0},
                   {Op::UNARY, 0, 0},       {Op::CONST, 0, 0},        {Op::COMPARE, 0, 0},
                   {Op::CHECK_BOOL, 0, 0},  {Op::DROP, 0, 0},         {Op::CONST, 0, 0},
                   {Op::JUMP_FALSE, 16, 0}, {Op::CONST, 0, 0},        {Op::JUMP_TRUE, 18, 0},
                   {Op::JUMP, 19, 0},       {Op::PRECALL, 0, 0},      {Op::CALL, 0, 0},
                   {Op::DROP, 0, 0},        {Op::PRECALL, 1, 0},      {Op::CONST, 0, 0},
                   {Op::CALL, 1, 0},        {Op::DROP, 0, 0},         {Op::PRECALL, 2, 0},
                   {Op::CONST, 0, 0},       {Op::CONST, 0, 0},        {Op::CALL, 2, 0},
                   {Op::DROP, 0, 0},        {Op::CONST, 0, 0},        {Op::CONST, 0, 0},
                   {Op::BUILD_ARRAY, 2, 0}, {Op::DROP, 0, 0},         {Op::CHECK_STRUCT, 2, 0},
                   {Op::CONST, 0, 0},       {Op::CHECK_FIELD, 2, 0},  {Op::CONST, 0, 0},
                   {Op::CHECK_FIELD, 2, 1}, {Op::BUILD_STRUCT, 2, 0}, {Op::DROP, 0, 0},
                   {Op::CONST, 0, 0},       {Op::GET_FIELD, 0, 0},    {Op::DROP, 0, 0},
                   {Op::CONST, 0, 0},       {Op::CONST, 0, 0},        {Op::GET_INDEX, 0, 0},
                   {Op::DROP, 0, 0},        {Op::CONST, 0, 0},        {Op::GET_SLICE, 0, 0},
                   {Op::DROP, 0, 0},        {Op::CONST, 0, 0},        {Op::CONST, 0, 0},
                   {Op::GET_SLICE, 1, 0},   {Op::DROP, 0, 0},         {Op::CONST, 0, 0},
                   {Op::CONST, 0, 0},       {Op::GET_SLICE, 2, 0},    {Op::DROP, 0, 0},
                   {Op::CONST, 0, 0},       {Op::CONST, 0, 0},        {Op::CONST, 0, 0},
                   {Op::GET_SLICE, 3, 0},   {Op::DROP, 0, 0},         {Op::CONST, 0, 0},
                   {Op::CONST, 0, 0},       {Op::SET_FIELD, 0, 0},    {Op::CONST, 0, 0},
                   {Op::CONST, 0, 0},       {Op::CONST, 0, 0},        {Op::SET_INDEX, 0, 0},
                   {Op::CONST, 0, 0},       {Op::ITER_BEGIN, 0, 0},   {Op::ITER_NEXT, 0, 75},
                   {Op::ITER_END, 0, 0},    {Op::CLEAR_LOCALS, 0, 3}, {Op::HALT, 0, 0}});
    std::array<bool, static_cast<std::size_t>(Op::ITER_END) + 1> covered{};
    for (const auto ins : all.instructions)
        covered[static_cast<std::size_t>(ins.op)] = true;
    covered[static_cast<std::size_t>(Op::RETURN)] = true;
    REQUIRE(std::all_of(covered.begin(), covered.end(), [](bool value) { return value; }));
    accepts_both(all, 3);
    accepts_both(unit_with({{Op::CONST, 0, 0}, {Op::RETURN, 0, 0}}), 1);
    accepts_both(unit_with({{Op::PRECALL, 2, 0},
                            {Op::CONST, 0, 0},
                            {Op::PRECALL, 1, 0},
                            {Op::CONST, 0, 0},
                            {Op::CALL, 1, 0},
                            {Op::CALL, 2, 0},
                            {Op::DROP, 0, 0},
                            {Op::HALT, 0, 0}}),
                 2);
    accepts_both(unit_with({{Op::CHECK_STRUCT, 1, 0},
                            {Op::CHECK_STRUCT, 0, 0},
                            {Op::BUILD_STRUCT, 0, 0},
                            {Op::CHECK_FIELD, 1, 0},
                            {Op::BUILD_STRUCT, 1, 0},
                            {Op::DROP, 0, 0},
                            {Op::HALT, 0, 0}}),
                 1);
    accepts_both(unit_with({{Op::CONST, 0, 0},
                            {Op::ITER_BEGIN, 0, 0},
                            {Op::CONST, 0, 0},
                            {Op::ITER_BEGIN, 1, 0},
                            {Op::ITER_END, 1, 0},
                            {Op::ITER_END, 0, 0},
                            {Op::HALT, 0, 0}}),
                 1);
    // RETURN 允许活动遍历交由帧统一清理。
    accepts_both(
        unit_with(
            {{Op::CONST, 0, 0}, {Op::ITER_BEGIN, 0, 0}, {Op::CONST, 0, 0}, {Op::RETURN, 0, 0}}),
        1);

    for (std::size_t i = 0; i < all.instructions.size(); ++i) {
        const auto op = all.instructions[i].op;
        if (op != Op::CLEAR_LOCALS && op != Op::CHECK_FIELD && op != Op::ITER_NEXT) {
            auto bad = all;
            bad.instructions[i].b = 1;
            rejects_both(std::move(bad.instructions));
        }
    }
    for (const auto op :
         {Op::DUP, Op::DROP, Op::CHECK_BOOL, Op::RETURN, Op::HALT, Op::GET_INDEX, Op::SET_INDEX})
        rejects_both({{op, 1, 0}, {Op::HALT, 0, 0}});
    for (const auto ins : std::vector<Instruction>{{static_cast<Op>(65535), 0, 0},
                                                   {Op::CONST, 1, 0},
                                                   {Op::DECL_LOCAL, 3, 0},
                                                   {Op::LOAD_LOCAL, 3, 0},
                                                   {Op::STORE_LOCAL, 3, 0},
                                                   {Op::CLEAR_LOCALS, 2, 2},
                                                   {Op::CLEAR_LOCALS, 4, 0},
                                                   {Op::UNARY, 3, 0},
                                                   {Op::BINARY, 6, 0},
                                                   {Op::COMPARE, 6, 0},
                                                   {Op::GET_SLICE, 4, 0},
                                                   {Op::PRECALL, 3, 0},
                                                   {Op::CALL, 3, 0},
                                                   {Op::CHECK_STRUCT, 3, 0},
                                                   {Op::BUILD_STRUCT, 3, 0},
                                                   {Op::CHECK_FIELD, 1, 1},
                                                   {Op::ITER_BEGIN, 2, 0},
                                                   {Op::ITER_NEXT, 0, 99},
                                                   {Op::ITER_END, 2, 0}})
        rejects_both({ins, {Op::HALT, 0, 0}});

    // 普通栈效应、终止深度、调用配对、字段顺序与遍历生命周期。
    for (const auto op : {Op::DUP, Op::STORE_LOCAL, Op::STORE_GLOBAL, Op::UNARY, Op::BINARY,
                          Op::COMPARE, Op::CHECK_BOOL, Op::GET_FIELD, Op::GET_INDEX, Op::GET_SLICE,
                          Op::SET_FIELD, Op::SET_INDEX, Op::ITER_BEGIN, Op::RETURN})
        rejects_both({{op, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::BUILD_ARRAY, 1, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::JUMP_FALSE, 1, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::JUMP_TRUE, 1, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::CONST, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::CONST, 0, 0}, {Op::CONST, 0, 0}, {Op::RETURN, 0, 0}});
    rejects_both({{Op::CALL, 0, 0}, {Op::DROP, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::PRECALL, 0, 0}, {Op::CALL, 1, 0}, {Op::DROP, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::PRECALL, 1, 0}, {Op::CALL, 1, 0}, {Op::DROP, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::PRECALL, 1, 0},
                  {Op::CONST, 0, 0},
                  {Op::CONST, 0, 0},
                  {Op::CALL, 1, 0},
                  {Op::DROP, 0, 0},
                  {Op::HALT, 0, 0}});
    rejects_both({{Op::PRECALL, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::PRECALL, 0, 0}, {Op::CONST, 0, 0}, {Op::RETURN, 0, 0}});
    rejects_both({{Op::CONST, 0, 0},
                  {Op::JUMP_FALSE, 4, 0},
                  {Op::PRECALL, 0, 0},
                  {Op::JUMP, 4, 0},
                  {Op::HALT, 0, 0}});
    rejects_both({{Op::BUILD_STRUCT, 0, 0}, {Op::DROP, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::CONST, 0, 0}, {Op::CHECK_FIELD, 1, 0}, {Op::DROP, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::CHECK_STRUCT, 2, 0},
                  {Op::CONST, 0, 0},
                  {Op::CHECK_FIELD, 2, 1},
                  {Op::BUILD_STRUCT, 2, 0},
                  {Op::DROP, 0, 0},
                  {Op::HALT, 0, 0}});
    rejects_both({{Op::CHECK_STRUCT, 1, 0},
                  {Op::CONST, 0, 0},
                  {Op::CHECK_FIELD, 1, 0},
                  {Op::CHECK_FIELD, 1, 0},
                  {Op::BUILD_STRUCT, 1, 0},
                  {Op::DROP, 0, 0},
                  {Op::HALT, 0, 0}});
    rejects_both({{Op::CHECK_STRUCT, 1, 0},
                  {Op::CONST, 0, 0},
                  {Op::BUILD_STRUCT, 1, 0},
                  {Op::DROP, 0, 0},
                  {Op::HALT, 0, 0}});
    rejects_both({{Op::CHECK_STRUCT, 1, 0},
                  {Op::CONST, 0, 0},
                  {Op::CHECK_FIELD, 2, 0},
                  {Op::BUILD_STRUCT, 1, 0},
                  {Op::DROP, 0, 0},
                  {Op::HALT, 0, 0}});
    rejects_both({{Op::CHECK_STRUCT, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::CHECK_STRUCT, 0, 0}, {Op::CONST, 0, 0}, {Op::RETURN, 0, 0}});
    rejects_both({{Op::ITER_NEXT, 0, 1}, {Op::HALT, 0, 0}});
    rejects_both({{Op::ITER_END, 0, 0}, {Op::HALT, 0, 0}});
    rejects_both({{Op::CONST, 0, 0},
                  {Op::ITER_BEGIN, 0, 0},
                  {Op::CONST, 0, 0},
                  {Op::ITER_BEGIN, 0, 0},
                  {Op::ITER_END, 0, 0},
                  {Op::HALT, 0, 0}});
    rejects_both({{Op::CONST, 0, 0},
                  {Op::ITER_BEGIN, 0, 0},
                  {Op::CONST, 0, 0},
                  {Op::ITER_BEGIN, 1, 0},
                  {Op::ITER_END, 0, 0},
                  {Op::ITER_END, 1, 0},
                  {Op::HALT, 0, 0}});
    rejects_both({{Op::CONST, 0, 0},
                  {Op::ITER_BEGIN, 0, 0},
                  {Op::ITER_END, 0, 0},
                  {Op::ITER_END, 0, 0},
                  {Op::HALT, 0, 0}});
    rejects_both({{Op::CONST, 0, 0}, {Op::ITER_BEGIN, 0, 0}, {Op::HALT, 0, 0}});

    auto bad_table = unit_with({{Op::HALT, 0, 0}});
    bad_table.locations.clear();
    rejects(bad_table);
    bad_table = unit_with({{Op::HALT, 0, 0}});
    bad_table.parameter_count = 4;
    rejects(bad_table);
    bad_table = unit_with({{Op::HALT, 0, 0}});
    bad_table.iterations[0].slot = 3;
    rejects(bad_table);
    bad_table = unit_with({{Op::HALT, 0, 0}});
    bad_table.statement_boundaries = {1};
    bad_table.boundary_iterations = {{}};
    rejects(bad_table);
    bad_table.statement_boundaries = {0};
    bad_table.boundary_iterations = {{2}};
    rejects(bad_table);
    bad_table.statement_boundaries = {0};
    bad_table.boundary_iterations.clear();
    rejects(bad_table);

    // 程序级核验补充检查名称所属表，以及顶层和函数各自的终止指令。
    RootRegistry roots;
    const auto rejects_program = [&](auto damage) {
        EmergencyFailure failure;
        CompiledProgram program(roots);
        program.names = {"f", "T", "field"};
        program.units = {unit_with({{Op::HALT, 0, 0}}),
                         unit_with({{Op::CONST, 0, 0}, {Op::RETURN, 0, 0}})};
        program.functions = {{0, 0, 1, {}}};
        damage(program);
        bool rejected = false;
        try {
            verify(program, failure);
        } catch (const HostFailure &ex) {
            rejected = ex.header().domain == FailureDomain::Internal && exit_code(ex.header()) == 3;
        }
        REQUIRE(rejected);
    };
    for (const auto op :
         {Op::DECL_GLOBAL, Op::LOAD_GLOBAL, Op::STORE_GLOBAL, Op::GET_FIELD, Op::SET_FIELD})
        rejects_program([&](CompiledProgram &p) { p.units[1].instructions[0] = {op, 3, 0}; });
    rejects_program([](CompiledProgram &p) { p.units[1].calls[0].target = 3; });
    rejects_program([](CompiledProgram &p) { p.units[1].constructions[0].type = 3; });
    rejects_program([](CompiledProgram &p) { p.units[1].constructions[1].fields[0] = 3; });
    rejects_program([](CompiledProgram &p) { p.functions[0].name = 3; });
    rejects_program([](CompiledProgram &p) { p.functions[0].unit = 2; });
    rejects_program([](CompiledProgram &p) { p.functions[0].unit = 0; });
    rejects_program([](CompiledProgram &p) { p.functions[0].arity = 1; });
    rejects_program(
        [](CompiledProgram &p) { p.units.emplace_back(unit_with({{Op::HALT, 0, 0}})); });
    rejects_program([](CompiledProgram &p) { p.top_unit = 2; });
    rejects_program([](CompiledProgram &p) {
        p.units[0] = unit_with({{Op::CONST, 0, 0}, {Op::RETURN, 0, 0}});
    });
    rejects_program([](CompiledProgram &p) { p.units[1] = unit_with({{Op::HALT, 0, 0}}); });
}

MYB_TEST(bytecode_constant_roots) {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap(roots, first);
    SourceManager sources(first);
    CompiledProgram program(roots);
    static_assert(std::is_nothrow_constructible_v<CompiledProgram, RootRegistry &>);
    TestHooks::force_gc(true);
    try {
        std::string text = "fn f(){return \"kept\";} \"top\";";
        // 迫使各单元常量表与代码单元表增长，根提供者须重新枚举容器内容。
        for (int i = 0; i < 24; ++i)
            text += "fn unused" + std::to_string(i) + "(){\"first" + std::to_string(i) +
                    "\";\"second" + std::to_string(i) + "\";return \"last" + std::to_string(i) +
                    "\";}";
        const auto source = sources.add("roots.myb", text);
        auto ast = Driver(sources, source).parse();
        auto checked = Checker(first).check(*ast);
        BytecodeCompiler(heap, roots, first).compile(checked, program);
        ast.reset();
        heap.collect();
        REQUIRE(program.units.size() == 26);
        REQUIRE(std::get<StringRef>(program.units[0].constants[0].data).ptr->utf8 == "top");
        REQUIRE(std::get<StringRef>(program.units[1].constants[0].data).ptr->utf8 == "kept");
        for (std::size_t i = 2; i < program.units.size(); ++i) {
            const auto suffix = std::to_string(i - 2);
            const auto &constants = program.units[i].constants;
            REQUIRE(std::get<StringRef>(constants[0].data).ptr->utf8 == "first" + suffix);
            REQUIRE(std::get<StringRef>(constants[1].data).ptr->utf8 == "second" + suffix);
            REQUIRE(std::get<StringRef>(constants[2].data).ptr->utf8 == "last" + suffix);
        }
        REQUIRE(TestHooks::heap_counts(heap).live == 74);
        program.detach_roots();
        program.detach_roots();
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 0);

        for (const std::string_view source_text :
             {"\"unpublished\";", "fn f(){return \"unpublished\";}", "1;"}) {
            const auto failed_source =
                sources.add("constant-failure.myb", std::string(source_text));
            auto failed_ast = Driver(sources, failed_source).parse();
            auto failed_checked = Checker(first).check(*failed_ast);
            {
                // 根登记本身不分配。登记期间设置的故障须延后至实际常量追加。
                TestHooks::fail_next(FaultPoint::ProgramConstants);
                CompiledProgram failed_program(roots);
                bool failed = false;
                try {
                    BytecodeCompiler(heap, roots, first).compile(failed_checked, failed_program);
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
                REQUIRE(failed);
                failed_ast.reset();
                heap.collect();
                REQUIRE(TestHooks::heap_counts(heap).live == 0);
            }
            heap.collect();
            REQUIRE(TestHooks::heap_counts(heap).live == 0);
        }
        // 一次失败的编译产物销毁后，同一根表仍可承接新的程序并回收。
        {
            CompiledProgram recovery(roots);
            const auto recovered_source = sources.add("recovered.myb", "\"recovered\";");
            auto recovered_ast = Driver(sources, recovered_source).parse();
            auto recovered_checked = Checker(first).check(*recovered_ast);
            BytecodeCompiler(heap, roots, first).compile(recovered_checked, recovery);
            recovered_ast.reset();
            heap.collect();
            REQUIRE(std::get<StringRef>(recovery.units[0].constants[0].data).ptr->utf8 ==
                    "recovered");
            REQUIRE(TestHooks::heap_counts(heap).live == 1);
        }
        heap.collect();
        REQUIRE(TestHooks::heap_counts(heap).live == 0);
    } catch (...) {
        TestHooks::clear_fault();
        TestHooks::force_gc(false);
        throw;
    }
    TestHooks::force_gc(false);
}
