#pragma once
#include "bytecode/program.hpp"
namespace myb {
/**
 * @brief 核验完整控制流、操作数栈和副表协议，成功后写入入口可达最大深度。
 * @param[in,out] unit 待核验单元，成功时更新 max_operand。
 * @param[in,out] first 失败时记录首个错误。
 * @throws Failure 指令、相关索引、栈深或终止结构无效，或最大栈深超限。
 *
 * 核验各连通片段，调用预检与调用配对，字段检查顺序和遍历栈。
 * 不可达语句使用生成期入口信息，其容量不计入入口可达最大深度。
 * 不读取运行值，不提前报告语言运行错误。
 */
void verify(CodeUnit &, EmergencyFailure &);
/** @brief 核验程序级名称、函数、结构编号和各单元；不执行任何 MYB 操作。 */
void verify(CompiledProgram &, EmergencyFailure &);
} // namespace myb
