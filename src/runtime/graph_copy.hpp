#pragma once
#include "runtime/value.hpp"

namespace myb {
class Heap;
class RootRegistry;
class RootedValue;
/**
 * @brief 迭代复制可变对象图，并保留副本内部的共享、循环和结构类型。
 * @param[in] heap 创建副本的会话堆。
 * @param[in] roots 与 heap 相同会话的根表。
 * @param[in] source 已有根保护的输入；基本值和不可变 String 保持原值。
 * @param[in,out] out 已登记于 roots 的结果根；全部复制成功后才替换原值。
 * @pre source 的对象属于 heap，输入根和结构定义覆盖整个调用。
 * 分配可触发 GC；工作区登记源对象、目标对象与尚未完成的副本，退出时撤销登记。
 * @throws HostFailure 堆分配、容量或根归属检查失败。
 * @throws std::bad_alloc 工作区的宿主分配失败，由会话边界转换为资源故障。
 * @throws std::length_error 工作区容量耗尽，由会话边界转换为资源故障。
 */
void deep_copy(Heap &heap, RootRegistry &roots, Value source, RootedValue &out);
} // namespace myb
