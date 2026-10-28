#pragma once
#include "builtins/registry.hpp"
#include "bytecode/program.hpp"
#include <optional>
#include <unordered_map>
#include <vector>

namespace myb {
/** @brief 区分未知名称、用户函数和本次登记表中的内置目标。 */
enum class CallTargetKind : std::uint8_t { Missing, User, Builtin };
/** @brief 保存只读调用目标；内置指针借用执行期间保持不变的实际登记表。 */
struct ResolvedCallTarget {
    CallTargetKind kind{CallTargetKind::Missing};
    std::uint32_t function_index{};
    const BuiltinDescriptor *builtin{};
};
/** @brief 保存构造的静态匹配结果，错误状态只在对应操作码执行时报告。 */
enum class ConstructionStatus : std::uint8_t {
    UnknownType,
    NotConstructible,
    FieldSetMismatch,
    Ready
};
/** @brief 按构造源码字段顺序保存声明槽编号，不持有字段动态值或受管对象。 */
struct PreparedConstruction {
    ConstructionStatus status{ConstructionStatus::UnknownType};
    std::optional<SchemaId> schema;
    std::vector<std::uint32_t> slots;
};
/**
 * @brief 在发布首帧前建立一次执行所需的名称、字段和构造索引。
 *
 * 拥有辅助文本和编号；借用的 Program、Schema、实际内置描述及名称须覆盖查询期，
 * 期间保持不变。索引不持有 Value，不新增 GC 根，所有查询均不分配或回收。
 */
class ExecutionIndex final {
  public:
    /**
     * @brief 检查原始元数据并准备临时索引，全部成功后一次发布。
     * @param[in] program 原始程序，不要求 name_ids 已填充。
     * @param[in] builtins 本次 VM 使用的实际登记表。
     * @param[in,out] first 保存宿主容量、分配或元数据失败。
     * @throws Failure 编号越界、无效元数据或宿主容量不足。
     * @throws std::bad_alloc 宿主分配失败；保留首错，不发布部分结果。
     * @throws std::length_error 宿主容器容量不足；保留资源首错，不发布部分结果。
     *
     * 只记录动态无效目标和构造状态，不执行 MYB、不触发 GC。
     * 失败时旧索引仍完整；VM 统一失败边界负责清理，成功后旧查询借用失效。
     */
    void prepare(const CompiledProgram &program, const BuiltinRegistry &builtins,
                 EmergencyFailure &first);
    /**
     * @brief 返回目标记录；Missing 由 PRECALL 报告，越界报告 Internal。
     * @return 借用本索引中的记录，成功重新准备或 clear 后失效。
     */
    const ResolvedCallTarget &call(NameId name, ErrorSite site) const;
    /**
     * @brief 返回借用的 Schema；未知/不可构造名称分别报告运行时 Name/Type。
     * @return 由程序拥有的定义，程序及定义保持有效期间可借用。
     */
    const StructSchema &schema(NameId name, ErrorSite site) const;
    /**
     * @brief 按实际 Schema 查询字段槽；不属于本程序的定义报告 Internal，缺失字段报告 Name。
     * @return 当前定义中对应字段的声明槽编号。
     */
    std::size_t field_slot(const StructSchema &schema, NameId name, ErrorSite site) const;
    /**
     * @brief 返回本单元的准备记录，动态无效状态由 CHECK_STRUCT 报告；越界报告 Internal。
     * @return 借用本索引中的记录，成功重新准备或 clear 后失效。
     */
    const PreparedConstruction &construction(std::uint32_t unit, std::uint32_t plan,
                                             ErrorSite site) const;
    /** @brief 幂等地释放辅助记录并撤销借用，不分配、不回收；旧查询引用随后失效。 */
    void clear() noexcept;

  private:
    struct NameResolution {
        ResolvedCallTarget call;
        std::optional<SchemaId> schema;
        bool nonconstructible{};
    };
    /** @brief 比较字段文本；内部测试在实际比较处统计工作量。 */
    struct FieldEqual {
        bool operator()(const std::string &left, const std::string &right) const noexcept;
    };
    using Fields =
        std::unordered_map<std::string, std::uint32_t, std::hash<std::string>, FieldEqual>;
    const NameResolution &name(NameId id, ErrorSite site) const;
    void swap(ExecutionIndex &) noexcept;
    const CompiledProgram *program_{};
    std::vector<NameResolution> names_;
    std::vector<Fields> fields_;
    std::vector<std::vector<PreparedConstruction>> constructions_;
};
} // namespace myb
