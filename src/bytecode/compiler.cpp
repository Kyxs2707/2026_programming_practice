#include "bytecode/compiler.hpp"
#include "bytecode/verify.hpp"
#include "runtime/text_builder.hpp"
#include <limits>
#ifdef MYB_TEST_HOOKS
#include "test_hooks.hpp"
#endif
namespace myb {
namespace {
/** @brief 检查表长度可转为字节码编号，达到上限时保存容量失败。 */
std::uint32_t index_of(std::size_t n, ErrorSite site) {
    if (n >= std::numeric_limits<std::uint32_t>::max())
        fail(site, FailureDomain::Resource, ReasonCode::Capacity, "bytecode table is too large");
    return static_cast<std::uint32_t>(n);
}
} // namespace
void BytecodeCompiler::compile(const CheckedAst &checked, CompiledProgram &out) {
    if (!out.units.empty())
        fail({first_, {}}, FailureDomain::Internal, ReasonCode::Invariant,
             "program already compiled");
    checked_ = &checked;
    program_ = &out;
    layout_ = BindingLayout::build(checked);
    out.units.resize(layout_.units.size());
    std::vector<const FunctionDecl *> functions;
    std::vector<const StructDecl *> structures;
    for (const auto &item : checked.program->items()) {
        if (auto fn = dynamic_cast<const FunctionDecl *>(&item)) {
            auto id = index_of(functions.size() + 1, {first_, fn->span()});
            out.functions.push_back({out.intern_name(fn->name(), first_),
                                     index_of(fn->parameters().size(), {first_, fn->span()}), id,
                                     fn->name_span()});
            functions.push_back(fn);
        } else if (auto type = dynamic_cast<const StructDecl *>(&item)) {
            auto schema = std::make_unique<StructSchema>();
            schema->id = index_of(out.schemas.size(), {first_, type->span()});
            schema->name = type->name();
            out.intern_name(type->name(), first_);
            out.schemas.push_back(std::move(schema));
            structures.push_back(type);
        }
    }
    for (std::size_t i = 0; i < structures.size(); ++i) {
        for (const auto &field : structures[i]->fields()) {
            FieldType type{TypeKind::Null, {}, field.type.nullable};
            const auto &name = field.type.name;
            if (name == "Bool")
                type.kind = TypeKind::Bool;
            else if (name == "Int64")
                type.kind = TypeKind::Int64;
            else if (name == "Float64")
                type.kind = TypeKind::Float64;
            else if (name == "String")
                type.kind = TypeKind::String;
            else if (name == "Array")
                type.kind = TypeKind::Array;
            else if (name != "Null") {
                type.kind = TypeKind::Struct;
                for (const auto &schema : out.schemas)
                    if (schema->name == name)
                        type.schema_id = schema->id;
                if (!type.schema_id)
                    fail({first_, field.type.span}, FailureDomain::Internal, ReasonCode::Invariant,
                         "checked field type is missing");
            }
            out.schemas[i]->fields.push_back({field.name, type});
            out.intern_name(field.name, first_);
        }
    }
    for (std::size_t i = 0; i < out.units.size(); ++i) {
        unit_ = &out.units[i];
        unit_layout_ = &layout_.units[i];
        unit_->local_count = unit_layout_->local_count;
        unit_->parameter_count = unit_layout_->parameter_count;
        scopes_.clear();
        loops_.clear();
        active_iterations_.clear();
        if (i == 0) {
            checked.program->accept(*this);
            emit(Op::HALT, checked.program->span());
        } else {
            scopes_.emplace_back();
            std::uint32_t parameter = 0;
            for (const auto &p : functions[i - 1]->parameters())
                scopes_.back().emplace(p.name, parameter++);
            functions[i - 1]->body().accept(*this);
            statement();
            constant(Value{NullValue{}}, functions[i - 1]->span());
            emit(Op::RETURN, functions[i - 1]->span());
        }
        verify(*unit_, first_);
    }
    verify(out, first_);
    checked_ = nullptr;
    unit_ = nullptr;
    unit_layout_ = nullptr;
    program_ = nullptr;
}
std::uint32_t BytecodeCompiler::emit(Op op, SourceSpan span, std::uint32_t a, std::uint32_t b) {
    const auto id = index_of(unit_->instructions.size(), {first_, span});
    unit_->instructions.push_back({op, a, b});
    unit_->locations.push_back(span);
    return id;
}
void BytecodeCompiler::patch(std::uint32_t instruction, std::uint32_t target) {
    unit_->instructions[instruction].a = target;
}
void BytecodeCompiler::statement() {
    unit_->statement_boundaries.push_back(index_of(unit_->instructions.size(), {first_, {}}));
    unit_->boundary_iterations.push_back(active_iterations_);
}
void BytecodeCompiler::clear(LocalRange range, SourceSpan span) {
    if (range.count)
        emit(Op::CLEAR_LOCALS, span, range.first, range.count);
}
void BytecodeCompiler::binding(std::string_view name, SourceSpan span, bool store) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto found = scope->find(std::string(name));
        if (found != scope->end()) {
            emit(store ? Op::STORE_LOCAL : Op::LOAD_LOCAL, span, found->second);
            return;
        }
    }
    emit(store ? Op::STORE_GLOBAL : Op::LOAD_GLOBAL, span, program_->intern_name(name, first_));
}
void BytecodeCompiler::constant(Value value, SourceSpan span) {
    auto index = index_of(unit_->constants.size(), {first_, span});
#ifdef MYB_TEST_HOOKS
    TestHooks::hit(FaultPoint::ProgramConstants);
#endif
    // 宿主表增长不触发 MYB GC；写入后由程序常量集合接管。
    unit_->constants.push_back(value);
    emit(Op::CONST, span, index);
}
void BytecodeCompiler::literal(const Node &node) {
    auto found = checked_->literals.find(&node);
    if (found == checked_->literals.end())
        fail({first_, node.span()}, FailureDomain::Internal, ReasonCode::Invariant,
             "missing normalized literal");
    std::visit(
        [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string>) {
                RootedValue result(roots_);
                TextBuilder builder(heap_);
                builder.append(std::span<const char>(value.data(), value.size()));
                heap_.make_string(std::move(builder), result);
                constant(result.get(), node.span());
            } else
                constant(Value{value}, node.span());
        },
        found->second);
}
void BytecodeCompiler::visit(const AstProgram &n) {
    for (const auto &item : n.items())
        item.accept(*this);
}
void BytecodeCompiler::visit(const FunctionDecl &) {}
void BytecodeCompiler::visit(const StructDecl &) {}
void BytecodeCompiler::visit(const IntExpr &n) {
    literal(n);
}
void BytecodeCompiler::visit(const FloatExpr &n) {
    literal(n);
}
void BytecodeCompiler::visit(const StringExpr &n) {
    literal(n);
}
void BytecodeCompiler::visit(const BoolExpr &n) {
    constant(Value{n.value()}, n.span());
}
void BytecodeCompiler::visit(const NullExpr &n) {
    constant(Value{NullValue{}}, n.span());
}
void BytecodeCompiler::visit(const NameExpr &n) {
    binding(n.name(), n.name_span(), false);
}
void BytecodeCompiler::visit(const GroupExpr &n) {
    n.expr().accept(*this);
}
void BytecodeCompiler::visit(const UnaryExpr &n) {
    if (checked_->literals.contains(&n)) {
        literal(n);
        return;
    }
    n.operand().accept(*this);
    emit(Op::UNARY, n.operator_span(), static_cast<std::uint32_t>(n.op()));
}
void BytecodeCompiler::visit(const BinaryExpr &n) {
    n.left().accept(*this);
    n.right().accept(*this);
    std::visit(
        [&](auto op) {
            using T = decltype(op);
            emit(std::is_same_v<T, BinaryOp> ? Op::BINARY : Op::COMPARE, n.operator_span(),
                 static_cast<std::uint32_t>(op));
        },
        n.op());
}
void BytecodeCompiler::visit(const LogicalExpr &n) {
    n.left().accept(*this);
    emit(Op::DUP, n.operator_span());
    const auto jump =
        emit(n.op() == LogicalOp::And ? Op::JUMP_FALSE : Op::JUMP_TRUE, n.operator_span());
    emit(Op::DROP, n.operator_span());
    n.right().accept(*this);
    emit(Op::CHECK_BOOL, n.operator_span());
    patch(jump, index_of(unit_->instructions.size(), {first_, n.span()}));
}
void BytecodeCompiler::visit(const CallExpr &n) {
    auto index = index_of(unit_->calls.size(), {first_, n.span()});
    unit_->calls.push_back(
        {program_->intern_name(n.name(), first_), index_of(n.args().size(), {first_, n.span()})});
    emit(Op::PRECALL, n.span(), index);
    for (const auto &arg : n.args())
        arg.accept(*this);
    emit(Op::CALL, n.span(), index);
}
void BytecodeCompiler::visit(const ArrayExpr &n) {
    for (const auto &item : n.elements())
        item.accept(*this);
    emit(Op::BUILD_ARRAY, n.span(), index_of(n.elements().size(), {first_, n.span()}));
}
void BytecodeCompiler::visit(const ConstructExpr &n) {
    const auto id = index_of(unit_->constructions.size(), {first_, n.span()});
    ConstructionPlan plan{program_->intern_name(n.name(), first_), {}};
    for (const auto &field : n.fields())
        plan.fields.push_back(program_->intern_name(field.name(), first_));
    unit_->constructions.push_back(std::move(plan));
    emit(Op::CHECK_STRUCT, n.name_span(), id);
    std::uint32_t position = 0;
    for (const auto &field : n.fields()) {
        field.value().accept(*this);
        emit(Op::CHECK_FIELD, field.name_span(), id, position++);
    }
    emit(Op::BUILD_STRUCT, n.span(), id);
}
void BytecodeCompiler::visit(const FieldExpr &n) {
    n.receiver().accept(*this);
    emit(Op::GET_FIELD, n.access_span(), program_->intern_name(n.name(), first_));
}
void BytecodeCompiler::visit(const IndexExpr &n) {
    n.receiver().accept(*this);
    n.index().accept(*this);
    emit(Op::GET_INDEX, n.access_span());
}
void BytecodeCompiler::visit(const SliceExpr &n) {
    n.receiver().accept(*this);
    if (n.start())
        n.start()->accept(*this);
    if (n.end())
        n.end()->accept(*this);
    emit(Op::GET_SLICE, n.access_span(), (n.start() ? 1u : 0u) | (n.end() ? 2u : 0u));
}
void BytecodeCompiler::visit(const LetStmt &n) {
    statement();
    if (scopes_.empty())
        emit(Op::DECL_GLOBAL, n.name_span(), program_->intern_name(n.name(), first_));
    else {
        const auto slot = unit_layout_->slots.at(&n);
        scopes_.back().emplace(n.name(), slot);
        emit(Op::DECL_LOCAL, n.name_span(), slot);
    }
    if (n.initializer()) {
        n.initializer()->accept(*this);
        binding(n.name(), n.name_span(), true);
    }
}
void BytecodeCompiler::assign(const Expr &target) {
    if (auto n = dynamic_cast<const GroupExpr *>(&target))
        assign(n->expr());
    else if (auto n = dynamic_cast<const NameExpr *>(&target))
        binding(n->name(), n->name_span(), true);
    else if (auto n = dynamic_cast<const FieldExpr *>(&target)) {
        n->receiver().accept(*this);
        emit(Op::SET_FIELD, n->access_span(), program_->intern_name(n->name(), first_));
    } else if (auto n = dynamic_cast<const IndexExpr *>(&target)) {
        n->receiver().accept(*this);
        n->index().accept(*this);
        emit(Op::SET_INDEX, n->access_span());
    } else
        fail({first_, target.span()}, FailureDomain::Internal, ReasonCode::Invariant,
             "invalid checked assignment");
}
void BytecodeCompiler::visit(const AssignStmt &n) {
    statement();
    n.value().accept(*this);
    assign(n.target());
}
void BytecodeCompiler::visit(const ExprStmt &n) {
    statement();
    n.expr().accept(*this);
    emit(Op::DROP, n.span());
}
void BytecodeCompiler::visit(const BlockStmt &n) {
    statement();
    scopes_.emplace_back();
    for (const auto &stmt : n.statements())
        stmt.accept(*this);
    statement();
    clear(unit_layout_->ranges.at(&n), n.span());
    scopes_.pop_back();
}
void BytecodeCompiler::visit(const IfStmt &n) {
    statement();
    n.condition().accept(*this);
    auto alternative = emit(Op::JUMP_FALSE, n.condition().span());
    n.then_body().accept(*this);
    auto end = emit(Op::JUMP, n.span());
    patch(alternative, index_of(unit_->instructions.size(), {first_, n.span()}));
    if (n.else_body())
        n.else_body()->accept(*this);
    patch(end, index_of(unit_->instructions.size(), {first_, n.span()}));
}
void BytecodeCompiler::visit(const WhileStmt &n) {
    statement();
    auto start = index_of(unit_->instructions.size(), {first_, n.span()});
    n.condition().accept(*this);
    auto done = emit(Op::JUMP_FALSE, n.condition().span());
    loops_.push_back({start, unit_layout_->ranges.at(&n), {}});
    n.body().accept(*this);
    emit(Op::JUMP, n.span(), start);
    const auto end = index_of(unit_->instructions.size(), {first_, n.span()});
    patch(done, end);
    for (auto jump : loops_.back().breaks)
        patch(jump, end);
    loops_.pop_back();
}
void BytecodeCompiler::visit(const ForStmt &n) {
    statement();
    n.source().accept(*this);
    auto site = index_of(unit_->iterations.size(), {first_, n.span()});
    const auto slot = unit_layout_->slots.at(&n);
    unit_->iterations.push_back({slot});
    emit(Op::ITER_BEGIN, n.source().span(), site);
    active_iterations_.push_back(site);
    auto next = emit(Op::ITER_NEXT, n.span(), site);
    scopes_.emplace_back();
    scopes_.back().emplace(n.name(), slot);
    loops_.push_back({next, unit_layout_->ranges.at(&n), {}});
    n.body().accept(*this);
    clear(unit_layout_->ranges.at(&n), n.span());
    emit(Op::JUMP, n.span(), next);
    const auto end = emit(Op::ITER_END, n.span(), site);
    active_iterations_.pop_back();
    unit_->instructions[next].b = end;
    for (auto jump : loops_.back().breaks)
        patch(jump, end);
    loops_.pop_back();
    scopes_.pop_back();
}
void BytecodeCompiler::visit(const ReturnStmt &n) {
    statement();
    if (n.value())
        n.value()->accept(*this);
    else
        constant(Value{NullValue{}}, n.span());
    emit(Op::RETURN, n.span());
}
void BytecodeCompiler::visit(const BreakStmt &n) {
    statement();
    clear(loops_.back().locals, n.span());
    loops_.back().breaks.push_back(emit(Op::JUMP, n.span()));
}
void BytecodeCompiler::visit(const ContinueStmt &n) {
    statement();
    clear(loops_.back().locals, n.span());
    emit(Op::JUMP, n.span(), loops_.back().next);
}
} // namespace myb
