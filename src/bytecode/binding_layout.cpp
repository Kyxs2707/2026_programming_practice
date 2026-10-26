#include "bytecode/binding_layout.hpp"
#include "diagnostic/failure.hpp"
#include <limits>
namespace myb {
namespace {
/** @brief 分配一个新槽位，容量不足时报告受控宿主故障。 */
std::uint32_t slot(UnitLayout &unit) {
    if (unit.local_count == std::numeric_limits<std::uint32_t>::max())
        throw HostFailure({FailureDomain::Resource, ReasonCode::Capacity}, "too many local slots");
    return unit.local_count++;
}
/** @brief 预遍历语句子树，为局部声明和循环变量分配唯一槽位。 */
void layout(const Stmt &stmt, UnitLayout &unit) {
    const auto first = unit.local_count;
    if (auto n = dynamic_cast<const LetStmt *>(&stmt))
        unit.slots.emplace(n, slot(unit));
    else if (auto n = dynamic_cast<const BlockStmt *>(&stmt)) {
        for (const auto &child : n->statements())
            layout(child, unit);
    } else if (auto n = dynamic_cast<const IfStmt *>(&stmt)) {
        layout(n->then_body(), unit);
        if (n->else_body())
            layout(*n->else_body(), unit);
    } else if (auto n = dynamic_cast<const WhileStmt *>(&stmt))
        layout(n->body(), unit);
    else if (auto n = dynamic_cast<const ForStmt *>(&stmt)) {
        unit.slots.emplace(n, slot(unit));
        layout(n->body(), unit);
    }
    unit.ranges.emplace(&stmt, LocalRange{first, unit.local_count - first});
}
} // namespace
BindingLayout BindingLayout::build(const CheckedAst &checked) {
    BindingLayout result;
    result.units.emplace_back();
    for (const auto &item : checked.program->items()) {
        if (auto function = dynamic_cast<const FunctionDecl *>(&item)) {
            UnitLayout fn;
            for (const auto &parameter : function->parameters()) {
                (void)parameter;
                slot(fn);
            }
            fn.parameter_count = fn.local_count;
            layout(function->body(), fn);
            result.units.push_back(std::move(fn));
        } else if (auto statement = dynamic_cast<const Stmt *>(&item)) {
            // 文件直接 let 使用全局表；只有嵌套语句树需要局部槽。
            if (!dynamic_cast<const LetStmt *>(statement))
                layout(*statement, result.units[0]);
        }
    }
    return result;
}
} // namespace myb
