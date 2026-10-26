#pragma once
#include "runtime/value.hpp"

namespace myb {
class Marker;
class RootLink;
/**
 * @brief 从稳定的宿主上下文枚举当前受管值的根回调。
 * 回调通过 Marker 标记有效值；枚举期间保持集合和根链稳定，不进入 MYB GC。
 * 标记工作表的宿主分配失败可传播给 Heap，交由回收失败清理处理。
 */
using RootTrace = void (*)(void *, Marker &);
/**
 * @brief 以侵入式链表登记单会话根提供者，不扫描 C++ 栈或未登记容器。
 * @pre 本对象先于所有 RootLink 创建，并在全部链接注销后销毁。
 * 登记和注销不分配内存、不触发 GC；trace 期间不得修改根链。
 */
class RootRegistry final {
  public:
    RootRegistry() noexcept = default;
    ~RootRegistry() noexcept;
    RootRegistry(const RootRegistry &) = delete;
    RootRegistry &operator=(const RootRegistry &) = delete;
    /** @brief 调用所有已登记提供者，向本轮 Marker 枚举当前根。 */
    void trace(Marker &);

  private:
    friend class RootLink;
    RootLink *head_{};
};
/**
 * @brief 把一个地址稳定的上下文接入根链，析构时无分配注销。
 * @pre 上下文和回调在链接存续期间有效；Registry 的寿命覆盖本链接。
 */
class RootLink final {
  public:
    /** @brief 登记非空上下文及回调，注册过程不触发 GC。 */
    RootLink(RootRegistry &, void *, RootTrace) noexcept;
    ~RootLink() noexcept;
    RootLink(const RootLink &) = delete;
    RootLink &operator=(const RootLink &) = delete;
    /** @brief 注销链接；重复调用保持已注销状态，不触发 GC。 */
    void detach() noexcept;
    /** @brief 判断链接是否仍登记在指定 Registry 中。 */
    bool belongs_to(const RootRegistry &) const noexcept;

  private:
    friend class RootRegistry;
    RootRegistry *registry_{};
    RootLink *previous_{};
    RootLink *next_{};
    void *context_;
    RootTrace trace_;
};
/**
 * @brief 在本对象的稳定槽内保存并登记单个临时根。
 * 本对象不可复制或移动；根保护持续到 detach 或析构，set 可无分配替换槽值。
 * get 取得的引用借用本槽；复制出的 Value 仍须由根保持对象可达。
 */
class RootedValue final {
  public:
    /** @brief 登记初始为 null 的根槽，Registry 的寿命须覆盖本对象。 */
    explicit RootedValue(RootRegistry &) noexcept;
    /**
     * @brief 复制初值并立即登记根槽，期间不触发 GC。
     * @pre 初值的对象引用在复制前仍有效；不得在旧根到本根的交接间触发回收。
     */
    RootedValue(RootRegistry &, Value) noexcept;
    ~RootedValue() noexcept = default;
    RootedValue(const RootedValue &) = delete;
    RootedValue &operator=(const RootedValue &) = delete;
    const Value &get() const noexcept {
        return slot_;
    }
    /**
     * @brief 无分配替换槽值；新值受本根保护，旧值失去本槽的保护。
     * @param[in] value 仍有效的值表示，复制过程不触发 GC。
     * @pre 本槽用于保活时须仍登记在所属 Registry 中。
     */
    void set(Value value) noexcept {
        slot_ = value;
#ifndef NDEBUG
        written_ = true;
#endif
    }
#ifndef NDEBUG
    /** @brief 开始检查一次 native 输出交接，仅重置调试标记，原值继续由根保活。 */
    void begin_output() noexcept {
        written_ = false;
    }
    /** @brief 查询本次 native 输出检查开始后是否调用过 set。 */
    bool was_written() const noexcept {
        return written_;
    }
#endif
    /** @brief 幂等撤销根登记，保留槽值；后续回收不再通过本槽保活对象。 */
    void detach() noexcept {
        link_.detach();
    }
    bool belongs_to(const RootRegistry &registry) const noexcept {
        return link_.belongs_to(registry);
    }

  private:
    static void trace_slot(void *, Marker &);
    Value slot_{NullValue{}};
    RootLink link_;
#ifndef NDEBUG
    bool written_{};
#endif
};
/**
 * @brief 为宿主多值集合登记一个稳定上下文，回调每次枚举容器的当前内容。
 * 集合内部容器可扩容；上下文地址须保持稳定，不能登记可增长容器元素的地址。
 * 集合修改期间不触发 GC；调用可回收接口前须恢复完整可枚举状态。
 */
class NativeRootSet final {
  public:
    /**
     * @brief 无分配登记集合回调；链接存续期间借用 Registry 与上下文。
     * @param[in] registry 寿命覆盖本集合的会话根表。
     * @param[in] context 非空、地址稳定且寿命覆盖登记期间的集合上下文。
     * @param[in] trace 枚举上下文内所有有效受管值的非空回调。
     */
    NativeRootSet(RootRegistry &registry, void *context, RootTrace trace) noexcept
        : link_(registry, context, trace) {}
    NativeRootSet(const NativeRootSet &) = delete;
    NativeRootSet &operator=(const NativeRootSet &) = delete;
    /** @brief 幂等撤销集合根登记，不销毁或清空上下文。 */
    void detach() noexcept {
        link_.detach();
    }

  private:
    RootLink link_;
};
} // namespace myb
