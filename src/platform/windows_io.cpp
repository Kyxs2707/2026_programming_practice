#include "platform/host_io.hpp"
#include "source/utf8.hpp"
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <climits>
#include <windows.h>
#ifdef MYB_TEST_HOOKS
#include "test_hooks.hpp"
#endif

namespace myb {
namespace {
/** @brief 抛出只保存宿主原因与已捕获 Windows 代码的无分配异常。 */
[[noreturn]] void io_error(const char *message, DWORD code) {
    throw IoFailure(ReasonCode::IO, message, code);
}
/** @brief 封装读操作，测试构建可分别注入 OS 失败和实际分配失败。 */
BOOL read_bytes(HANDLE handle, void *buffer, DWORD capacity, DWORD &read) {
#ifdef MYB_TEST_HOOKS
    TestHooks::hit(FaultPoint::OsRead);
    if (auto code = TestHooks::take_os_failure(FaultPoint::OsRead)) {
        SetLastError(*code);
        return FALSE;
    }
#endif
    return ReadFile(handle, buffer, capacity, &read, nullptr);
}
/** @brief 封装写操作，返回实际推进的字节数并保留失败时的 OS 代码。 */
BOOL write_bytes(HANDLE handle, const void *buffer, DWORD size, DWORD &written) {
#ifdef MYB_TEST_HOOKS
    TestHooks::hit(FaultPoint::OsWrite);
    if (auto code = TestHooks::take_os_failure(FaultPoint::OsWrite)) {
        SetLastError(*code);
        return FALSE;
    }
#endif
    return WriteFile(handle, buffer, size, &written, nullptr);
}
/** @brief 独占文件句柄；正常关闭报告失败，异常清理只关闭且不抛出异常。 */
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() noexcept {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    /** @brief 显式关闭后撤销所有权，失败保留原 OS 代码；失败清理不重复关闭。 */
    void close() {
#ifdef MYB_TEST_HOOKS
        TestHooks::hit(FaultPoint::OsClose);
        if (auto code = TestHooks::take_os_failure(FaultPoint::OsClose))
            io_error("file close failed", *code);
#endif
        auto handle = value;
        value = INVALID_HANDLE_VALUE;
        if (!CloseHandle(handle)) {
            const auto code = GetLastError();
            io_error("file close failed", code);
        }
    }
};
/** @brief 将 OS 文本转换的容量上限交给统一资源错误边界。 */
void check_conversion_size(std::size_t size, ErrorSite site) {
    if (size > INT_MAX)
        fail(site, FailureDomain::Resource, ReasonCode::Capacity,
             "Windows text conversion exceeds capacity");
}
/** @brief 严格转换宿主 UTF-8，编码失败不预先决定语言错误阶段。 */
std::wstring wide_text(std::string_view input, ErrorSite site) {
    if (input.empty())
        return {};
    check_conversion_size(input.size(), site);
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                         static_cast<int>(input.size()), nullptr, 0);
    if (!size)
        throw IoFailure(ReasonCode::Encoding, "invalid UTF-8 at Windows boundary", GetLastError());
    std::wstring result(size, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                           static_cast<int>(input.size()), result.data(), size) != size)
        throw IoFailure(ReasonCode::Encoding, "UTF-8 conversion failed", GetLastError());
    return result;
}
/** @brief 严格转换宿主 UTF-16，拒绝孤立代理项并保留 NUL。 */
std::string utf8_text(std::wstring_view input, ErrorSite site) {
    if (input.empty())
        return {};
    check_conversion_size(input.size(), site);
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                         static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (!size)
        throw IoFailure(ReasonCode::Encoding, "invalid UTF-16 at Windows boundary", GetLastError());
    std::string result(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                          static_cast<int>(input.size()), result.data(), size, nullptr, nullptr) != size)
        throw IoFailure(ReasonCode::Encoding, "UTF-16 conversion failed", GetLastError());
    return result;
}
/** @brief 拒绝路径 NUL，并以启动工作目录拼接相对路径；不创建目录。 */
std::wstring file_path(std::string_view path, std::wstring_view directory, ErrorSite site) {
    if (path.find('\0') != path.npos)
        throw IoFailure(ReasonCode::IO, "path contains NUL");
    auto wide = wide_text(path, site);
    if (wide.empty())
        io_error("empty file path", ERROR_PATH_NOT_FOUND);
    // 盘符相对路径交给 Windows 解析；普通相对路径固定使用启动目录。
    if (wide[0] != L'/' && wide[0] != L'\\' && !(wide.size() > 1 && wide[1] == L':'))
        return std::wstring(directory) + L'\\' + wide;
    return wide;
}
/** @brief 严格分块解码 UTF-8，carry 只保存跨块的未完整字符；sink 增长可触发 GC。 */
void append_utf8(Utf8Sink &sink, std::string &carry, std::string_view input, bool final) {
    carry.append(input);
    std::size_t at = 0;
    while (at < carry.size()) {
        const auto first = static_cast<unsigned char>(carry[at]);
        const std::size_t width = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
                                                    : first >= 0xe0 && first <= 0xef ? 3
                                                    : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!width)
            throw IoFailure(ReasonCode::Encoding, "invalid UTF-8 input");
        if (carry.size() - at < width) {
            if (final)
                throw IoFailure(ReasonCode::Encoding, "truncated UTF-8 input");
            break;
        }
        if (!utf8::decode_one(carry, at))
            throw IoFailure(ReasonCode::Encoding, "invalid UTF-8 input");
        at += width;
    }
    if (at)
        sink.append(std::span<const char>(carry.data(), at));
    carry.erase(0, at);
}
/** @brief 按字节写尽普通文件和管道；部分写入继续，零进度停止并报告固定 OS 代码。 */
void write_all(HANDLE handle, std::string_view bytes) {
    std::size_t at = 0;
    while (at < bytes.size()) {
        DWORD written{};
        const auto size = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - at, 65536));
        if (!write_bytes(handle, bytes.data() + at, size, written)) {
            const auto code = GetLastError();
            io_error("file or stream write failed", code);
        }
        if (!written)
            io_error("file or stream write made no progress", ERROR_WRITE_FAULT);
        at += written;
    }
}
/** @brief 按句柄类型写尽 UTF-8 字节或 UTF-16 单元，借用句柄而不关闭。 */
void write_handle(HANDLE handle, std::string_view bytes, ErrorSite site) {
    DWORD mode{};
    if (!GetConsoleMode(handle, &mode)) {
        write_all(handle, bytes);
        return;
    }
    auto wide = wide_text(bytes, site);
    std::size_t at = 0;
    while (at < wide.size()) {
        auto size = std::min<std::size_t>(wide.size() - at, 4096);
        if (at + size < wide.size() && wide[at + size - 1] >= 0xd800 &&
            wide[at + size - 1] <= 0xdbff)
            --size;
        DWORD written{};
#ifdef MYB_TEST_HOOKS
        TestHooks::hit(FaultPoint::OsWrite);
        if (auto code = TestHooks::take_os_failure(FaultPoint::OsWrite))
            io_error("console write failed", *code);
#endif
        if (!WriteConsoleW(handle, wide.data() + at, static_cast<DWORD>(size), &written, nullptr)) {
            const auto code = GetLastError();
            io_error("console write failed", code);
        }
        if (!written)
            io_error("console write made no progress", ERROR_WRITE_FAULT);
        at += written;
    }
}
/** @brief 读取一个输入块；零进度、ERROR_HANDLE_EOF 和读端断管都表示 EOF。 */
bool fill_bytes(HANDLE handle, std::string &buffer) {
    char bytes[4096];
    DWORD size{};
    if (!read_bytes(handle, bytes, sizeof(bytes), size)) {
        const auto code = GetLastError();
        if (code == ERROR_HANDLE_EOF || code == ERROR_BROKEN_PIPE)
            return false;
        io_error("input read failed", code);
    }
    buffer.assign(bytes, size);
    return size != 0;
}
/** @brief 读取控制台 UTF-16 块，保留原代码单元以延后验证下一行。 */
bool fill_console(HANDLE handle, std::wstring &buffer) {
    wchar_t units[4096];
    DWORD size{};
#ifdef MYB_TEST_HOOKS
    TestHooks::hit(FaultPoint::OsRead);
    if (auto code = TestHooks::take_os_failure(FaultPoint::OsRead)) {
        if (*code == ERROR_HANDLE_EOF || *code == ERROR_BROKEN_PIPE)
            return false;
        io_error("console read failed", *code);
    }
#endif
    if (!ReadConsoleW(handle, units, 4096, &size, nullptr)) {
        const auto code = GetLastError();
        if (code == ERROR_HANDLE_EOF || code == ERROR_BROKEN_PIPE)
            return false;
        io_error("console read failed", code);
    }
    buffer.assign(units, size);
    return size != 0;
}
/**
 * @brief 逐块划分一行，保留尾 CR 至确定 LF 或 EOF；只把当前行交给编码器。
 * 缓冲最多保存一个 OS 读取块；emit 可以扩容 sink 并触发 GC，所有片段仅借用至调用结束。
 */
