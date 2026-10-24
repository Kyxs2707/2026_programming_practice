#include "semantic/checker.hpp"
#include "builtins/registry.hpp"
#include "runtime/decimal_codec.hpp"
#include <limits>
namespace myb {
namespace {
constexpr std::string_view base_types[] = {"Int64", "Float64", "Bool", "String", "Array"};

}
CheckedAst Checker::check(const AstProgram &program) {
    literals_.clear();
    declarations_ = {};
    protected_names_.clear();
    struct_names_.clear();
    scopes_.clear();
    in_function_ = false;
    loop_depth_ = 0;
    for (auto name : base_types)
        protected_names_.emplace(name);
    for (const auto &builtin : builtin_descriptors())
        protected_names_.emplace(builtin.name);
    auto global_names = protected_names_;
    // 第一遍检查整个文件的直接声明冲突，不建立运行时绑定。
    for (const auto &item : program.items()) {
        std::string_view name;
        SourceSpan span = item.span();
        if (auto *fn = dynamic_cast<const FunctionDecl *>(&item)) {
            name = fn->name();
            span = fn->name_span();
            declarations_.functions.push_back(fn);
        } else if (auto *structure = dynamic_cast<const StructDecl *>(&item)) {
            name = structure->name();
            span = structure->name_span();
            declarations_.structs.push_back(structure);
        } else if (auto *variable = dynamic_cast<const LetStmt *>(&item)) {
            name = variable->name();
            span = variable->name_span();
        } else {
            continue;
        }
        if (!global_names.emplace(name).second)
            structure_error(span, ReasonCode::Declaration, "conflicting global declaration");
    }
    if (declarations_.functions.size() > UINT32_MAX || declarations_.structs.size() > UINT32_MAX)
        fail({failure_, program.span()}, FailureDomain::Resource, ReasonCode::Capacity,
             "declaration index exceeds uint32 capacity");
    for (auto *fn : declarations_.functions)
        protected_names_.emplace(fn->name());
    for (std::size_t i = 0; i < declarations_.structs.size(); ++i) {
        auto name = declarations_.structs[i]->name();
        protected_names_.emplace(name);
        struct_names_.emplace(name, static_cast<SchemaId>(i));
    }
    // 第二遍在全部结构名已知后解析类型，支持前向和互相引用。
    for (auto *structure : declarations_.structs) {
        std::unordered_set<std::string> names;
        std::vector<FieldType> types;
        for (const auto &field : structure->fields()) {
            if (!names.emplace(field.name).second)
                structure_error(field.name_span, ReasonCode::Declaration,
                                "duplicate structure field");
            FieldType type{TypeKind::Null, {}, field.type.nullable};
            if (field.type.name == "Int64")
                type.kind = TypeKind::Int64;
            else if (field.type.name == "Float64")
                type.kind = TypeKind::Float64;
            else if (field.type.name == "Bool")
                type.kind = TypeKind::Bool;
            else if (field.type.name == "String")
                type.kind = TypeKind::String;
            else if (field.type.name == "Array")
                type.kind = TypeKind::Array;
            else if (struct_names_.contains(field.type.name)) {
                type.kind = TypeKind::Struct;
                type.schema_id = struct_names_.at(field.type.name);
            } else {
                structure_error(field.type.span, ReasonCode::Declaration,
                                "unknown structure field type");
            }
            types.push_back(type);
        }
        declarations_.field_types.push_back(std::move(types));
    }
    // 第三遍检查全部子树；声明集合只表达词法合法性。
    scopes_.emplace_back();
    program.accept(*this);
    scopes_.clear();
    return {&program, std::move(declarations_), std::move(literals_)};
}
[[noreturn]] void Checker::structure_error(SourceSpan span, ReasonCode reason,
                                           std::string_view text) {
    fail({failure_, span}, FailureDomain::Structure, reason, text);
}
void Checker::declare(std::string_view name, SourceSpan span) {
    if (protected_names_.contains(std::string(name)))
        structure_error(span, ReasonCode::Declaration, "declaration uses a protected name");
    if (!scopes_.back().emplace(name).second)
        structure_error(span, ReasonCode::Declaration, "duplicate declaration in scope");
}
void Checker::visit(const AstProgram &n) {
    for (const auto &item : n.items())
        item.accept(*this);
}
void Checker::visit(const FunctionDecl &n) {
    // 函数上下文独立于文件中的循环和块；参数与最外层函数体共用一层。
    scopes_.emplace_back();
    in_function_ = true;
    loop_depth_ = 0;
    for (const auto &parameter : n.parameters())
        declare(parameter.name, parameter.name_span);
    block_contents(n.body());
    scopes_.pop_back();
    in_function_ = false;
}
void Checker::visit(const StructDecl &) {}
std::uint64_t Checker::magnitude(const IntExpr &n) {
    constexpr std::uint64_t limit = std::uint64_t{1} << 63;
    std::uint64_t value = 0;
    for (char c : n.digits()) {
        if (c < '0' || c > '9')
            fail({failure_, n.span()}, FailureDomain::Internal, ReasonCode::Invariant,
                 "invalid integer AST payload");
        auto digit = static_cast<unsigned>(c - '0');
        if (value > (limit - digit) / 10)
            fail({failure_, n.span()}, FailureDomain::Frontend, ReasonCode::Range,
                 "integer literal exceeds Int64 range");
        value = value * 10 + digit;
    }
    return value;
}
void Checker::visit(const IntExpr &n) {
    auto value = magnitude(n);
    if (value > static_cast<std::uint64_t>(INT64_MAX))
        fail({failure_, n.span()}, FailureDomain::Frontend, ReasonCode::Range,
             "positive integer literal exceeds Int64 range");
    literals_.emplace(&n, static_cast<std::int64_t>(value));
}
void Checker::visit(const FloatExpr &n) {
    auto p = DecimalCodec::parse_float(n.text(), ParseMode::SourceLiteral);
    if (!p.value)
        fail({failure_, n.span()}, FailureDomain::Frontend,
             p.error == NumericParseError::Range ? ReasonCode::Range : ReasonCode::Syntax,
             "invalid floating-point literal");
    literals_.emplace(&n, *p.value);
}
void Checker::visit(const StringExpr &n) {
    literals_.emplace(&n, std::string(n.value()));
}
void Checker::visit(const BoolExpr &) {}
void Checker::visit(const NullExpr &) {}
void Checker::visit(const NameExpr &) {}
void Checker::visit(const GroupExpr &n) {
    n.expr().accept(*this);
}
void Checker::visit(const UnaryExpr &n) {
    if (n.op() == UnaryOp::Minus) {
        // 只接纳负号的直接整数孩子；中间的 GroupExpr 会保留并走普通范围检查。
        if (auto *integer = dynamic_cast<const IntExpr *>(&n.operand());
            integer && magnitude(*integer) == (std::uint64_t{1} << 63)) {
            // 以负号节点记录 MIN，跳过不能单独表示为 Int64 的正幅值孩子。
            literals_.emplace(&n, INT64_MIN);
            return;
        }
    }
    n.operand().accept(*this);
}
void Checker::visit(const BinaryExpr &n) {
    n.left().accept(*this);
    n.right().accept(*this);
}
void Checker::visit(const CallExpr &n) {
    for (const auto &arg : n.args())
        arg.accept(*this);
}
void Checker::visit(const ExprStmt &n) {
    n.expr().accept(*this);
}
void Checker::visit(const LogicalExpr &n) {
    n.left().accept(*this);
    n.right().accept(*this);
}
void Checker::visit(const ArrayExpr &n) {
    for (const auto &element : n.elements())
        element.accept(*this);
}
void Checker::visit(const ConstructExpr &n) {
    for (const auto &field : n.fields())
        field.value().accept(*this);
}
void Checker::visit(const FieldExpr &n) {
    n.receiver().accept(*this);
}
void Checker::visit(const IndexExpr &n) {
    n.receiver().accept(*this);
    n.index().accept(*this);
}
void Checker::visit(const SliceExpr &n) {
    n.receiver().accept(*this);
    if (n.start())
        n.start()->accept(*this);
    if (n.end())
        n.end()->accept(*this);
}
void Checker::visit(const LetStmt &n) {
    declare(n.name(), n.name_span());
    if (n.initializer())
        n.initializer()->accept(*this);
}
void Checker::visit(const AssignStmt &n) {
    // 仅剥去目标最外层分组以识别写入形状，保留原树供后续子树检查。
    auto *target = &n.target();
    while (auto *group = dynamic_cast<const GroupExpr *>(target))
        target = &group->expr();
    if (!dynamic_cast<const NameExpr *>(target) && !dynamic_cast<const FieldExpr *>(target) &&
        !dynamic_cast<const IndexExpr *>(target))
        structure_error(n.operator_span(), ReasonCode::Syntax, "invalid assignment target");
    // 形状合法后先检查右侧再检查原始目标；此处不读取绑定或运行值。
    n.value().accept(*this);
    n.target().accept(*this);
}
void Checker::block_contents(const BlockStmt &n) {
    for (const auto &statement : n.statements())
        statement.accept(*this);
}
void Checker::visit(const BlockStmt &n) {
    scopes_.emplace_back();
    block_contents(n);
    scopes_.pop_back();
}
void Checker::visit(const IfStmt &n) {
    n.condition().accept(*this);
    n.then_body().accept(*this);
    if (n.else_body())
        n.else_body()->accept(*this);
}
void Checker::visit(const WhileStmt &n) {
    n.condition().accept(*this);
    ++loop_depth_;
    n.body().accept(*this);
    --loop_depth_;
}
void Checker::visit(const ForStmt &n) {
    // 来源先按外层上下文检查；循环变量与循环体最外层随后共用一个声明集合。
    // block_contents 沿用这一作用域，避免为循环体再开一层。
    n.source().accept(*this);
    scopes_.emplace_back();
    declare(n.name(), n.name_span());
    ++loop_depth_;
    block_contents(n.body());
    --loop_depth_;
    scopes_.pop_back();
}
void Checker::visit(const ReturnStmt &n) {
    if (!in_function_)
        structure_error(n.span(), ReasonCode::Syntax, "return outside function");
    if (n.value())
        n.value()->accept(*this);
}
void Checker::visit(const BreakStmt &n) {
    if (!loop_depth_)
        structure_error(n.span(), ReasonCode::Syntax, "break outside loop");
}
void Checker::visit(const ContinueStmt &n) {
    if (!loop_depth_)
        structure_error(n.span(), ReasonCode::Syntax, "continue outside loop");
}
} // namespace myb
