#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
namespace myb {
/**
 * @brief 用索引描述 VM 活动帧和值区边界，不持有容器元素地址。
 *
 * 用户帧保存函数身份、调用续接和独立参数区；顶层没有函数身份。
 * 根扫描只读取已初始化局部槽及 sp 描述的有效操作数，预留槽不属于根。
 */
struct Frame {
    std::uint32_t unit_id{};                  ///< 所属 CompiledProgram 的单元编号。
    std::optional<std::uint32_t> function_id; ///< 用户函数编号，顶层帧为空。
    /**
     * @brief next_ip 指向下条指令，executing_ip 保留当前指令以供诊断。
     *
     * 局部区为 [base, operand_base)，预留操作数区为 [operand_base, region_end)。
     * sp 是有效操作数数量，对应 [operand_base, operand_base + sp)。
     */
    std::size_t next_ip{}, executing_ip{}, base{}, operand_base{}, region_end{}, sp{};
    /** @brief 预留调用者实参起点、调用指令位置和进入帧时的遍历深度。 */
    std::size_t caller_arg_base{}, caller_call_ip{}, iteration_base{};
};
} // namespace myb
