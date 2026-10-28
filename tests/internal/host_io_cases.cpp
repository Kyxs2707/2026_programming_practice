#include "platform/host_io.hpp"
#include "builtins/functions.hpp"
#include "cli/session.hpp"
#include "runtime/text_builder.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include <filesystem>
#include <string>
#include <array>
#include <climits>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace myb;
namespace {
/** @brief 收集宿主文本接口交出的原始字节，不涉及受管对象。 */
struct StringSink final : Utf8Sink {
    std::string bytes;
    void append(std::span<const char> input) override {
        bytes.append(input.data(), input.size());
    }
};
/** @brief 独占管道两端并临时替换标准句柄，析构恢复原句柄。 */
struct PipeInput {
    HANDLE reader{}, writer{}, previous{GetStdHandle(STD_INPUT_HANDLE)};
    explicit PipeInput(std::string_view bytes) {
        REQUIRE(CreatePipe(&reader, &writer, nullptr, 65536));
        DWORD written{};
        REQUIRE(WriteFile(writer, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
        REQUIRE(written == bytes.size());
        REQUIRE(CloseHandle(writer));
        writer = nullptr;
        REQUIRE(SetStdHandle(STD_INPUT_HANDLE, reader));
    }
    ~PipeInput() noexcept {
        SetStdHandle(STD_INPUT_HANDLE, previous);
        if (writer) CloseHandle(writer);
        if (reader) CloseHandle(reader);
    }
};
/** @brief 保存并恢复 stdout，持有一个读端已经关闭的真实匿名管道。 */
struct BrokenOutput {
    HANDLE writer{}, previous{GetStdHandle(STD_OUTPUT_HANDLE)};
    BrokenOutput() {
        HANDLE reader{};
        REQUIRE(CreatePipe(&reader, &writer, nullptr, 0));
        REQUIRE(CloseHandle(reader));
        REQUIRE(SetStdHandle(STD_OUTPUT_HANDLE, writer));
    }
    ~BrokenOutput() noexcept {
        SetStdHandle(STD_OUTPUT_HANDLE, previous);
        CloseHandle(writer);
    }
};
/** @brief 检查无分配宿主异常保留原因和注入的 Windows 代码。 */
template <typename Action>
void expect_os_failure(Action action, DWORD code) {
    bool failed = false;
    try {
        action();
    } catch (const IoFailure &failure) {
        REQUIRE(failure.reason() == ReasonCode::IO);
        REQUIRE(failure.os_code() == code);
        failed = true;
    }
    REQUIRE(failed);
}
/** @brief 将测试字节创建为已登记字符串根，缓冲增长与发布可以触发 GC。 */
void make_string(Heap &heap, RootedValue &out, std::string_view bytes) {
    TextBuilder builder(heap);
    builder.append(std::span<const char>(bytes.data(), bytes.size()));
    heap.make_string(std::move(builder), out);
}
/** @brief 临时接管一个标准输出句柄，独占管道并在离开作用域时恢复宿主句柄。 */
struct CapturedEntryStream {
    DWORD stream;
    HANDLE reader{}, writer{}, previous;
    explicit CapturedEntryStream(DWORD stream_id)
        : stream(stream_id), previous(GetStdHandle(stream_id)) {
        REQUIRE(CreatePipe(&reader, &writer, nullptr, 4096));
        REQUIRE(SetStdHandle(stream, writer));
    }
    ~CapturedEntryStream() noexcept {
        SetStdHandle(stream, previous);
        CloseHandle(writer);
        CloseHandle(reader);
    }
    /** @brief 读取当前已写入的短诊断；先查询可用字节，不等待尚未关闭的写端。 */
    std::string read_available() {
        DWORD available{}, read{};
        REQUIRE(PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr));
        if (!available)
            return {};
        std::string bytes(available, '\0');
        REQUIRE(ReadFile(reader, bytes.data(), available, &read, nullptr));
        REQUIRE(read == available);
        return bytes;
    }
};
/** @brief 保存一次真实入口会话的退出状态、首错头、输出和调用链长度。 */
struct EntryObservation {
    int exit{};
    std::optional<FailureHeader> failure;
    std::string output, diagnostic;
    std::size_t calls{};
};
/** @brief 恢复入口测试的故障状态，断言失败时也不影响后续用例。 */
struct EntryFaultScope {
    ~EntryFaultScope() noexcept { TestHooks::clear_fault(); }
};
/**
 * @brief 在真实 WindowsHostIO 和 Session 边界执行入口，arm 在会话建立后设置故障。
 * @param[in] path 本次入口借用的路径，生命周期覆盖会话执行。
 * @param[in] mode 正常执行或检查模式。
 * @param[in] arm 故障设置动作，不替换入口读取、错误转换或诊断输出。
 * @return 拥有全部观察文本的会话结果；标准句柄和故障设置已恢复。
 */
template <typename Arm>
EntryObservation observe_entry(std::string_view path, RunMode mode, Arm arm) {
    EntryFaultScope scope;
    EmergencyFailure first;
    WindowsHostIO host(first);
    BuiltinRegistry builtins;
    Session session(first, host, builtins);
    CapturedEntryStream output(STD_OUTPUT_HANDLE), diagnostic(STD_ERROR_HANDLE);
    arm();
    EntryObservation result;
    result.exit = session.run({mode, std::string(path)});
    if (first.is_set())
        result.failure = first.header();
    result.calls = session.call_trace().size();
    result.output = output.read_available();
    result.diagnostic = diagnostic.read_available();
    return result;
}
/** @brief 核对入口故障首错、无源码位置及无用户调用链，并保留真实 Session 返回值检查。 */
void require_entry_failure(const EntryObservation &result, FailureDomain domain,
                           ReasonCode reason, int exit, std::optional<std::uint32_t> code) {
    REQUIRE(result.exit == exit);
    REQUIRE(result.failure.has_value());
    REQUIRE(result.failure->domain == domain);
    REQUIRE(result.failure->reason == reason);
    REQUIRE(result.failure->os_code == code);
    REQUIRE(!result.failure->span);
    REQUIRE(result.output.empty());
    REQUIRE(result.calls == 0);
    REQUIRE(result.diagnostic.find("  at ") == std::string::npos);
}
} // namespace

MYB_TEST(host_io_entry_failure) {
    const std::string relative = "build/bugfix-20261028/b03/入口 源.myb";
    auto path = std::filesystem::absolute(
        std::filesystem::path(L"build/bugfix-20261028/b03/入口 源.myb"));
    std::filesystem::create_directories(path.parent_path());
    path.make_preferred();
    EmergencyFailure setup;
    WindowsHostIO host(setup);
    const auto absolute = wide_to_utf8(path.wstring(), {setup, {}});
    host.write_text(relative, "");
    for (const auto mode : {RunMode::Run, RunMode::Check}) {
        const auto missing = observe_entry(relative + ".missing", mode, [] {});
        require_entry_failure(missing, FailureDomain::EntryIO, ReasonCode::IO, 2,
                              ERROR_FILE_NOT_FOUND);
        REQUIRE(missing.diagnostic.find(absolute + ".missing") != std::string::npos);
        for (const auto point : {FaultPoint::OsRead, FaultPoint::OsClose}) {
            const auto code = point == FaultPoint::OsRead ? ERROR_READ_FAULT : ERROR_INVALID_HANDLE;
            const auto result = observe_entry(relative, mode, [&] {
                TestHooks::fail_os_next(point, code);
            });
            require_entry_failure(result, FailureDomain::EntryIO, ReasonCode::IO, 2, code);
            REQUIRE(result.diagnostic.find(absolute) != std::string::npos);
            // 入口错误清理释放文件句柄，独占打开不受上次读取或关闭失败影响。
            const auto reopened = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            REQUIRE(reopened != INVALID_HANDLE_VALUE);
            REQUIRE(CloseHandle(reopened));
        }
        for (const auto point : {FaultPoint::EntryPathBuffer, FaultPoint::EntryPathText}) {
            const auto result = observe_entry(relative, mode, [&] { TestHooks::fail_next(point); });
            require_entry_failure(result, FailureDomain::Resource, ReasonCode::Allocation, 3, {});
        }
        for (const auto point : {FaultPoint::EntryPathBuffer, FaultPoint::EntryPathText}) {
            const bool encoding = point == FaultPoint::EntryPathText;
            const auto code = encoding ? ERROR_NO_UNICODE_TRANSLATION : ERROR_INVALID_NAME;
            const auto result = observe_entry(relative, mode, [&] {
                TestHooks::fail_os_next(point, code);
            });
            require_entry_failure(result, FailureDomain::EntryIO,
                                  encoding ? ReasonCode::Encoding : ReasonCode::IO, 2, code);
            REQUIRE(result.diagnostic.find(relative) != std::string::npos);
            REQUIRE(result.diagnostic.find(absolute) == std::string::npos);
        }
        const auto failed_message = observe_entry(relative + ".missing", mode, [] {
            TestHooks::fail_next(FaultPoint::EntryDiagnostic);
        });
        require_entry_failure(failed_message, FailureDomain::EntryIO, ReasonCode::IO, 2,
                              ERROR_FILE_NOT_FOUND);
        REQUIRE(failed_message.diagnostic.find("entry I/O error [IO]") != std::string::npos);
    }
    REQUIRE(std::filesystem::remove(path));
}

MYB_TEST(host_io) {
    EmergencyFailure first;
    WindowsHostIO io(first);
    {
        PipeInput pipe("ok\n\xff\n");
        StringSink one;
        REQUIRE(io.read_line(one) == LineStatus::Line);
        REQUIRE(one.bytes == "ok");
        bool failed = false;
        try {
            StringSink two;
            io.read_line(two);
        } catch (const IoFailure &failure) {
            REQUIRE(failure.reason() == ReasonCode::Encoding);
            REQUIRE(!first.is_set());
            failed = true;
        }
        REQUIRE(failed);
    }
    {
        PipeInput pipe("\nlast");
        WindowsHostIO lines(first);
        StringSink empty, last, end;
        REQUIRE(lines.read_line(empty) == LineStatus::Line);
        REQUIRE(empty.bytes.empty());
        REQUIRE(lines.read_line(last) == LineStatus::Line);
        REQUIRE(last.bytes == "last");
        REQUIRE(lines.read_line(end) == LineStatus::End);
    }
    {
        PipeInput pipe(std::string(4095, 'a') + "\xf0\x9f\x98\x80\r\n\x1a\n");
        WindowsHostIO lines(first);
        StringSink long_line, control, end;
        REQUIRE(lines.read_line(long_line) == LineStatus::Line);
        REQUIRE(long_line.bytes == std::string(4095, 'a') + "\xf0\x9f\x98\x80");
        REQUIRE(lines.read_line(control) == LineStatus::Line);
        REQUIRE(control.bytes == "\x1a");
        REQUIRE(lines.read_line(end) == LineStatus::End);
    }
    auto path = std::filesystem::absolute(std::filesystem::path("build") / L"文本 读写.txt");
    auto path_utf8 = wide_to_utf8(path.wstring(), {first, {}});
    const std::string bytes("\xef\xbb\xbf\xef\xbb\xbf" "A\r\n\0", 10);
    io.write_text(path_utf8, bytes);
    StringSink content;
    io.read_text(path_utf8, content);
    REQUIRE(content.bytes == bytes.substr(3));

    // 文件缓冲边界中的四字节码点，以及跨多次写入的文本都保留原字节。
    const auto large = std::string(4095, 'a') + "\xf0\x9f\x98\x80" +
                       std::string(131072, 'b') + std::string("\r\n\0", 3);
    io.write_text(path_utf8, large);
    StringSink large_content;
    io.read_text(path_utf8, large_content);
    REQUIRE(large_content.bytes == large);
    {
        const auto original = std::filesystem::current_path();
        REQUIRE(SetCurrentDirectoryW((original / "build").c_str()));
        try {
            StringSink relative_content;
            io.read_text("build/文本 读写.txt", relative_content);
            REQUIRE(relative_content.bytes == large);
        } catch (...) {
            SetCurrentDirectoryW(original.c_str());
            throw;
        }
        REQUIRE(SetCurrentDirectoryW(original.c_str()));
    }

    for (const auto code : {ERROR_HANDLE_EOF, ERROR_BROKEN_PIPE}) {
        PipeInput pipe("");
        EmergencyFailure state;
        WindowsHostIO lines(state);
        TestHooks::fail_os_next(FaultPoint::OsRead, code);
        StringSink sink;
        REQUIRE(lines.read_line(sink) == LineStatus::End);
        REQUIRE(lines.read_line(sink) == LineStatus::End);
        REQUIRE(!state.is_set());
    }
    {
        PipeInput pipe("x");
        EmergencyFailure state;
        WindowsHostIO lines(state);
        TestHooks::fail_os_next(FaultPoint::OsRead, ERROR_ACCESS_DENIED);
        expect_os_failure([&] { StringSink sink; lines.read_line(sink); }, ERROR_ACCESS_DENIED);
        REQUIRE(!state.is_set());
    }
    TestHooks::fail_os_next(FaultPoint::OsRead, ERROR_READ_FAULT);
    expect_os_failure([&] { StringSink sink; io.read_text(path_utf8, sink); }, ERROR_READ_FAULT);
    TestHooks::fail_os_next(FaultPoint::OsWrite, ERROR_DISK_FULL);
    expect_os_failure([&] { io.write_text(path_utf8, "saved"); }, ERROR_DISK_FULL);
    for (bool writing : {false, true}) {
        TestHooks::fail_os_next(FaultPoint::OsClose, ERROR_INVALID_HANDLE);
        expect_os_failure([&] {
            if (writing) io.write_text(path_utf8, "saved");
            else { StringSink sink; io.read_text(path_utf8, sink); }
        }, ERROR_INVALID_HANDLE);
        // 注入关闭失败后的 RAII 清理必须释放独占写句柄，下一次打开才能成功。
        io.write_text(path_utf8, "saved");
    }
    for (const auto point : {FaultPoint::OsRead, FaultPoint::OsWrite, FaultPoint::OsClose}) {
        TestHooks::fail_next(point);
        bool allocation_failed = false;
        try {
            if (point == FaultPoint::OsRead) { StringSink sink; io.read_text(path_utf8, sink); }
            else io.write_text(path_utf8, "saved");
        } catch (const std::bad_alloc &) {
            allocation_failed = true;
        }
        REQUIRE(allocation_failed);
        io.write_text(path_utf8, "saved");
    }
    {
        EmergencyFailure state;
        WindowsHostIO entry(state);
        TestHooks::fail_os_next(FaultPoint::OsRead, ERROR_READ_FAULT);
        bool failed = false;
        try { entry.read_entry(path_utf8); }
        catch (const HostFailure &failure) {
            REQUIRE(failure.header().domain == FailureDomain::EntryIO);
            REQUIRE(failure.header().os_code == ERROR_READ_FAULT);
            REQUIRE(exit_code(failure.header()) == 2);
            failed = true;
        }
        REQUIRE(failed);
    }
    for (const auto point : {FaultPoint::OsRead, FaultPoint::OsWrite}) {
        EmergencyFailure state;
        WindowsHostIO host(state);
        RootRegistry roots;
        Heap heap(roots, state);
        RootedValue path_root(roots), text_root(roots), out(roots);
        NativeContext context{heap, roots, host, state, {3, 4, 5}};
        make_string(heap, path_root, path_utf8);
        make_string(heap, text_root, "saved");
        const std::array arguments{path_root.get(), text_root.get()};
        TestHooks::fail_os_next(point, ERROR_ACCESS_DENIED);
        bool failed = false;
        try {
            if (point == FaultPoint::OsRead)
                builtin_read_text(context, std::span<const Value>(arguments.data(), 1), out);
            else
                builtin_write_text(context, arguments, out);
        } catch (const MybRuntimeFailure &failure) {
            REQUIRE(failure.header().domain == FailureDomain::Runtime);
            REQUIRE(failure.header().reason == ReasonCode::IO);
            REQUIRE(failure.header().os_code == ERROR_ACCESS_DENIED);
            REQUIRE(failure.header().span == context.span);
            REQUIRE(exit_code(failure.header()) == 1);
            failed = true;
        }
        REQUIRE(failed);
    }
    {
        EmergencyFailure state;
        bool failed = false;
        try {
            utf8_to_wide(std::string_view("x", static_cast<std::size_t>(INT_MAX) + 1), {state, {}});
        } catch (const HostFailure &failure) {
            REQUIRE(failure.header().domain == FailureDomain::Resource);
            REQUIRE(failure.header().reason == ReasonCode::Capacity);
            REQUIRE(exit_code(failure.header()) == 3);
            failed = true;
        }
        REQUIRE(failed);
    }
    {
        BrokenOutput pipe;
        DWORD written{};
        REQUIRE(!WriteFile(pipe.writer, "x", 1, &written, nullptr));
        const auto native_code = GetLastError();
        REQUIRE(native_code == ERROR_BROKEN_PIPE || native_code == ERROR_NO_DATA);
        EmergencyFailure state;
        WindowsHostIO output(state);
        bool failed = false;
        try { output.write_stdout("x", false); }
        catch (const IoFailure &failure) {
            REQUIRE(failure.reason() == ReasonCode::IO);
            REQUIRE(failure.os_code() == native_code);
            REQUIRE(!state.is_set());
            failed = true;
        }
        REQUIRE(failed);
    }
    {
        EmergencyFailure state;
        WindowsHostIO host(state);
        RootRegistry roots;
        Heap heap(roots, state);
        RootedValue path_root(roots), text_root(roots), out(roots);
        NativeContext context{heap, roots, host, state, {7, 8, 9}};
        TestHooks::force_gc(true);
        make_string(heap, path_root, path_utf8);
        make_string(heap, text_root, large);
        const std::array arguments{path_root.get(), text_root.get()};
        builtin_write_text(context, arguments, out);
        REQUIRE(std::holds_alternative<NullValue>(out.get().data));
        builtin_read_text(context, std::span<const Value>(arguments.data(), 1), out);
        REQUIRE(std::get<StringRef>(out.get().data).ptr->utf8 == large);
        TestHooks::fail_os_next(FaultPoint::OsClose, ERROR_DISK_FULL);
        bool failed = false;
        try { builtin_write_text(context, arguments, out); }
        catch (const MybRuntimeFailure &failure) {
            REQUIRE(failure.header().domain == FailureDomain::Runtime);
            REQUIRE(failure.header().reason == ReasonCode::IO);
            REQUIRE(failure.header().os_code == ERROR_DISK_FULL);
            REQUIRE(failure.header().span == context.span);
            REQUIRE(exit_code(failure.header()) == 1);
            failed = true;
        }
        REQUIRE(failed);
        TestHooks::force_gc(false);
    }
    REQUIRE(std::filesystem::remove(path));
}
