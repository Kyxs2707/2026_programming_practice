#pragma once
#include "diagnostic/failure.hpp"
#include "platform/utf8_sink.hpp"
#include <string>
#include <string_view>
namespace myb {
/**
 * @brief 无分配地携带宿主 I/O 原因及 Windows 错误码，由调用方决定语言退出状态。
 * 消息须为静态存储期文本；异常不持有受管对象，也不记录会话首错。
 */
class IoFailure final : public std::exception {
  public:
    /** @brief 保存原因、可选 OS 代码及具有静态寿命的说明。 */
    IoFailure(ReasonCode reason, const char *message,
              std::optional<std::uint32_t> code = {}) noexcept
        : reason_(reason), message_(message), code_(code) {}
    /** @brief 返回宿主失败原因。 */
    ReasonCode reason() const noexcept { return reason_; }
    /** @brief 返回调用失败时保存的 OS 错误码。 */
    std::optional<std::uint32_t> os_code() const noexcept { return code_; }
    /** @brief 借用具有静态寿命的错误说明。 */
    const char *what() const noexcept override { return message_; }
  private:
    ReasonCode reason_;
    const char *message_;
    std::optional<std::uint32_t> code_;
};
/** @brief 入口读取结果，独占保存解析后的 UTF-8 路径与未经文本转换的源码字节。 */
struct EntryBytes {
    std::string path_utf8, bytes;
};
/** @brief 文本行读取状态，区分一行内容与输入结束。 */
enum class LineStatus { Line, End };
/**
 * @brief 隔离核心与平台 I/O，只交换宿主文本和字节，不持有受管对象。
 *
 * 传入视图只在调用期间借用；Utf8Sink 的存储增长可触发 GC，受管值由上层保活。
 */
class HostIO {
  public:
    virtual ~HostIO() noexcept = default;
    /** @brief 读取入口的原始字节并返回拥有存储的路径与内容。 */
    virtual EntryBytes read_entry(std::string_view) = 0;
    /** @brief 严格读取一行 UTF-8，移去 LF/CRLF；正常 EOF 返回 End，失败抛 IoFailure。 */
    virtual LineStatus read_line(Utf8Sink &) = 0;
    /** @brief 严格读取 UTF-8 文件，去一次开头 BOM，保留其余字节；失败抛 IoFailure。 */
    virtual void read_text(std::string_view, Utf8Sink &) = 0;
    /** @brief 创建或覆盖文本文件，按长度写尽并检查关闭；失败抛 IoFailure。 */
    virtual void write_text(std::string_view, std::string_view) = 0;
    /** @brief 写出全部 stdout 字节，并按 append_lf 决定是否额外写一个 LF。 */
    virtual void write_stdout(std::string_view, bool append_lf) = 0;
    /** @brief 写出全部 stderr 字节，失败交由诊断调用方处理。 */
    virtual void write_stderr(std::string_view) = 0;
};
/**
 * @brief 严格将 UTF-8 转为 Windows UTF-16，保留内嵌 NUL，空串返回空串。
 * @throws HostFailure 输入超过 INT_MAX 为 Resource/Capacity，编码或转换失败为 Internal/Encoding。
 */
std::wstring utf8_to_wide(std::string_view, ErrorSite);
/**
 * @brief 严格将 Windows UTF-16 转为 UTF-8，拒绝孤立代理项并保留内嵌 NUL。
 * @throws HostFailure 输入超过 INT_MAX 为 Resource/Capacity，编码或转换失败为 Internal/Encoding。
 */
std::string wide_to_utf8(std::wstring_view, ErrorSite);
/**
 * @brief 使用 Windows 宽路径读写文件，并按标准句柄类型严格桥接 Unicode 文本。
 *
 * 借用的首错记录须覆盖本对象寿命。入口路径拒绝 NUL 并解析为绝对路径；
 * 源码编码由 SourceManager 验证。控制台输出严格转 UTF-16，文件和管道按原字节写尽。
 * 标准流由宿主持有，本对象不关闭；输入缓冲保存尚未交出的当前块内容。
 * 文件相对路径按构造时工作目录解析；输入缓冲不持有受管对象，sink 增长可触发 GC。
 */
class WindowsHostIO final : public HostIO {
  public:
    /** @brief 绑定会话首错记录，不预读标准流。 */
    explicit WindowsHostIO(EmergencyFailure &failure);
    EntryBytes read_entry(std::string_view) override;
    LineStatus read_line(Utf8Sink &) override;
    void read_text(std::string_view, Utf8Sink &) override;
    void write_text(std::string_view, std::string_view) override;
    void write_stdout(std::string_view, bool append_lf) override;
    void write_stderr(std::string_view) override;

  private:
    EmergencyFailure &failure_;
    std::wstring working_directory_;
    std::string input_bytes_;
    std::wstring input_wide_;
    bool input_started_{};
    bool input_console_{};
    bool input_end_{};
    std::uintptr_t input_handle_{};
};
} // namespace myb