template <typename Character, typename Fill, typename Emit>
LineStatus consume_line(std::basic_string<Character> &buffer, bool &ended, Fill fill, Emit emit) {
    bool any = false;
    bool trailing_cr = false;
    for (;;) {
        if (buffer.empty() && !ended)
            ended = !fill(buffer);
        if (buffer.empty() && ended) {
            if (trailing_cr) {
                const Character cr = static_cast<Character>('\r');
                emit(std::basic_string_view<Character>(&cr, 1), false);
            }
            emit({}, true);
            return any ? LineStatus::Line : LineStatus::End;
        }
        const auto lf = buffer.find(static_cast<Character>('\n'));
        auto part = std::basic_string_view<Character>(buffer).substr(0, lf);
        any = any || !part.empty() || lf != buffer.npos;
        if (trailing_cr && (!part.empty() || lf == buffer.npos)) {
            const Character cr = static_cast<Character>('\r');
            emit(std::basic_string_view<Character>(&cr, 1), false);
        }
        trailing_cr = !part.empty() && part.back() == static_cast<Character>('\r');
        if (trailing_cr)
            part.remove_suffix(1);
        emit(part, lf != buffer.npos);
        if (lf != buffer.npos) {
            buffer.erase(0, lf + 1);
            return LineStatus::Line;
        }
        buffer.clear();
    }
}
} // namespace

