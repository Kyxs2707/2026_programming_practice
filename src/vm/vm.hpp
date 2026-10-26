#pragma once
#include "builtins/registry.hpp"
#include "vm/call.hpp"
#include "vm/frame.hpp"
#include "vm/globals.hpp"
#include "vm/value_store.hpp"
namespace myb {
/**
 * @brief 以统一值区和显式帧循环执行字节码，登记活动局部和操作数根。
 * VM拥有槽位、全局绑定和帧，借用会话服务；服务须覆盖VM寿命，登记期间VM地址稳定。
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
    /** @brief 无分配清除值、全局绑定和活动帧，保留预留存储及根登记。 */
    void clear() noexcept;
    ~VM() noexcept {
        clear();
    }

  private:
    /** @brief 仅标记活动帧中已初始化局部与有效操作数。 */
    static void trace_values(void *, Marker &);
    /** @brief 在预留槽发布有根保护的值，不分配或回收。 */
    void push(Value);
    /** @brief 检查本单元索引，借用只读调用记录。 */
    const CallSite &call_site(const CodeUnit &, std::uint32_t);
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
    /** @brief 清理前保存已进入的函数链，记录失败不替换首错。 */
    void capture_trace() noexcept;
    [[noreturn]] void invalid();
    Heap &heap_;
    RootRegistry &roots_;
    HostIO &io_;
    EmergencyFailure &first_;
    const BuiltinRegistry &builtins_;
    ValueStore store_;
    Globals globals_;
    std::vector<Frame> frames_;
    NativeRootSet root_set_;
    const CompiledProgram *program_{};
    std::vector<CallTraceEntry> *trace_;
};
} // namespace myb
