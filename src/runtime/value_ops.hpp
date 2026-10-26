#pragma once
#include "diagnostic/failure.hpp"
#include "runtime/operators.hpp"
#include "runtime/schema.hpp"
#include "runtime/value.hpp"
namespace myb {
class Heap;
class RootRegistry;
class RootedValue;
/**
 * @brief 集中实现动态算术、严格 Bool 取反和数值、文本及对象身份比较。
 * 实例借用同一会话的 Heap 与根表；二者寿命须覆盖本对象。
 * 静态标量接口不分配受管对象；实例二元接口可在字符串拼接时触发 GC。
 */
class ValueOps {
  public:
    /**
     * @brief 借用堆和该堆所属根表，供字符串拼接建立临时根并发布结果。
     * @pre heap 与 roots 属于同一会话，且在本对象存续期间保持有效。
     */
    ValueOps(Heap &heap, RootRegistry &roots) noexcept : heap_(heap), roots_(roots) {}
    /**
     * @brief 对合法数值应用正负号，或对 Bool 应用逻辑取反。
     * @return 运算后的标量值；正号保留原数值表示。
     * @throws MybRuntimeFailure 操作数类型错误，或对 Int64 最小值取负。
     * @throws HostFailure 运算符编号无效。
     */
    static Value unary(UnaryOp, Value, ErrorSite);
    /**
     * @brief 对已求值的左右操作数执行算术，并在结果返回前完成类型和范围检查。
     * 整数加减乘保留 Int64；除法及混合算术提升为 Float64。
     * 整除和余数只接受 Int64，商向下取整，非零余数与除数同号。
     * @return 合法的 Int64 或有限 Float64；允许浮点下溢及带符号零。
     * @throws MybRuntimeFailure 类型错误、除零、整数溢出或浮点结果非有限。
     * @throws HostFailure 运算符编号无效。
     */
    static Value binary(BinaryOp, Value, Value, ErrorSite);
    /**
     * @brief 执行数值算术或两个 String 的拼接，成功后才替换输出槽。
     * @param[in] lhs 已完成求值且仍有效的左操作数。
     * @param[in] rhs 已完成求值且仍有效的右操作数。
     * @param[in,out] out 同会话已登记的根槽，可与任一输入共用原值。
     * 入口先为输入登记临时根；Builder 增长及发布可触发 GC，输出接管后撤销临时根。
     * 失败时保留 out 原值；成功后结果由 out 保活，借用至撤根后回收或 Heap 清空。
     * @throws MybRuntimeFailure 类型错误、除零、整数溢出或非有限浮点结果。
     * @throws HostFailure 无效运算符、输出根归属错误、容量或宿主分配失败。
     */
    void binary(BinaryOp, Value lhs, Value rhs, ErrorSite, RootedValue &out);
    /**
     * @brief 按 MYB 规则返回 Bool 比较结果，不分配受管对象、不触发 GC。
     * 混合数值比较保留 Int64 精度；String 按无规范化的 Unicode 码点序比较。
     * Array 与结构只比较身份；跨类别仅数值组合可相等，其他组合不相等。
     * @pre 对象引用仍有效；double 为有限值。
     * @return 相等判断或有序比较的 Bool 值。
     * @throws MybRuntimeFailure 对数值组合及两个 String 之外的值执行有序比较。
     * @throws HostFailure 比较运算符编号无效。
     */
    static Value compare(CompareOp, Value, Value, ErrorSite);
    /**
     * @brief 检查结构字段的类别、可空约束和具名结构身份，按需提升 Int64 至 Float64。
     * @return 已通过约束的值；除数值提升外保留原表示和共享关系。
     * @pre 引用对象与其 Schema 仍有效；接口不分配受管对象或触发 GC。
     * @throws MybRuntimeFailure 字段值不符合约束。
     */
    static Value check_field_value(const FieldConstraint &, Value, ErrorSite);

  private:
    Heap &heap_;
    RootRegistry &roots_;
};
} // namespace myb
