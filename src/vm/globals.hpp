#pragma once
#include "bytecode/program.hpp"
#include <unordered_map>
namespace myb {
/** @brief 保存一个已执行全局声明的状态；占位 null 与未初始化状态分开。 */
struct GlobalBinding {
    Value value{NullValue{}};
    BindingState state{BindingState::Uninitialized};
};
/**
 * @brief 拥有本次执行的全局绑定，并为已初始化值登记稳定根集合。
 * roots 须覆盖本对象寿命；登记期间本对象地址稳定。名称编号属于执行程序。
 * 查找和赋值不插入缺失名称；声明插入可能分配宿主内存，不触发 MYB GC。
 */
class Globals final {
  public:
    explicit Globals(RootRegistry &roots) noexcept : roots_(roots, this, trace) {}
    /** @brief 建立未初始化绑定；同名声明重复执行属于内部状态错误。 */
    void declare(NameId name, ErrorSite site);
    /** @brief 读取已初始化值，分别报告缺失名称和未初始化错误。 */
    Value load(NameId name, ErrorSite site) const;
    /** @brief 写入已有绑定并将其初始化，缺失名称报告 Name。 */
    void store(NameId name, Value value, ErrorSite site);
    /** @brief 清除所有绑定和对象引用，不触发 GC。 */
    void clear() noexcept {
        bindings_.clear();
    }

  private:
    static void trace(void *, Marker &);
    std::unordered_map<NameId, GlobalBinding> bindings_;
    NativeRootSet roots_;
};
} // namespace myb
