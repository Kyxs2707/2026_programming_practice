#pragma once
#include "semantic/checked_ast.hpp"
#include <unordered_map>
#include <vector>
namespace myb {
/** @brief 表示一个词法子树所占的连续局部槽区间。 */
struct LocalRange {
    std::uint32_t first{}, count{};
};
/**
 * @brief 为代码单元预分配槽位；编号不建立名称绑定，节点键借用输入 AST。
 *
 * 参数从零编号，普通局部编号不复用；ranges 的父区间包含所有子区间。
 * 调用方须使 CheckedAst 的树覆盖布局读取期，生成后的程序不保存这些键。
 */
struct UnitLayout {
    std::uint32_t parameter_count{}, local_count{};
    std::unordered_map<const Node *, std::uint32_t> slots;
    std::unordered_map<const Node *, LocalRange> ranges;
};
/** @brief 拥有顶层和各函数的槽位布局，函数顺序与源码一致。 */
struct BindingLayout {
    std::vector<UnitLayout> units;
    /** @brief 仅分配槽位和子树区间，不解析或激活普通变量名称。 */
    static BindingLayout build(const CheckedAst &);
};
} // namespace myb
