#pragma once
#include "builtins/registry.hpp"
#include "vm/call.hpp"
#include "vm/execution_index.hpp"
#include "vm/frame.hpp"
#include "vm/globals.hpp"
#include "vm/iteration.hpp"
#include "vm/value_store.hpp"
namespace myb {
/**
 * @brief 以统一值区和显式帧循环执行字节码，登记局部、操作数及遍历来源根。
 * VM拥有槽位、全局绑定、帧和遍历记录，借用会话服务；服务须覆盖VM寿命。
 * 登记期间VM地址稳定；数组活动计数在来源记录撤销前解除。
 * 用户调用不递归进入宿主解释函数，跨存储增长只保存索引。
 */
class VM final {
  public:
    /**
     * @brief 登记根并借用服务；可选trace接收错误时的真实函数链。
     * trace由调用方拥有，须覆盖execute调用；链记录可能因内存不足而省略。
     */
    VM(Heap &, RootRegistry &, HostIO &, EmergencyFailure &, const BuiltinRegistry &,
       std::vector<CallTraceEntry> *trace = nullptr) noexcept;
    VM(const VM &) = delete;
    VM &operator=(const VM &) = delete;
    /**
     * @brief 执行不可变程序，正常退出和异常均清除所有活动根。
     * @pre VM无活动帧，program及其常量根、结构元数据覆盖执行期间。
     * @throws Failure 动态检查、宿主IO、容量或内部状态失败。
     * native借用实参期间不增长值区或帧表，其结果由独立根交接。
     */
    void execute(const CompiledProgram &program);
    /** @brief 逆序解除全部遍历，再无分配清除值、全局绑定和帧，保留存储及根登记。 */
    void clear() noexcept;
    /** @brief 统一处理正常及错误出口；先解除数组计数和来源根，再清空活动帧与全局。 */
    void unwind_all() noexcept;
    ~VM() noexcept {
        clear();
    }

  private:
#ifdef MYB_TEST_HOOKS
    friend std::size_t TestHooks::active_iteration_count(const VM &) noexcept;
#endif
    /** @brief 标记已提交遍历来源，以及活动帧中已初始化局部与有效操作数。 */
    static void trace_values(void *, Marker &);
    /** @brief 在预留槽发布有根保护的值，不分配或回收。 */
    void push(Value);
    /** @brief 检查本单元索引，借用只读调用记录。 */
    const CallSite &call_site(const CodeUnit &, std::uint32_t);
    /**
     * @brief 查找构造方案的具名结构定义，未知类型与不可构造名称分别报告名称或类型错误。
     * @return 借用执行程序中地址稳定的 Schema，其寿命覆盖 VM 执行；不分配或触发 GC。
     */
    const StructSchema &construction_schema(const CodeUnit &, std::uint32_t, ErrorSite) const;
    /** @brief 按名称查找声明字段槽，缺失字段报告 Name；不分配或触发 GC。 */
    std::size_t field_slot(const StructSchema &, NameId, ErrorSite) const;
    /** @brief 按名称和数量预检，失败不求实参。 */
    void precheck(const CallSite &, ErrorSite) const;
    /**
     * @brief 准备用户参数和子区域后才发布帧，准备失败保留父帧和实参根。
     * 所有可能分配的操作在提交前完成，提交后清父实参不分配或回收。
     */
    void prepare_call(std::uint32_t call_site_id, ErrorSite);
    /** @brief 经结果根清理子帧并恢复父预留结果槽，不分配或回收。 */
    void return_to_caller(RootedValue &);
    /** @brief 按至少16项和1.5倍策略准备帧容量，失败不发布新帧。 */
    void ensure_frames(std::size_t required, ErrorSite);
    /**
     * @brief 核验栈顶来源并准备记录容量，成功后才接管来源根及数组计数。
     * @pre 当前帧和来源操作数完整可扫描；id 属于当前代码单元。
     * @throws Failure 来源类型或容量错误；宿主分配失败保留原操作数和已有记录。
     * 提交过程不触发 GC，记录在结束前负责保护来源对象。
     */
    void begin_iteration(IterationSiteId id);
    /**
     * @brief 读取原来源的当前元素，或分配一个码点 String，成功后初始化本轮局部。
     * @return 来源耗尽时返回 false，成功发布当前元素时返回 true。
     * @pre id 是当前帧最内层活动记录，循环变量槽已由词法出口清空。
     * 字符串分配可触发 GC，来源由记录保活，失败前不更新游标或绑定状态。
     * @throws Failure 资源错误或字节码及内部状态不一致。
     */
    bool next_iteration(IterationSiteId id);
    /**
     * @brief 结束当前帧最内层对应记录，恰好解除该记录拥有的一次计数与来源根。
     * @pre id 对应当前最内层记录；过程不分配、不回收或运行 MYB。
     */
    void end_iteration(IterationSiteId id) noexcept;
    /** @brief 按至少16项和1.5倍策略准备记录表，失败保持现有登记和操作数根。 */
    void ensure_iterations(std::size_t required, ErrorSite);
    /** @brief 逆序解除指定深度之后的记录，不分配或回收；来源须保持有效直到解除。 */
    void unwind_iterations(std::size_t base) noexcept;
    /** @brief 清理前保存已进入的函数链，记录失败不替换首错。 */
    void capture_trace() noexcept;
    /** @brief 无分配读取当前失败指令范围；尚未进入帧或位置无效时返回空值。 */
    std::optional<SourceSpan> current_location() const noexcept;
    [[noreturn]] void invalid();
    Heap &heap_;
    RootRegistry &roots_;
    HostIO &io_;
    EmergencyFailure &first_;
    const BuiltinRegistry &builtins_;
    ExecutionIndex execution_index_;
    ValueStore store_;
    Globals globals_;
    std::vector<Frame> frames_;
    std::vector<IterationRecord> iterations_;
    NativeRootSet root_set_;
    const CompiledProgram *program_{};
    std::vector<CallTraceEntry> *trace_;
};
} // namespace myb
