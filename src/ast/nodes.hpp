#pragma once
#include "ast/node.hpp"
#include "ast/visitor.hpp"
#include "runtime/operators.hpp"
#include <span>
#include <string>
#include <string_view>
#include <variant>
namespace myb {
/** @brief 二元节点保存的算术运算或比较运算种类。 */
using ArithmeticOrCompare = std::variant<BinaryOp, CompareOp>;
/** @brief 结构字段的类型拼写；名称和可空标记留待声明检查解析。 */
struct TypeRef {
    std::string name;
    bool nullable;
    SourceSpan span;
};
/** @brief 函数形参的名称及诊断位置，不单独计入 AST 高度。 */
struct Parameter {
    std::string name;
    SourceSpan name_span;
};
/** @brief 按声明顺序保存的结构字段记录，不单独形成 AST 节点。 */
struct FieldDecl {
    std::string name;
    SourceSpan name_span;
    TypeRef type;
};
/**
 * @brief 独占一个结构构造字段的名称和取值表达式。
 * 构造接收非空表达式；value() 和 name() 的结果借用本记录。
 * 字段记录保留源码顺序及重复名称，供后续阶段按构造规则处理。
 */
class FieldInit {
  public:
    FieldInit(std::string n, SourceSpan s, std::unique_ptr<Expr> v)
        : name_(std::move(n)), span_(s), value_(std::move(v)) {}
    std::string_view name() const {
        return name_;
    }
    SourceSpan name_span() const {
        return span_;
    }
    const Expr &value() const {
        return *value_;
    }

  private:
    std::string name_;
    SourceSpan span_;
    std::unique_ptr<Expr> value_;
};
/**
 * @brief 独占按源码顺序排列的文件顶层项目。
 *
 * 本文件的节点类型覆盖语言规范的完整语法结构。
 * 各节点构造函数接收并拥有名称、记录序列和子树；必选子节点须非空，
 * 位置与高度由 AstFactory 传入。只读访问结果遵循 Node 的借用约定。
 */
class AstProgram final : public Node {
  public:
    AstProgram(SourceSpan, std::uint16_t, std::vector<std::unique_ptr<TopItem>> items);
    ~AstProgram() noexcept override;
    void accept(AstVisitor &) const override;
    const ReadOnlyNodes<TopItem> &items() const noexcept {
        return items_;
    }

  private:
    ReadOnlyNodes<TopItem> items_;
};
/** @brief 保存命名函数的有序参数和非空函数体。 */
class FunctionDecl final : public TopItem {
  public:
    FunctionDecl(SourceSpan, std::uint16_t, std::string name, SourceSpan name_span,
                 std::vector<Parameter> parameters, std::unique_ptr<BlockStmt> body);
    ~FunctionDecl() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view name() const noexcept {
        return name_;
    }
    SourceSpan name_span() const noexcept {
        return name_span_;
    }
    std::span<const Parameter> parameters() const noexcept {
        return parameters_;
    }
    const BlockStmt &body() const noexcept {
        return *body_;
    }

  private:
    std::string name_;
    SourceSpan name_span_;
    std::vector<Parameter> parameters_;
    std::unique_ptr<BlockStmt> body_;
};
/** @brief 保存结构名称和有序字段声明。 */
class StructDecl final : public TopItem {
  public:
    StructDecl(SourceSpan, std::uint16_t, std::string name, SourceSpan name_span,
               std::vector<FieldDecl> fields);
    ~StructDecl() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view name() const noexcept {
        return name_;
    }
    SourceSpan name_span() const noexcept {
        return name_span_;
    }
    std::span<const FieldDecl> fields() const noexcept {
        return fields_;
    }

  private:
    std::string name_;
    SourceSpan name_span_;
    std::vector<FieldDecl> fields_;
};
/** @brief 拥有无符号十进制整数词素，幅值合法性由检查器判定。 */
class IntExpr final : public Expr {
  public:
    IntExpr(SourceSpan, std::uint16_t, std::string digits);
    ~IntExpr() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view digits() const noexcept {
        return digits_;
    }

  private:
    std::string digits_;
};
/** @brief 拥有原始浮点词素，数值转换由检查器调用 DecimalCodec 完成。 */
class FloatExpr final : public Expr {
  public:
    FloatExpr(SourceSpan, std::uint16_t, std::string text);
    ~FloatExpr() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view text() const noexcept {
        return text_;
    }

  private:
    std::string text_;
};
/** @brief 拥有扫描器完成转义解码后的 UTF-8 字符串。 */
class StringExpr final : public Expr {
  public:
    StringExpr(SourceSpan, std::uint16_t, std::string value);
    ~StringExpr() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view value() const noexcept {
        return value_;
    }

