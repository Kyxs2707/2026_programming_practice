#include "vm/vm.hpp"
#include "runtime/value_ops.hpp"
#include <algorithm>
#include <limits>
#ifdef MYB_TEST_HOOKS
#include "test_hooks.hpp"
#endif
namespace myb {
VM::VM(Heap &heap, RootRegistry &roots, HostIO &io, EmergencyFailure &first,
       const BuiltinRegistry &builtins, std::vector<CallTraceEntry> *trace) noexcept
    : heap_(heap), roots_(roots), io_(io), first_(first), builtins_(builtins), globals_(roots),
      root_set_(roots, this, trace_values), trace_(trace) {}
void VM::trace_values(void *self, Marker &marker) {
    auto &vm = *static_cast<VM *>(self);
    for (const auto &frame : vm.frames_) {
        for (auto i = frame.base; i < frame.operand_base; ++i)
            if (vm.store_.state(i) == BindingState::Initialized)
                marker.mark(vm.store_.value(i));
        for (std::size_t i = 0; i < frame.sp; ++i)
            marker.mark(vm.store_.value(frame.operand_base + i));
    }
}
[[noreturn]] void VM::invalid() {
    fail({first_, {}}, FailureDomain::Internal, ReasonCode::Invariant, "invalid VM state");
}
void VM::push(Value value) {
    auto &frame = frames_.back();
    if (frame.sp >= frame.region_end - frame.operand_base)
        invalid();
    store_.value(frame.operand_base + frame.sp) = value;
    ++frame.sp;
}
const CallSite &VM::call_site(const CodeUnit &unit, std::uint32_t index) {
    if (index >= unit.calls.size())
        invalid();
    return unit.calls[index];
}
void VM::precheck(const CallSite &call, ErrorSite site) const {
    if (call.target >= program_->names.size())
        fail(site, FailureDomain::Internal, ReasonCode::Invariant, "invalid call target index");
    for (const auto &function : program_->functions) {
        if (function.name != call.target)
            continue;
        if (function.arity != call.arity)
            fail(site, FailureDomain::Runtime, ReasonCode::Type, "wrong number of arguments");
        return;
    }
    (void)builtins_.precheck(program_->names[call.target], call.arity, site);
}
void VM::ensure_frames(std::size_t required, ErrorSite site) {
    const auto capacity = frames_.capacity(), limit = frames_.max_size();
    if (required <= capacity)
        return;
    if (required > limit)
        fail(site, FailureDomain::Resource, ReasonCode::Capacity, "VM frame capacity exceeded");
    const auto grown = capacity > limit - capacity / 2 ? limit : capacity + capacity / 2;
    try {
#ifdef MYB_TEST_HOOKS
        TestHooks::hit(FaultPoint::VmFrames);
#endif
        frames_.reserve(std::min(limit, std::max({required, grown, std::size_t{16}})));
    } catch (const std::bad_alloc &) {
        first_.record_first({FailureDomain::Resource, ReasonCode::Allocation, site.span});
        throw;
    }
}
void VM::prepare_call(std::uint32_t id, ErrorSite site) {
    const auto parent_index = frames_.size() - 1;
    const auto parent = frames_[parent_index];
    const auto &call = call_site(program_->units[parent.unit_id], id);
    precheck(call, site);
    if (parent.sp < call.arity)
        invalid();
    for (std::uint32_t function_id = 0; function_id < program_->functions.size(); ++function_id) {
        const auto &function = program_->functions[function_id];
        if (function.name != call.target)
            continue;
        if (function.unit >= program_->units.size())
            invalid();
        const auto &unit = program_->units[function.unit];
        if (unit.local_count < call.arity || unit.parameter_count != call.arity)
            invalid();
        Frame child;
        child.unit_id = function.unit;
        child.function_id = function_id;
        child.base = parent.region_end;
        child.operand_base = checked_region_end(child.base, unit.local_count,
                                                std::numeric_limits<std::size_t>::max(), first_);
        child.region_end = checked_region_end(child.operand_base, unit.max_operand,
                                              std::numeric_limits<std::size_t>::max(), first_);
        child.caller_arg_base = parent.sp - call.arity;
        child.caller_call_ip = parent.executing_ip;
        // 准备期间只保存索引，父sp与实参根保持不变。
        store_.ensure(child.region_end, site);
        ensure_frames(frames_.size() + 1, site);
        store_.clear(child.base, child.region_end);
        for (std::size_t i = 0; i < call.arity; ++i) {
            store_.value(child.base + i) =
                store_.value(parent.operand_base + child.caller_arg_base + i);
            store_.state(child.base + i) = BindingState::Initialized;
        }
        frames_.push_back(child); // 容量准备成功后无分配发布，子参数根先接管。
        frames_[parent_index].sp = child.caller_arg_base;
        store_.clear(parent.operand_base + child.caller_arg_base, parent.operand_base + parent.sp);
        return;
    }
    const auto &target = builtins_.precheck(program_->names[call.target], call.arity, site);
    if (!target.invoke)
        fail(site, FailureDomain::Internal, ReasonCode::Invariant,
             "builtin callback is not implemented");
    const auto base = parent.operand_base + parent.sp - call.arity;
    RootedValue result(roots_);
    NativeContext context{heap_, roots_, io_, first_, *site.span};
    // native期间禁止VM扩容，实参在回调及GC期间仍属于父帧有效栈。
    target.invoke(context, std::span<const Value>(&store_.value(base), call.arity), result);
    store_.clear(base, base + call.arity);
    frames_.back().sp = parent.sp - call.arity;
    push(result.get());
}
void VM::return_to_caller(RootedValue &result) {
    if (frames_.size() < 2)
        invalid();
    const auto child = frames_.back();
    store_.clear(child.base, child.region_end);
    frames_.pop_back();
    frames_.back().sp = child.caller_arg_base;
    push(result.get());
}
void VM::capture_trace() noexcept {
    if (!trace_)
        return;
    try {
#ifdef MYB_TEST_HOOKS
        TestHooks::hit(FaultPoint::DiagnosticSnapshot);
#endif
        std::vector<CallTraceEntry> entries;
        entries.reserve(frames_.empty() ? 0 : frames_.size() - 1);
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            if (!it->function_id)
                continue;
            const auto &function = program_->functions[*it->function_id];
            entries.push_back({program_->names[function.name],
                               program_->units[it->unit_id].locations[it->executing_ip]});
        }
        *trace_ = std::move(entries);
    } catch (...) {
        // 诊断分配失败只省略链，不替换已有首错。
    }
}
void VM::execute(const CompiledProgram &program) {
    if (!frames_.empty() || program.top_unit >= program.units.size())
        invalid();
    program_ = &program;
    if (trace_)
        trace_->clear();
    try {
        const auto &top = program.units[program.top_unit];
        Frame frame;
        frame.unit_id = program.top_unit;
        frame.operand_base = top.local_count;
        frame.region_end = checked_region_end(top.local_count, top.max_operand,
                                              std::numeric_limits<std::size_t>::max(), first_);
        store_.ensure(std::max<std::size_t>(1, frame.region_end), {first_, {}});
        ensure_frames(1, {first_, {}});
        frames_.push_back(frame);
        for (;;) {
            auto &current = frames_.back();
            const auto &unit = program.units[current.unit_id];
            if (current.next_ip >= unit.instructions.size())
                invalid();
            current.executing_ip = current.next_ip++;
            const auto ins = unit.instructions[current.executing_ip];
            const ErrorSite site{first_, unit.locations[current.executing_ip]};
            const auto top_index = current.operand_base + current.sp;
            auto need = [&](std::size_t count) {
                if (current.sp < count)
                    invalid();
            };
            auto local = [&]() {
                if (ins.a >= unit.local_count)
                    invalid();
                return current.base + ins.a;
            };
            auto drop = [&]() {
                need(1);
                store_.clear(top_index - 1, top_index);
                --current.sp;
            };
            switch (ins.op) {
            case Op::CONST:
                if (ins.a >= unit.constants.size())
                    invalid();
                push(unit.constants[ins.a]);
                break;
            case Op::DUP:
                need(1);
                push(store_.value(top_index - 1));
                break;
            case Op::DROP:
                drop();
                break;
            case Op::DECL_LOCAL: {
                auto index = local();
                if (store_.state(index) != BindingState::Dormant)
                    invalid();
                store_.state(index) = BindingState::Uninitialized;
                break;
            }
            case Op::DECL_GLOBAL:
                globals_.declare(ins.a, site);
                break;
            case Op::LOAD_LOCAL: {
                auto index = local();
                if (store_.state(index) == BindingState::Dormant)
                    fail(site, FailureDomain::Runtime, ReasonCode::Name, "undeclared variable");
                if (store_.state(index) == BindingState::Uninitialized)
                    fail(site, FailureDomain::Runtime, ReasonCode::Uninitialized,
                         "uninitialized variable");
                push(store_.value(index));
                break;
            }
            case Op::LOAD_GLOBAL:
                push(globals_.load(ins.a, site));
                break;
            case Op::STORE_LOCAL: {
                need(1);
                auto index = local();
                if (store_.state(index) == BindingState::Dormant)
                    fail(site, FailureDomain::Runtime, ReasonCode::Name, "undeclared variable");
                store_.value(index) = store_.value(top_index - 1);
                store_.state(index) = BindingState::Initialized;
                drop();
                break;
            }
            case Op::STORE_GLOBAL:
                need(1);
                globals_.store(ins.a, store_.value(top_index - 1), site);
                drop();
                break;
            case Op::CLEAR_LOCALS:
                if (ins.a > unit.local_count || ins.b > unit.local_count - ins.a)
                    invalid();
                store_.clear(current.base + ins.a, current.base + ins.a + ins.b);
                break;
            case Op::UNARY:
                need(1);
                store_.value(top_index - 1) =
                    ValueOps::unary(static_cast<UnaryOp>(ins.a), store_.value(top_index - 1), site);
                break;
            case Op::BINARY: {
                need(2);
                RootedValue result(roots_);
                ValueOps(heap_, roots_)
                    .binary(static_cast<BinaryOp>(ins.a), store_.value(top_index - 2),
                            store_.value(top_index - 1), site, result);
                store_.value(top_index - 2) = result.get();
                drop();
                break;
            }
            case Op::COMPARE: {
                need(2);
                const auto result =
                    ValueOps::compare(static_cast<CompareOp>(ins.a), store_.value(top_index - 2),
                                      store_.value(top_index - 1), site);
                store_.value(top_index - 2) = result;
                drop();
                break;
            }
            case Op::CHECK_BOOL:
                need(1);
                if (!std::holds_alternative<bool>(store_.value(top_index - 1).data))
                    fail(site, FailureDomain::Runtime, ReasonCode::Type, "expected Bool");
                break;
            case Op::JUMP:
                current.next_ip = ins.a;
                break;
            case Op::JUMP_FALSE:
            case Op::JUMP_TRUE: {
                need(1);
                const auto boolean = std::get_if<bool>(&store_.value(top_index - 1).data);
                if (!boolean)
                    fail(site, FailureDomain::Runtime, ReasonCode::Type, "expected Bool");
                const bool value = *boolean;
                drop();
                if (value == (ins.op == Op::JUMP_TRUE))
                    current.next_ip = ins.a;
                break;
            }
            case Op::PRECALL:
                precheck(call_site(unit, ins.a), site);
                break;
            case Op::CALL:
                prepare_call(ins.a, site);
                break;
            case Op::RETURN: {
                need(1);
                RootedValue result(roots_);
                result.set(store_.value(top_index - 1));
                return_to_caller(result);
                break;
            }
            case Op::HALT:
                if (frames_.size() != 1 || current.sp)
                    invalid();
                clear();
                return;
            default:
                fail(site, FailureDomain::Internal, ReasonCode::Invariant,
                     "operation is not implemented in this milestone");
            }
        }
    } catch (const Failure &failure) {
        auto header = failure.header();
        if (!header.span && !frames_.empty())
            header.span =
                program.units[frames_.back().unit_id].locations[frames_.back().executing_ip];
        first_.record_first(header);
        capture_trace();
        clear();
        if (header.domain == FailureDomain::Runtime)
            throw MybRuntimeFailure(header, failure.what());
        throw HostFailure(header, failure.what());
    } catch (const std::bad_alloc &) {
        std::optional<SourceSpan> span;
        if (!frames_.empty())
            span = program.units[frames_.back().unit_id].locations[frames_.back().executing_ip];
        first_.record_first({FailureDomain::Resource, ReasonCode::Allocation, span});
        capture_trace();
        clear();
        throw;
    } catch (...) {
        capture_trace();
        clear();
        throw;
    }
}
void VM::clear() noexcept {
    store_.clear(0, store_.size());
    frames_.clear();
    globals_.clear();
    program_ = nullptr;
}
} // namespace myb
