#pragma once
#include "builtins/registry.hpp"
#include "bytecode/program.hpp"
#include "cli/options.hpp"
#include "source/source.hpp"
#include "vm/call.hpp"
namespace myb {
/**
 * @brief 拥有单次命令的源码、编译产物、根登记与受管堆，统一处理执行和诊断。
 *
 * 构造参数均为借用，须覆盖会话寿命。编译接口也供嵌入前端的宿主使用。
 * 析构先撤销程序常量根并清空堆，再销毁仍供对象借用的元数据和根登记表。
 */
class Session final {
  public:
    /** @brief 绑定会话首错、平台 I/O 和内置表，建立源码与堆的共同寿命。 */
    Session(EmergencyFailure &, HostIO &, const BuiltinRegistry &);
    /** @brief 按根、堆对象、元数据的顺序无异常地释放会话资源。 */
    ~Session() noexcept;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    /**
     * @brief 读取入口并完成全文件解析、结构检查、字节码生成与核验。
     * @pre 会话尚未开始编译；每个 Session 仅用于一个命令，编译失败后也须销毁。
     * @return 会话持有的程序，其借用引用有效至会话销毁。
     * @throws Failure 入口读取、前端、结构或字节码处理失败。
     */
    const CompiledProgram &compile_entry(std::string_view);
    /**
     * @brief 按模式运行命令，在统一边界记录异常并尝试输出诊断。
     *
     * Check 成功后直接返回，不进入 VM；错误报告失败仍保留首错退出状态。
     * @return 成功为 0，MYB 错误为 1，用法或入口 I/O 错误为 2，宿主故障为 3。
     */
    int run(const Options &) noexcept;
    /** @brief 借用错误时保存的用户函数链，记录按内层到外层排列。 */
    const std::vector<CallTraceEntry> &call_trace() const noexcept {
        return call_trace_;
    }

  private:
    /** @brief 输出一条转义后的诊断；文本构造失败时尝试固定消息，stderr 失败时停止。 */
    void report(const FailureHeader &, std::string_view) noexcept;
    EmergencyFailure &first_;
    HostIO &io_;
    const BuiltinRegistry &builtins_;
    RootRegistry roots_;
    SourceManager sources_;
    CompiledProgram program_;
    Heap heap_;
    std::vector<CallTraceEntry> call_trace_;
};
} // namespace myb