  private:
    std::string value_;
};
/** @brief 保存 true 或 false 字面量的布尔值。 */
class BoolExpr final : public Expr {
  public:
    BoolExpr(SourceSpan, std::uint16_t, bool value);
    ~BoolExpr() noexcept override;
    void accept(AstVisitor &) const override;
    bool value() const noexcept {
        return value_;
    }

  private:
    bool value_;
};
/** @brief 表示显式 null 字面量，与可选子节点的空指针区别。 */
class NullExpr final : public Expr {
  public:
    NullExpr(SourceSpan, std::uint16_t);
    ~NullExpr() noexcept override;
    void accept(AstVisitor &) const override;

  private:
};
/** @brief 保存待读取的名称和名称位置；节点自身不绑定运行时变量。 */
class NameExpr final : public Expr {
  public:
    NameExpr(SourceSpan, std::uint16_t, std::string name, SourceSpan name_span);
    ~NameExpr() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view name() const noexcept {
        return name_;
    }
    SourceSpan name_span() const noexcept {
        return name_span_;
    }

  private:
    std::string name_;
    SourceSpan name_span_;
};
/** @brief 独占非空内部表达式并保留括号范围，供检查器识别直接操作数。 */
class GroupExpr final : public Expr {
  public:
    GroupExpr(SourceSpan, std::uint16_t, std::unique_ptr<Expr> expr);
    ~GroupExpr() noexcept override;
    void accept(AstVisitor &) const override;
    const Expr &expr() const noexcept {
        return *expr_;
    }

  private:
    std::unique_ptr<Expr> expr_;
};
/** @brief 保存一元操作种类、操作符位置和非空操作数。 */
class UnaryExpr final : public Expr {
  public:
    UnaryExpr(SourceSpan, std::uint16_t, UnaryOp op, SourceSpan operator_span,
              std::unique_ptr<Expr> operand);
    ~UnaryExpr() noexcept override;
    void accept(AstVisitor &) const override;
    UnaryOp op() const noexcept {
        return op_;
    }
    SourceSpan operator_span() const noexcept {
        return operator_span_;
    }
    const Expr &operand() const noexcept {
        return *operand_;
    }

  private:
    UnaryOp op_;
    SourceSpan operator_span_;
    std::unique_ptr<Expr> operand_;
};
/** @brief 独占算术或比较操作的左右表达式，并保存操作符诊断位置。 */
class BinaryExpr final : public Expr {
  public:
    BinaryExpr(SourceSpan, std::uint16_t, ArithmeticOrCompare op, SourceSpan operator_span,
               std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
    ~BinaryExpr() noexcept override;
    void accept(AstVisitor &) const override;
    ArithmeticOrCompare op() const noexcept {
        return op_;
    }
    SourceSpan operator_span() const noexcept {
        return operator_span_;
    }
    const Expr &left() const noexcept {
        return *left_;
    }
    const Expr &right() const noexcept {
        return *right_;
    }

  private:
    ArithmeticOrCompare op_;
    SourceSpan operator_span_;
    std::unique_ptr<Expr> left_;
    std::unique_ptr<Expr> right_;
};
/** @brief 保存逻辑操作及两侧表达式。 */
class LogicalExpr final : public Expr {
  public:
    LogicalExpr(SourceSpan, std::uint16_t, LogicalOp op, SourceSpan operator_span,
                std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
    ~LogicalExpr() noexcept override;
    void accept(AstVisitor &) const override;
    LogicalOp op() const noexcept {
        return op_;
    }
    SourceSpan operator_span() const noexcept {
        return operator_span_;
    }
    const Expr &left() const noexcept {
        return *left_;
    }
    const Expr &right() const noexcept {
        return *right_;
    }

  private:
    LogicalOp op_;
    SourceSpan operator_span_;
    std::unique_ptr<Expr> left_;
    std::unique_ptr<Expr> right_;
};
/** @brief 保存被调用名称及有序实参；调用目标采用名称而非任意表达式。 */
class CallExpr final : public Expr {
  public:
    CallExpr(SourceSpan, std::uint16_t, std::string name, SourceSpan name_span,
             std::vector<std::unique_ptr<Expr>> args);
    ~CallExpr() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view name() const noexcept {
        return name_;
    }
    SourceSpan name_span() const noexcept {
        return name_span_;
    }
    const ReadOnlyNodes<Expr> &args() const noexcept {
        return args_;
    }

