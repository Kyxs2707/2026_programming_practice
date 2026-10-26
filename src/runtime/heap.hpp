#pragma once
#include "diagnostic/failure.hpp"
#include "runtime/object.hpp"
#include "runtime/roots.hpp"
#include <memory>
#include <vector>
#ifdef MYB_TEST_HOOKS
#include "test_hooks.hpp"
#endif

namespace myb {
class TextBuilder;
class Heap;
/**
 * @brief 在单次 Heap 回收中标记对象并把首次发现的对象加入显式工作表。
 * 实例由 Heap 创建并借用该 Heap；标记可以扩容宿主工作表，不进入 MYB GC。
 */
class Marker final {
  public:
    /**
     * @brief 对对象值标记并排队，对标量值不作处理。
     * @pre 引用对象属于本轮 Heap 且仍有效；仅在所属回收期间调用。
     * @throws HostFailure 对象引用为空或工作表容量耗尽。
     * @throws std::bad_alloc 工作表的宿主分配失败，由 Heap::collect 转换为资源故障。
     */
    void mark(Value);

  private:
    friend class Heap;
    explicit Marker(Heap &heap) noexcept : heap_(heap) {}
    /** @brief 首次遇到对象时设置标记并入队，以去重共享边并终止循环图遍历。 */
    void mark_object(HeapObject *);
    Heap &heap_;
};
/**
 * @brief 独占受管对象，以非移动标记清除回收并按对象和负载容量统计增长压力。
 * 分配和负载扩容入口可触发 GC；所有跨调用仍使用的对象值须先由同会话根保活。
 * 新对象登记后立即写入输出根，构造、登记和交接期间不再次触发 GC。
 * RootRegistry、首错记录和被对象借用的 Schema 须覆盖本堆所需寿命；
 * 所有 TextBuilder 须先于本堆销毁。统计量用于回收阈值，不表示进程内存配额。
 */
class Heap final {
  public:
    /** @brief 借用会话根表与首错记录，建立初始回收阈值为 1 MiB 的空堆。 */
    Heap(RootRegistry &, EmergencyFailure &) noexcept;
    ~Heap() noexcept;
    Heap(const Heap &) = delete;
    Heap &operator=(const Heap &) = delete;
    /**
     * @brief 创建空数组并预留容量，成功后用新数组替换输出根槽。
     * @param[in] initial_capacity 至少预留的元素容量，不改变数组长度。
     * @param[in,out] out 同一 Registry 中已登记的根；原值保护持续到成功替换。
     * @throws HostFailure 容量、分配或根归属检查失败；可能先触发 GC。
     */
    void make_array(std::size_t initial_capacity, RootedValue &out);
    /**
     * @brief 借用结构定义创建字段全为 null 的可扫描对象，并写入输出根。
     * 字段槽数取定义中的字段数；业务字段值及约束检查由调用方继续完成。
     * @param[in,out] out 同一 Registry 中已登记的根，成功前保持原值。
     * @pre 结构定义地址稳定，且寿命覆盖新对象；跨调用使用的对象值已有根。
     * @throws HostFailure 容量、分配或根归属检查失败；可能先触发 GC。
     */
    void make_struct(const StructSchema &, RootedValue &out);
    /**
     * @brief 验证 UTF-8 后接管同一 Heap 的 Builder 缓冲，转移记账并发布字符串。
     * 成功后 Builder 已被消费，只能销毁；输出根接管前原槽值持续受保护。
     * @param[in,out] out 同一 Registry 中已登记的根，成功时保存字符串引用。
     * @pre Builder 尚未消费；跨调用使用的对象值已有根。
     * @throws HostFailure Builder 归属或状态、UTF-8、根归属、容量或分配检查失败。
     * 验证完成后的对象分配入口可能触发 GC。
     */
    void make_string(TextBuilder &&, RootedValue &out);
    /**
     * @brief 为数组预留容量，按实际容量更新记账，保持元素数量和值不变。
     * @param[in] required_capacity 至少需要的元素容量；已有容量足够时直接返回。
     * 容量不足时按几何级数增长，所需容量与建议增长量均受元素及字节上界约束。
     * @pre 数组属于本堆，数组和跨调用仍使用的待写值均已有根。
     * 增长可能触发 GC；扩容后须按索引重取元素，既有元素指针和引用可能失效。
     * @throws HostFailure 容量或分配失败；失败后由会话终止路径清理。
     */
    void reserve_array(ArrayRef, std::size_t required_capacity);
    /**
     * @brief 从完整根集迭代标记并清除不可达对象，保留存活对象地址。
     * @pre 所有根提供者处于完整可枚举状态；回调不修改根链、不重入回收。
     * 标记或存活量核算失败时清除本轮标记并传播异常，不清除任何对象。
     * @throws HostFailure 重入、容量或宿主分配失败。
     */
    void collect();
    /**
     * @brief 无回收、无分配地销毁全部对象并重置统计和回收阈值。
     * @pre 当前未回收；持有本堆对象的根已撤销，Builder 已析构或完成缓冲转移。
     * 调用后本堆的所有对象借用失效，Schema 此时仍须有效。
     */
    void clear() noexcept;

  private:
    friend class Marker;
    friend class TextBuilder;
#ifdef MYB_TEST_HOOKS
    friend TestHooks::HeapCounts TestHooks::heap_counts(const Heap &) noexcept;
#endif
    /** @brief 检查容量相加，溢出时报告资源容量故障。 */
    std::size_t add(std::size_t, std::size_t);
    /** @brief 检查元素数量与单项字节数相乘，溢出时报告资源容量故障。 */
    std::size_t multiply(std::size_t, std::size_t);
    /** @brief 同时检查宿主容器上限及 MYB Int64 可表达的元素数量上限。 */
    void check_elements(std::size_t, std::size_t max_size);
    /** @brief 验证输出根仍登记在本堆的会话根表中，失败时报告内部故障。 */
    void check_output(const RootedValue &);
    /**
     * @brief 检查预计增长总量，必要时回收一次，并记录大请求的阈值更新条件。
     * @pre 所有存续对象值已有根；后续发布段直到 after_growth 不再进入 GC。
     */
    void before_growth(std::size_t);
    /** @brief 成功增长后仅为仍属大请求的操作调整阈值，避免普通小增长追高阈值。 */
    void after_growth() noexcept;
    /** @brief 以总存活容量的两倍更新阈值，在 size_t 上界饱和并保持至少 1 MiB。 */
    void update_threshold() noexcept;
    /** @brief 计算对象头与实际负载容量之和，容量算术溢出时报告资源故障。 */
    std::size_t object_bytes(const HeapObject &);
    /** @brief 检查并替换 Builder 容量记账，本操作不触发 GC。 */
    void update_builder(std::size_t old_capacity, std::size_t new_capacity);
    [[noreturn]] void allocation_failed();
    [[noreturn]] void capacity_failed();
    [[noreturn]] void invariant_failed(const char *);
    RootRegistry &roots_;
    EmergencyFailure &failure_;
    std::vector<std::unique_ptr<HeapObject>> objects_; ///< 对象的唯一所有者，本登记表本身不是根。
    std::vector<HeapObject *> mark_worklist_; ///< 本轮待扫描对象，不递归使用 C++ 调用栈。
    std::size_t managed_bytes_{}; ///< 受管对象头和实际负载容量之和。
    std::size_t builder_bytes_{}; ///< 尚未交给字符串对象的活动 Builder 容量。
    static constexpr std::size_t minimum_gc = 1024 * 1024;
    std::size_t next_gc_{minimum_gc};
    bool collecting_{};
    bool large_request_{};
};
} // namespace myb
