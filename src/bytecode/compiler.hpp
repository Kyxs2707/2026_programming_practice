#pragma once
#include "ast/visitor.hpp"
#include "bytecode/binding_layout.hpp"
#include "bytecode/program.hpp"
#include "semantic/checked_ast.hpp"
namespace myb {
/**
 * @brief 将完整已检查 AST 生成为独立程序，并核验控制流与操作数容量。
 * 编译器借用的 Heap、RootRegistry 和首错记录须覆盖其使用期。
 */
class BytecodeCompiler final : private AstVisitor {
  public:
    /** @brief 借用同一会话的堆、根登记表和首错记录，不取得其所有权。 */
    BytecodeCompiler(Heap &heap, RootRegistry &roots, EmergencyFailure &first)
        : heap_(heap), roots_(roots), first_(first) {}
    /**
     * @brief 生成顶层、全部函数和结构定义，添加终止指令并计算 max_operand。
     * @param[in] checked 编译期间借用的已检查 AST 及规范化字面量。
     * @param[in,out] out 接收独立拥有的代码表、名称和常量句柄。
     * @pre out.units 为空，且 out 的常量根登记于本编译器使用的 RootRegistry。
     * @throws Failure 字面量记录缺失、编号超限或产物核验失败。
     *
     * 字符串常量创建可触发 GC，先由临时根接收，再交给 out 的常量根集合。
     * 失败时 out 可含部分产物，由调用者随会话清理；成功产物不借用 checked。
     */
    void compile(const CheckedAst &, CompiledProgram &);

  private:
    /** @brief 追加指令及对应源码范围，返回单元内指令编号。 */
    std::uint32_t emit(Op, SourceSpan, std::uint32_t a = 0, std::uint32_t b = 0);
    /** @brief 将跳转修补为当前单元的实际指令编号。 */
    void patch(std::uint32_t, std::uint32_t);
    /** @brief 标记深度零的语句入口，供不可达片段核验使用。 */
    void statement();
    /** @brief 按当前声明环境解析名称，未引入局部时生成全局操作。 */
    void binding(std::string_view, SourceSpan, bool store);
    /** @brief 清除布局给出的词法子树局部区间。 */
    void clear(LocalRange, SourceSpan);
    /** @brief 在运行值求完后生成普通赋值的最终写入位置。 */
    void assign(const Expr &);
    /**
     * @brief 将值加入程序常量根集合，并生成引用该常量的 CONST。
     * @pre 受管 value 在常量表接管前由调用者保活；本操作不触发 MYB GC。
     *
     * 宿主容器增长仍可能抛出异常，调用者的临时根须覆盖整个调用。
     */
    void constant(Value, SourceSpan);
    /** @brief 读取规范化字面量，字符串经临时根创建后再写入常量表。 */
    void literal(const Node &);
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
    Heap &heap_;
    RootRegistry &roots_;
    EmergencyFailure &first_;
    const CheckedAst *checked_{};
    CompiledProgram *program_{};
    CodeUnit *unit_{};
    BindingLayout layout_;
    const UnitLayout *unit_layout_{};
    std::vector<std::unordered_map<std::string, std::uint32_t>> scopes_;
    /** @brief 保存最内循环的重入目标、退出修补列表和局部清理范围。 */
    struct Loop {
        std::uint32_t next;
        LocalRange locals;
        std::vector<std::uint32_t> breaks;
    };
    std::vector<Loop> loops_;
    std::vector<std::uint32_t> active_iterations_;
};
} // namespace myb