  private:
    std::string name_;
    SourceSpan name_span_;
    ReadOnlyNodes<Expr> args_;
};
/** @brief 独占按源码顺序排列的数组元素。 */
class ArrayExpr final : public Expr {
  public:
    ArrayExpr(SourceSpan, std::uint16_t, std::vector<std::unique_ptr<Expr>> elements);
    ~ArrayExpr() noexcept override;
    void accept(AstVisitor &) const override;
    const ReadOnlyNodes<Expr> &elements() const noexcept {
        return elements_;
    }

  private:
    ReadOnlyNodes<Expr> elements_;
};
/** @brief 保存结构类型名及有序字段取值记录。 */
class ConstructExpr final : public Expr {
  public:
    ConstructExpr(SourceSpan, std::uint16_t, std::string name, SourceSpan name_span,
                  std::vector<FieldInit> fields);
    ~ConstructExpr() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view name() const noexcept {
        return name_;
    }
    SourceSpan name_span() const noexcept {
        return name_span_;
    }
    std::span<const FieldInit> fields() const noexcept {
        return fields_;
    }

  private:
    std::string name_;
    SourceSpan name_span_;
    std::vector<FieldInit> fields_;
};
/** @brief 保存接收表达式、字段名与访问位置。 */
class FieldExpr final : public Expr {
  public:
    FieldExpr(SourceSpan, std::uint16_t, std::unique_ptr<Expr> receiver, std::string name,
              SourceSpan name_span, SourceSpan access_span);
    ~FieldExpr() noexcept override;
    void accept(AstVisitor &) const override;
    const Expr &receiver() const noexcept {
        return *receiver_;
    }
    std::string_view name() const noexcept {
        return name_;
    }
    SourceSpan name_span() const noexcept {
        return name_span_;
    }
    SourceSpan access_span() const noexcept {
        return access_span_;
    }

  private:
    std::unique_ptr<Expr> receiver_;
    std::string name_;
    SourceSpan name_span_;
    SourceSpan access_span_;
};
/** @brief 保存接收表达式、下标和访问位置。 */
class IndexExpr final : public Expr {
  public:
    IndexExpr(SourceSpan, std::uint16_t, std::unique_ptr<Expr> receiver,
              std::unique_ptr<Expr> index, SourceSpan access_span);
    ~IndexExpr() noexcept override;
    void accept(AstVisitor &) const override;
    const Expr &receiver() const noexcept {
        return *receiver_;
    }
    const Expr &index() const noexcept {
        return *index_;
    }
    SourceSpan access_span() const noexcept {
        return access_span_;
    }

  private:
    std::unique_ptr<Expr> receiver_;
    std::unique_ptr<Expr> index_;
    SourceSpan access_span_;
};
/**
 * @brief 保存切片接收表达式和可省略端点。
 * start() 或 end() 返回空指针表示省略该端点，显式 null 则为 NullExpr。
 */
class SliceExpr final : public Expr {
  public:
    SliceExpr(SourceSpan, std::uint16_t, std::unique_ptr<Expr> receiver,
              std::unique_ptr<Expr> start, std::unique_ptr<Expr> end, SourceSpan access_span);
    ~SliceExpr() noexcept override;
    void accept(AstVisitor &) const override;
    const Expr &receiver() const noexcept {
        return *receiver_;
    }
    const Expr *start() const noexcept {
        return start_.get();
    }
    const Expr *end() const noexcept {
        return end_.get();
    }
    SourceSpan access_span() const noexcept {
        return access_span_;
    }

  private:
    std::unique_ptr<Expr> receiver_;
    std::unique_ptr<Expr> start_;
    std::unique_ptr<Expr> end_;
    SourceSpan access_span_;
};
/**
 * @brief 保存变量声明和可选初始化表达式。
 * initializer() 返回空指针表示源码未提供初始化式。
 */
class LetStmt final : public Stmt {
  public:
    LetStmt(SourceSpan, std::uint16_t, std::string name, SourceSpan name_span,
            std::unique_ptr<Expr> initializer);
    ~LetStmt() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view name() const noexcept {
        return name_;
    }
    SourceSpan name_span() const noexcept {
        return name_span_;
    }
    const Expr *initializer() const noexcept {
        return initializer_.get();
    }

  private:
    std::string name_;
    SourceSpan name_span_;
    std::unique_ptr<Expr> initializer_;
};
/** @brief 保留原始赋值目标、右侧和赋值符位置。 */
class AssignStmt final : public Stmt {
  public:
    AssignStmt(SourceSpan, std::uint16_t, std::unique_ptr<Expr> target, std::unique_ptr<Expr> value,
               SourceSpan operator_span);
    ~AssignStmt() noexcept override;
    void accept(AstVisitor &) const override;
    const Expr &target() const noexcept {
        return *target_;
    }
    const Expr &value() const noexcept {
        return *value_;
    }
    SourceSpan operator_span() const noexcept {
        return operator_span_;
    }

