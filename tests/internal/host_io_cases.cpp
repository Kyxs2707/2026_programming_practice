#include "platform/host_io.hpp"
#include "builtins/functions.hpp"
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
} // namespace

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
