#pragma once
#include "bytecode/instruction.hpp"
#include "runtime/heap.hpp"
#include "source/span.hpp"
#include <string>
#include <unordered_map>
#include <vector>
namespace myb {
/** @brief 程序名称池中的索引，不借用名称字符串的存储地址。 */
using NameId = std::uint32_t;
/** @brief 保存一次命名调用的目标名称编号和固定实参数量。 */
struct CallSite {
    NameId target;       ///< 所属 CompiledProgram 的名称池索引。
    std::uint32_t arity; ///< PRECALL 和 CALL 共用的实参数量。
};
/** @brief 拥有用户函数的名称、固定参数数量、代码编号和声明位置。 */
struct FunctionInfo {
    NameId name;
    std::uint32_t arity, unit;
    SourceSpan span;
};
/** @brief 按源码顺序拥有结构构造字段名称，保留重复项供运行时预检。 */
struct ConstructionPlan {
    NameId type;
    std::vector<NameId> fields;
};
/** @brief 保存每轮遍历变量使用的局部槽编号。 */
struct IterationSite {
    std::uint32_t slot;
};
/**
 * @brief 拥有一个代码单元的指令、常量句柄、源码范围和调用点表。
 *
 * 常量中的受管对象由 Heap 拥有；单独的 CodeUnit 不登记 GC 根。
 * 放入 CompiledProgram 后，其常量由该程序的根回调保活。
 * 各 vector 增长可使其元素引用失效，跨增长操作应保留索引。
 */
struct CodeUnit {
    std::vector<Instruction> instructions;           ///< 当前单元的线性指令序列。
    std::vector<Value> constants;                    ///< CONST 使用的常量句柄。
    std::vector<SourceSpan> locations;               ///< 与 instructions 逐项对应的源码范围。
    std::vector<CallSite> calls;                     ///< PRECALL 和 CALL 使用的本单元调用点。
    std::vector<ConstructionPlan> constructions;     ///< 本单元构造方案。
    std::vector<IterationSite> iterations;           ///< 本单元遍历站点。
    std::vector<std::uint32_t> statement_boundaries; ///< 不可达语句以深度零核验的入口。
    std::vector<std::vector<std::uint32_t>> boundary_iterations; ///< 语句入口的词法遍历栈。
    std::uint32_t parameter_count{};                             ///< 局部区开头已初始化的参数数。
    std::uint32_t local_count{}, max_operand{}; ///< 局部槽数量及核验所得最大操作数深度。
};
/**
 * @brief 拥有编译产物，并为所有代码单元的受管常量登记 GC 根。
 *
 * 名称和代码表不借用 AST；受管对象仍由 Heap 拥有。
 * 根回调借用本对象地址，因此对象在登记期间保持地址稳定。
 * RootRegistry 必须存活至本对象析构或显式撤销根登记。
 */
class CompiledProgram final {
  public:
    /** @brief 在 roots 中登记常量根集合，初始产物为空。 */
    explicit CompiledProgram(RootRegistry &roots) noexcept : roots_(roots, this, trace_constants) {}
    CompiledProgram(const CompiledProgram &) = delete;
    CompiledProgram &operator=(const CompiledProgram &) = delete;
    /**
     * @brief 幂等地撤销常量根登记，不销毁产物或 Heap 对象。
     * @pre 撤销后仍需使用的受管常量已有其他根保护，或会话进入不再回收的清理阶段。
     */
    void detach_roots() noexcept {
        roots_.detach();
    }
    /**
     * @brief 查找名称编号，首次出现时复制名称并分配新编号。
     * @param[in] name 仅在调用期间借用的名称内容。
     * @param[in,out] first 编号容量不足时记录首个失败。
     * @return 在本程序内保持不变的名称编号。
     * @throws Failure 名称数量超出 NameId 可表示范围。
     *
     * 宿主容器分配可能失败，但此操作不触发 MYB GC；容器增长可使旧名称引用失效。
     */
    NameId intern_name(std::string_view, EmergencyFailure &);
    std::vector<std::string> names;                   ///< 按 NameId 索引、独立拥有内容的名称池。
    std::unordered_map<std::string, NameId> name_ids; ///< 名称内容到名称编号的查找表。
    std::vector<CodeUnit> units;         ///< 本程序拥有的代码单元，根回调扫描所有单元常量。
    std::vector<FunctionInfo> functions; ///< 按源码顺序保存用户函数。
    std::vector<std::unique_ptr<StructSchema>> schemas; ///< 独占地址稳定的结构定义。
    std::uint32_t top_unit{};                           ///< VM 开始执行的顶层单元编号。

  private:
    /** @brief 遍历所有单元常量并标记其可达受管对象，不增加容器容量。 */
    static void trace_constants(void *, Marker &);
    NativeRootSet roots_;
};
} // namespace myb