  private:
    std::unique_ptr<Expr> target_;
    std::unique_ptr<Expr> value_;
    SourceSpan operator_span_;
};
/** @brief 独占一条以分号结束的表达式语句所含的非空表达式。 */
class ExprStmt final : public Stmt {
  public:
    ExprStmt(SourceSpan, std::uint16_t, std::unique_ptr<Expr> expr);
    ~ExprStmt() noexcept override;
    void accept(AstVisitor &) const override;
    const Expr &expr() const noexcept {
        return *expr_;
    }

  private:
    std::unique_ptr<Expr> expr_;
};
/** @brief 独占有序语句列表，可表示空块。 */
class BlockStmt final : public Stmt {
  public:
    BlockStmt(SourceSpan, std::uint16_t, std::vector<std::unique_ptr<Stmt>> statements);
    ~BlockStmt() noexcept override;
    void accept(AstVisitor &) const override;
    const ReadOnlyNodes<Stmt> &statements() const noexcept {
        return statements_;
    }

  private:
    ReadOnlyNodes<Stmt> statements_;
};
/**
 * @brief 保存条件、then 块与可选 else 分支。
 * else_body() 返回空指针表示没有 else；非空分支按设计为 IfStmt 或 BlockStmt。
 */
class IfStmt final : public Stmt {
  public:
    IfStmt(SourceSpan, std::uint16_t, std::unique_ptr<Expr> condition,
           std::unique_ptr<BlockStmt> then_body, std::unique_ptr<Stmt> else_body);
    ~IfStmt() noexcept override;
    void accept(AstVisitor &) const override;
    const Expr &condition() const noexcept {
        return *condition_;
    }
    const BlockStmt &then_body() const noexcept {
        return *then_body_;
    }
    const Stmt *else_body() const noexcept {
        return else_body_.get();
    }

  private:
    std::unique_ptr<Expr> condition_;
    std::unique_ptr<BlockStmt> then_body_;
    std::unique_ptr<Stmt> else_body_;
};
/** @brief 保存 while 条件和非空循环体。 */
class WhileStmt final : public Stmt {
  public:
    WhileStmt(SourceSpan, std::uint16_t, std::unique_ptr<Expr> condition,
              std::unique_ptr<BlockStmt> body);
    ~WhileStmt() noexcept override;
    void accept(AstVisitor &) const override;
    const Expr &condition() const noexcept {
        return *condition_;
    }
    const BlockStmt &body() const noexcept {
        return *body_;
    }

  private:
    std::unique_ptr<Expr> condition_;
    std::unique_ptr<BlockStmt> body_;
};
/** @brief 保存循环变量名、来源表达式和循环体。 */
class ForStmt final : public Stmt {
  public:
    ForStmt(SourceSpan, std::uint16_t, std::string name, SourceSpan name_span,
            std::unique_ptr<Expr> source, std::unique_ptr<BlockStmt> body);
    ~ForStmt() noexcept override;
    void accept(AstVisitor &) const override;
    std::string_view name() const noexcept {
        return name_;
    }
    SourceSpan name_span() const noexcept {
        return name_span_;
    }
    const Expr &source() const noexcept {
        return *source_;
    }
    const BlockStmt &body() const noexcept {
        return *body_;
    }

  private:
    std::string name_;
    SourceSpan name_span_;
    std::unique_ptr<Expr> source_;
    std::unique_ptr<BlockStmt> body_;
};
/**
 * @brief 保存可选返回表达式。
 * value() 返回空指针表示源码中没有返回表达式。
 */
class ReturnStmt final : public Stmt {
  public:
    ReturnStmt(SourceSpan, std::uint16_t, std::unique_ptr<Expr> value);
    ~ReturnStmt() noexcept override;
    void accept(AstVisitor &) const override;
    const Expr *value() const noexcept {
        return value_.get();
    }

  private:
    std::unique_ptr<Expr> value_;
};
/** @brief 表示 break 的源码位置。 */
class BreakStmt final : public Stmt {
  public:
    BreakStmt(SourceSpan, std::uint16_t);
    ~BreakStmt() noexcept override;
    void accept(AstVisitor &) const override;

  private:
};
/** @brief 表示 continue 的源码位置。 */
class ContinueStmt final : public Stmt {
  public:
    ContinueStmt(SourceSpan, std::uint16_t);
    ~ContinueStmt() noexcept override;
    void accept(AstVisitor &) const override;

  private:
};
} // namespace myb
