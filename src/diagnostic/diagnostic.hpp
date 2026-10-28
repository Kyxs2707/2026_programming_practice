#pragma once
#include "diagnostic/failure.hpp"
#include "vm/call.hpp"
#include <span>
#include <string>
namespace myb {
class HostIO;
class SourceManager;
/**
 * @brief 独占保存错误头和已排版的 UTF-8 诊断，不借用源码、程序名称或受管对象。
 * 文本包含错误位置与实际用户调用点；构造后可在原会话及堆销毁后报告。
 */
struct Diagnostic {
    FailureHeader header;
    std::string text;
};
/**
 * @brief 将首错、原因和已保存的调用链转换为独立文本，转义用户控制字符。
 * @param[in] sources 在本次调用期间有效的源码记录。
 * @param[in] trace VM 清理前保存的内层到外层函数调用点。
 * @return 完全拥有文本存储的诊断。
 * @throws std::bad_alloc 宿主文本分配失败，调用方须使用最小报告。
 * 只分配宿主文本，不分配受管对象或触发 GC。
 */
Diagnostic make_diagnostic(const FailureHeader &, std::string_view message,
                           const SourceManager &sources, std::span<const CallTraceEntry> trace);
/** @brief 向 stderr 写出一次诊断；写出失败后停止，不递归报告或重试。 */
void report_failure(const Diagnostic &, HostIO &) noexcept;
/** @brief 仅尝试写出一次固定 ASCII 消息，不构造动态文本，写出失败后停止。 */
void report_minimal_failure(HostIO &) noexcept;
} // namespace myb
