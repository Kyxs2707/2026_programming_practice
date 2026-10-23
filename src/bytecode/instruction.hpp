#pragma once
#include "runtime/operators.hpp"
#include <cstdint>
namespace myb {
/**
 * @brief 集中定义完整编译器和核验器使用的固定操作码及操作数含义。
 * 常量、副表、槽位和跳转属于当前单元；名称编号属于整个程序。
 */
enum class Op : std::uint16_t {
    CONST,        ///< 将当前 CodeUnit 的常量压入操作数区。
    DUP,          ///< a=b=0；复制栈顶，1→2。
    DROP,         ///< 清空并移除栈顶操作数，使其退出 VM 根扫描范围。
    DECL_LOCAL,   ///< a=槽位；建立未初始化绑定，0→0。
    DECL_GLOBAL,  ///< a=名称；建立未初始化全局，0→0。
    LOAD_LOCAL,   ///< a=槽位；读取局部，0→1。
    LOAD_GLOBAL,  ///< a=名称；读取全局，0→1。
    STORE_LOCAL,  ///< a=槽位；写入已有局部，1→0。
    STORE_GLOBAL, ///< a=名称；写入已有全局，1→0。
    CLEAR_LOCALS, ///< a=首槽、b=数量；撤销局部区间，0→0。
    UNARY,        ///< 将栈顶值替换为一元运算结果。
    BINARY,       ///< 将栈顶两个值替换为二元运算结果。
    COMPARE,      ///< a=CompareOp；比较两个值，2→1。
    CHECK_BOOL,   ///< a=b=0；严格检查栈顶 Bool，1→1。
    JUMP,         ///< a=绝对指令号；无条件转移，0→0。
    JUMP_FALSE,   ///< a=绝对指令号；消费严格 Bool 条件，1→0。
    JUMP_TRUE,    ///< a=绝对指令号；消费严格 Bool 条件，1→0。
    PRECALL,      ///< a=调用点号；预检用户函数或内置的目标及数量，0→0。
    CALL,         ///< a=调用点号；消费 k 个实参并接收一个结果，k→1。
    RETURN,       ///< a=b=0；返回一个结果并统一清理当前帧。
    HALT,         ///< 在空操作数栈上结束顶层执行。
    BUILD_ARRAY,  ///< a=元素数 k；创建新数组，k→1。
    CHECK_STRUCT, ///< a=构造方案号；预检类型和字段集合，0→0。
    CHECK_FIELD,  ///< a=构造方案、b=源码字段序号；立即检查栈顶，1→1。
    BUILD_STRUCT, ///< a=构造方案号；创建新结构，k→1。
    GET_FIELD,    ///< a=字段名称号；读取字段，1→1。
    GET_INDEX,    ///< a=b=0；读取接收对象和索引，2→1。
    GET_SLICE,    ///< a的位0/1表示起点/终点存在；1+m→1。
    SET_FIELD,    ///< a=字段名称；右值在下、接收对象在上，2→0。
    SET_INDEX,    ///< a=b=0；依次消费右值、接收对象和索引，3→0。
    ITER_BEGIN,   ///< a=遍历站点号；登记来源，1→0。
    ITER_NEXT,    ///< a=站点号、b=耗尽跳转目标；取得下一元素，0→0。
    ITER_END      ///< a=站点号；恰好解除一次遍历登记，0→0。
};
/**
 * @brief 保存操作码及其两个无符号操作数。
 *
 * 未使用的 a/b 必须为零，核验器会拒绝非零保留字段。
 */
struct Instruction {
    Op op;
    std::uint32_t a{}, b{};
};
} // namespace myb