WindowsHostIO::WindowsHostIO(EmergencyFailure &failure) : failure_(failure) {
    const auto capacity = GetCurrentDirectoryW(0, nullptr);
    if (!capacity)
        fail({failure_, {}}, FailureDomain::Internal, ReasonCode::IO,
             "cannot read working directory", GetLastError());
    working_directory_.resize(capacity);
    const auto used = GetCurrentDirectoryW(capacity, working_directory_.data());
    if (!used || used >= capacity)
        fail({failure_, {}}, FailureDomain::Internal, ReasonCode::IO,
             "cannot read working directory", GetLastError());
    working_directory_.resize(used);
}
std::wstring utf8_to_wide(std::string_view input, ErrorSite site) {
    try {
        return wide_text(input, site);
    } catch (const IoFailure &failure) {
        fail(site, FailureDomain::Internal, failure.reason(), failure.what(), failure.os_code());
    }
}
std::string wide_to_utf8(std::wstring_view input, ErrorSite site) {
    try {
        return utf8_text(input, site);
    } catch (const IoFailure &failure) {
        fail(site, FailureDomain::Internal, failure.reason(), failure.what(), failure.os_code());
    }
}
EntryBytes WindowsHostIO::read_entry(std::string_view path) {
    const ErrorSite site{failure_, {}};
    try {
        const auto wide = file_path(path, working_directory_, site);
        const auto capacity = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
        if (!capacity)
            io_error("cannot resolve source path", GetLastError());
        std::wstring absolute(capacity, L'\0');
        const auto used = GetFullPathNameW(wide.c_str(), capacity, absolute.data(), nullptr);
        if (!used || used >= capacity)
            io_error("cannot resolve source path", GetLastError());
        absolute.resize(used);
        Handle file{CreateFileW(absolute.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE)
            io_error("cannot open source file", GetLastError());
        std::string bytes, block;
        while (fill_bytes(file.value, block))
            bytes.append(block);
        file.close();
        return {utf8_text(absolute, site), std::move(bytes)};
    } catch (const IoFailure &failure) {
        // 在拼接入口路径前固定首错，消息分配再次失败时仍保留入口错误退出状态。
        failure_.record_first({FailureDomain::EntryIO, failure.reason(), {}, failure.os_code()});
        std::string message(failure.what());
        message += ": ";
        message += path;
        fail(site, FailureDomain::EntryIO, failure.reason(), message, failure.os_code());
    }
}
void WindowsHostIO::write_stdout(std::string_view bytes, bool append_lf) {
    try {
        const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
        write_handle(handle, bytes, {failure_, {}});
        if (append_lf)
            write_handle(handle, "\n", {failure_, {}});
    } catch (const IoFailure &failure) {
        fail({failure_, {}}, FailureDomain::Runtime, failure.reason(), failure.what(), failure.os_code());
    }
}
void WindowsHostIO::write_stderr(std::string_view bytes) {
    try {
        write_handle(GetStdHandle(STD_ERROR_HANDLE), bytes, {failure_, {}});
    } catch (const IoFailure &failure) {
        fail({failure_, {}}, FailureDomain::Internal, failure.reason(), failure.what(), failure.os_code());
    }
}
void WindowsHostIO::read_text(std::string_view path, Utf8Sink &sink) {
    const auto wide = file_path(path, working_directory_, {failure_, {}});
    Handle file{CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        io_error("cannot open text file", GetLastError());
    std::string block, carry;
    bool first = true;
    while (fill_bytes(file.value, block)) {
        if (first) {
            carry.append(block);
            if (carry.size() < 3)
                continue;
            if (carry.starts_with("\xef\xbb\xbf"))
                carry.erase(0, 3);
            block.swap(carry);
            carry.clear();
            first = false;
        }
        append_utf8(sink, carry, block, false);
    }
    append_utf8(sink, carry, {}, true);
    file.close();
}
void WindowsHostIO::write_text(std::string_view path, std::string_view text) {
    const auto wide = file_path(path, working_directory_, {failure_, {}});
    if (!utf8::valid(text))
        throw IoFailure(ReasonCode::Encoding, "invalid UTF-8 text");
    Handle file{CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        io_error("cannot create text file", GetLastError());
    write_all(file.value, text);
    file.close();
}
LineStatus WindowsHostIO::read_line(Utf8Sink &sink) {
    if (!input_started_) {
        const auto handle = GetStdHandle(STD_INPUT_HANDLE);
        input_handle_ = reinterpret_cast<std::uintptr_t>(handle);
        DWORD mode{};
        input_console_ = GetConsoleMode(handle, &mode) != FALSE;
        input_started_ = true;
    }
    const auto handle = reinterpret_cast<HANDLE>(input_handle_);
    if (!input_console_) {
        std::string carry;
        return consume_line(input_bytes_, input_end_,
                            [&](std::string &buffer) { return fill_bytes(handle, buffer); },
                            [&](std::string_view part, bool final) { append_utf8(sink, carry, part, final); });
    }
    std::wstring carry;
    bool held_control_z = false;
    bool emitted = false;
    const auto status = consume_line(input_wide_, input_end_,
        [&](std::wstring &buffer) { return fill_console(handle, buffer); },
        [&](std::wstring_view part, bool final) {
            if (held_control_z && !part.empty()) {
                const char control_z = '\x1a';
                sink.append(std::span<const char>(&control_z, 1));
                held_control_z = false;
                emitted = true;
            }
            if (!emitted && carry.empty() && !held_control_z && part == L"\x1a") {
                held_control_z = true;
                return;
            }
            carry.append(part);
            auto count = carry.size();
            if (!final && count && carry.back() >= 0xd800 && carry.back() <= 0xdbff)
                --count;
            const auto bytes = utf8_text(std::wstring_view(carry).substr(0, count), {failure_, {}});
            if (!bytes.empty()) {
                sink.append(std::span<const char>(bytes.data(), bytes.size()));
                emitted = true;
            }
            carry.erase(0, count);
        });
    if (held_control_z && !emitted) {
        input_end_ = true;
        input_wide_.clear();
        return LineStatus::End;
    }
    return status;
}
} // namespace myb
