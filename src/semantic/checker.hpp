#pragma once
#include "diagnostic/failure.hpp"
#include "semantic/checked_ast.hpp"
#include <unordered_set>
namespace myb {
/**
 * @brief 三遍检查顶层声明、结构字段类型与全部子树，并生成字面量侧表。
 *
 * 检查器不求运行值；名称存在性、调用数量和构造字段集合留至运行时。
 * 借用的 EmergencyFailure 须比检查器存活更久。
 */
class Checker final : public AstVisitor {
  public:
    explicit Checker(EmergencyFailure &failure) : failure_(failure) {}
    /**
     * @brief 清除上次临时结果，检查整棵树并交付本次侧表。
     * @return 借用输入程序、独占侧表的结果；调用方负责输入树的寿命。
     * @throws FrontendFailure 发现超范围字面量、非法声明或控制语句。
     * @throws HostFailure AST 整数载荷违反内部约束，或声明编号超过 uint32 容量。
     * @throws std::bad_alloc 侧表或宿主字符串分配失败。
     * 任一异常均终止本次检查，不交付部分结果。
     */
    CheckedAst check(const AstProgram &);
    void visit(const AstProgram &) override;
    void visit(const FunctionDecl &) override;
    void visit(const StructDecl &) override;
    void visit(const IntExpr &) override;
    void visit(const FloatExpr &) override;
    void visit(const StringExpr &) override;
    void visit(const BoolExpr &) override;
    void visit(const NullExpr &) override;
    void visit(const NameExpr &) override;
    void visit(const GroupExpr &) override;
    void visit(const UnaryExpr &) override;
    void visit(const BinaryExpr &) override;
    void visit(const LogicalExpr &) override;
    void visit(const CallExpr &) override;
    void visit(const ArrayExpr &) override;
    void visit(const ConstructExpr &) override;
    void visit(const FieldExpr &) override;
    void visit(const IndexExpr &) override;
    void visit(const SliceExpr &) override;
    void visit(const LetStmt &) override;
    void visit(const AssignStmt &) override;
    void visit(const ExprStmt &) override;
    void visit(const BlockStmt &) override;
    void visit(const IfStmt &) override;
    void visit(const WhileStmt &) override;
    void visit(const ForStmt &) override;
    void visit(const ReturnStmt &) override;
    void visit(const BreakStmt &) override;
    void visit(const ContinueStmt &) override;

  private:
    /**
     * @brief 按十进制逐位读取整数幅值，并在乘加前检查上限。
     * @return 0～2^63 的无符号幅值，保留最小负整数所需的额外一个正幅值。
     * @throws FrontendFailure 幅值超过 2^63。
     * @throws HostFailure 词素含非十进制数字，属于 AST 内部约束失败。
     */
    std::uint64_t magnitude(const IntExpr &);
    /** @brief 在当前词法层登记名称，拒绝重复声明及受保护名称。 */
    void declare(std::string_view, SourceSpan);
    /** @brief 在已有词法层检查块内语句，供函数参数与 for 变量共享该层。 */
    void block_contents(const BlockStmt &);
    /** @brief 对结构错误登记首错并终止检查。 */
    [[noreturn]] void structure_error(SourceSpan, ReasonCode, std::string_view);
    DeclarationIndex declarations_;
    std::unordered_set<std::string> protected_names_;
    std::unordered_map<std::string, SchemaId> struct_names_;
    std::vector<std::unordered_set<std::string>> scopes_;
    bool in_function_{};
    std::size_t loop_depth_{};
    EmergencyFailure &failure_;
    LiteralTable literals_;
};
} // namespace myb
