#include "bytecode/verify.hpp"
#include <algorithm>
#include <deque>
#include <limits>
#include <optional>
#include <tuple>
namespace myb {
namespace {
/**
 * @brief 保存控制流点的栈深和未完成调用、构造、遍历协议。
 *
 * calls 各项为（调用点号，PRECALL 时的操作数深度），按未完成调用的嵌套次序保存。
 * constructions 各项为（构造方案号，下个待检查字段序号，CHECK_STRUCT 时的操作数深度）。
 * iterations 从外到内保存活动遍历站点号，末项为当前最内层遍历。
 */
struct State {
    std::uint64_t depth{};
    std::vector<std::pair<std::uint32_t, std::uint64_t>> calls;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint64_t>> constructions;
    std::vector<std::uint32_t> iterations;
    bool operator==(const State &) const = default;
};
} // namespace
void verify(CodeUnit &unit, EmergencyFailure &first) {
    const auto bad = [&] {
        fail({first, {}}, FailureDomain::Internal, ReasonCode::Invariant, "invalid bytecode");
    };
    const auto count = unit.instructions.size();
    const auto limit = std::numeric_limits<std::uint32_t>::max();
    if (!count || count > limit || count != unit.locations.size() ||
        unit.parameter_count > unit.local_count || unit.constants.size() > limit ||
        unit.calls.size() > limit || unit.constructions.size() > limit ||
        unit.iterations.size() > limit ||
        unit.statement_boundaries.size() != unit.boundary_iterations.size())
        bad();
    // 先检查所有指令的形状及索引，死代码同样不能隐藏生成缺陷。
    for (const auto ins : unit.instructions) {
        const bool uses_b =
            ins.op == Op::CLEAR_LOCALS || ins.op == Op::CHECK_FIELD || ins.op == Op::ITER_NEXT;
        if (!uses_b && ins.b)
            bad();
        switch (ins.op) {
        case Op::CONST:
            if (ins.a >= unit.constants.size())
                bad();
            break;
        case Op::DECL_LOCAL:
        case Op::LOAD_LOCAL:
        case Op::STORE_LOCAL:
            if (ins.a >= unit.local_count)
                bad();
            break;
        case Op::CLEAR_LOCALS:
            if (ins.a > unit.local_count || ins.b > unit.local_count - ins.a)
                bad();
            break;
        case Op::UNARY:
            if (ins.a > static_cast<std::uint32_t>(UnaryOp::Not))
                bad();
            break;
        case Op::BINARY:
            if (ins.a > static_cast<std::uint32_t>(BinaryOp::Modulo))
                bad();
            break;
        case Op::COMPARE:
            if (ins.a > static_cast<std::uint32_t>(CompareOp::GreaterEqual))
                bad();
            break;
        case Op::JUMP:
        case Op::JUMP_FALSE:
        case Op::JUMP_TRUE:
            if (ins.a >= count)
                bad();
            break;
        case Op::PRECALL:
        case Op::CALL:
            if (ins.a >= unit.calls.size())
                bad();
            break;
        case Op::CHECK_STRUCT:
        case Op::BUILD_STRUCT:
            if (ins.a >= unit.constructions.size())
                bad();
            break;
        case Op::CHECK_FIELD:
            if (ins.a >= unit.constructions.size() ||
                ins.b >= unit.constructions[ins.a].fields.size())
                bad();
            break;
        case Op::GET_SLICE:
            if (ins.a > 3)
                bad();
            break;
        case Op::ITER_BEGIN:
        case Op::ITER_NEXT:
        case Op::ITER_END:
            if (ins.a >= unit.iterations.size())
                bad();
            if (ins.op == Op::ITER_NEXT && ins.b >= count)
                bad();
            break;
        case Op::DUP:
        case Op::DROP:
        case Op::CHECK_BOOL:
        case Op::RETURN:
        case Op::HALT:
        case Op::GET_INDEX:
        case Op::SET_INDEX:
            if (ins.a)
                bad();
            break;
        case Op::DECL_GLOBAL:
        case Op::LOAD_GLOBAL:
        case Op::STORE_GLOBAL:
        case Op::GET_FIELD:
        case Op::SET_FIELD:
        case Op::BUILD_ARRAY:
            break;
        default:
            bad();
        }
    }
    for (const auto site : unit.iterations)
        if (site.slot >= unit.local_count)
            bad();
    for (std::size_t i = 0; i < unit.statement_boundaries.size(); ++i) {
        if (unit.statement_boundaries[i] >= count)
            bad();
        for (auto site : unit.boundary_iterations[i])
            if (site >= unit.iterations.size())
                bad();
    }
    // 合流要求栈深及全部未完成协议一致；不一致表示生成错误。
    // 因此仅首次到达时入队，无需用最大深度反复合并状态。
    std::vector<std::optional<State>> states(count);
    std::deque<std::size_t> work;
    std::uint64_t maximum = 0;
    const auto seed = [&](std::size_t ip, State state) {
        if (ip >= count)
            bad();
        if (states[ip]) {
            if (*states[ip] != state)
                bad();
        } else {
            states[ip] = std::move(state);
            work.push_back(ip);
        }
    };
    const auto analyze = [&](bool reachable) {
        while (!work.empty()) {
            const auto ip = work.front();
            work.pop_front();
            auto state = *states[ip];
            const auto ins = unit.instructions[ip];
            std::uint64_t consume = 0, produce = 0;
            switch (ins.op) {
            case Op::CONST:
            case Op::LOAD_LOCAL:
            case Op::LOAD_GLOBAL:
                produce = 1;
                break;
            case Op::DUP:
                consume = 1;
                produce = 2;
                break;
            case Op::DROP:
            case Op::STORE_LOCAL:
            case Op::STORE_GLOBAL:
                consume = 1;
                break;
            case Op::UNARY:
            case Op::CHECK_BOOL:
            case Op::GET_FIELD:
                consume = produce = 1;
                break;
            case Op::BINARY:
            case Op::COMPARE:
            case Op::GET_INDEX:
                consume = 2;
                produce = 1;
                break;
            case Op::GET_SLICE:
                consume = 1 + ((ins.a & 1) != 0) + ((ins.a & 2) != 0);
                produce = 1;
                break;
            case Op::SET_FIELD:
                consume = 2;
                break;
            case Op::SET_INDEX:
                consume = 3;
                break;
            case Op::BUILD_ARRAY:
                consume = ins.a;
                produce = 1;
                break;
            case Op::JUMP_FALSE:
            case Op::JUMP_TRUE:
                consume = 1;
                break;
            case Op::PRECALL:
                state.calls.emplace_back(ins.a, state.depth);
                break;
            case Op::CALL:
                if (state.calls.empty() || state.calls.back().first != ins.a ||
                    state.depth != state.calls.back().second + unit.calls[ins.a].arity)
                    bad();
                state.calls.pop_back();
                consume = unit.calls[ins.a].arity;
                produce = 1;
                break;
            case Op::CHECK_STRUCT:
                state.constructions.emplace_back(ins.a, 0, state.depth);
                break;
            case Op::CHECK_FIELD:
                if (state.constructions.empty())
                    bad();
                {
                    auto &[plan, next, base] = state.constructions.back();
                    if (plan != ins.a || next != ins.b || state.depth != base + next + 1)
                        bad();
                    ++next;
                    consume = produce = 1;
                }
                break;
            case Op::BUILD_STRUCT:
                if (state.constructions.empty())
                    bad();
                {
                    const auto [plan, next, base] = state.constructions.back();
                    if (plan != ins.a || next != unit.constructions[ins.a].fields.size() ||
                        state.depth != base + next)
                        bad();
                    consume = next;
                    produce = 1;
                    state.constructions.pop_back();
                }
                break;
            case Op::ITER_BEGIN:
                if (std::find(state.iterations.begin(), state.iterations.end(), ins.a) !=
                    state.iterations.end())
                    bad();
                state.iterations.push_back(ins.a);
                consume = 1;
                break;
            case Op::ITER_NEXT:
            case Op::ITER_END:
                if (state.iterations.empty() || state.iterations.back() != ins.a)
                    bad();
                if (ins.op == Op::ITER_END)
                    state.iterations.pop_back();
                break;
            case Op::RETURN:
                if (state.depth != 1 || !state.calls.empty() || !state.constructions.empty())
                    bad();
                break;
            case Op::HALT:
                if (state.depth || !state.calls.empty() || !state.constructions.empty() ||
                    !state.iterations.empty())
                    bad();
                break;
            default:
                break;
            }
            if (state.depth < consume)
                bad();
            state.depth = state.depth - consume + produce;
            if (state.depth > limit)
                fail({first, {}}, FailureDomain::Resource, ReasonCode::Capacity,
                     "operand stack is too large");
            if (reachable)
                maximum = std::max(maximum, state.depth);
            switch (ins.op) {
            case Op::RETURN:
            case Op::HALT:
                break;
            case Op::JUMP:
                seed(ins.a, state);
                break;
            case Op::JUMP_FALSE:
            case Op::JUMP_TRUE:
                seed(ins.a, state);
                seed(ip + 1, state);
                break;
            case Op::ITER_NEXT:
                seed(ins.b, state);
                seed(ip + 1, state);
                break;
            default:
                seed(ip + 1, state);
                break;
            }
        }
    };
    seed(0, {});
    analyze(true);
    // return/break 后的不可达语句仍可能处于 for 的词法范围，用记录的遍历栈核验后续遍历指令。
    // 这些入口从零操作数深度开始，analyze(false) 只核验结构，不扩大 max_operand。
    for (std::size_t i = 0; i < unit.statement_boundaries.size(); ++i) {
        const auto ip = unit.statement_boundaries[i];
        if (!states[ip]) {
            State initial;
            initial.iterations = unit.boundary_iterations[i];
            seed(ip, std::move(initial));
            analyze(false);
        } else if (states[ip]->depth != 0 || !states[ip]->calls.empty() ||
                   !states[ip]->constructions.empty() ||
                   states[ip]->iterations != unit.boundary_iterations[i])
            bad();
    }
    // 手写或损坏产物中缺少入口记录的片段仍接受结构核验。
    for (std::size_t ip = 0; ip < count; ++ip)
        if (!states[ip]) {
            seed(ip, {});
            analyze(false);
        }
    unit.max_operand = static_cast<std::uint32_t>(maximum);
}
void verify(CompiledProgram &program, EmergencyFailure &first) {
    const auto bad = [&] {
        fail({first, {}}, FailureDomain::Internal, ReasonCode::Invariant, "invalid program table");
    };
    if (program.top_unit >= program.units.size())
        bad();
    const auto name = [&](NameId id) {
        if (id >= program.names.size())
            bad();
    };
    for (std::size_t i = 0; i < program.schemas.size(); ++i) {
        if (!program.schemas[i] || program.schemas[i]->id != i)
            bad();
        for (const auto &field : program.schemas[i]->fields)
            if (field.type.kind == TypeKind::Struct &&
                (!field.type.schema_id || *field.type.schema_id >= program.schemas.size()))
                bad();
    }
    std::vector<bool> owned(program.units.size());
    owned[program.top_unit] = true;
    for (const auto &fn : program.functions) {
        name(fn.name);
        if (fn.unit >= program.units.size() || owned[fn.unit] ||
            fn.arity != program.units[fn.unit].parameter_count)
            bad();
        owned[fn.unit] = true;
    }
    for (std::size_t i = 0; i < program.units.size(); ++i) {
        if (!owned[i])
            bad();
        auto &unit = program.units[i];
        for (const auto call : unit.calls)
            name(call.target);
        for (const auto &plan : unit.constructions) {
            name(plan.type);
            for (auto field : plan.fields)
                name(field);
        }
        for (const auto ins : unit.instructions) {
            switch (ins.op) {
            case Op::DECL_GLOBAL:
            case Op::LOAD_GLOBAL:
            case Op::STORE_GLOBAL:
            case Op::GET_FIELD:
            case Op::SET_FIELD:
                name(ins.a);
                break;
            case Op::RETURN:
                if (i == program.top_unit)
                    bad();
                break;
            case Op::HALT:
                if (i != program.top_unit)
                    bad();
                break;
            default:
                break;
            }
        }
        verify(unit, first);
    }
}
} // namespace myb
