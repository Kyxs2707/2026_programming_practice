#include "cli/session.hpp"
#include "diagnostic/diagnostic.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include <array>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace myb;
namespace {
/** @brief 在真实 Session 边界保存输出，并模拟宿主资源或写出失败。 */
class DiagnosticIO final : public HostIO {
  public:
    std::string source, out, err;
    std::size_t reports{};
    bool reject_output{}, reject_report{};
    EntryBytes read_entry(std::string_view path) override { return {std::string(path), source}; }
    LineStatus read_line(Utf8Sink &) override { return LineStatus::End; }
    void read_text(std::string_view, Utf8Sink &) override { throw std::logic_error("unused"); }
    void write_text(std::string_view, std::string_view) override { throw std::logic_error("unused"); }
    void write_stdout(std::string_view text, bool newline) override {
        if (reject_output)
            throw IoFailure(ReasonCode::IO, "output rejected", 5);
        out.append(text);
        if (newline)
            out += '\n';
    }
    void write_stderr(std::string_view text) override {
        ++reports;
        if (reject_report)
            throw IoFailure(ReasonCode::IO, "report rejected", 5);
        err.append(text);
    }
};
/** @brief 临时接管 stderr 的真实管道，析构先恢复原句柄再关闭两端。 */
struct DiagnosticPipe {
    HANDLE reader{}, writer{}, previous{GetStdHandle(STD_ERROR_HANDLE)};
    DiagnosticPipe() {
        REQUIRE(CreatePipe(&reader, &writer, nullptr, 0));
        REQUIRE(SetStdHandle(STD_ERROR_HANDLE, writer));
    }
    ~DiagnosticPipe() noexcept {
        SetStdHandle(STD_ERROR_HANDLE, previous);
        CloseHandle(writer);
        CloseHandle(reader);
    }
};
void capacity_failure(NativeContext &, std::span<const Value>, RootedValue &) {
    throw std::length_error("host capacity");
}
void unknown_failure(NativeContext &, std::span<const Value>, RootedValue &) {
    throw std::logic_error("host invariant");
}
void allocation_failure(NativeContext &, std::span<const Value>, RootedValue &) {
    throw std::bad_alloc{};
}
void original_then_allocation(NativeContext &context, std::span<const Value>, RootedValue &) {
    context.failure.record_first({FailureDomain::Runtime, ReasonCode::Type, context.span, 17});
    throw std::bad_alloc{};
}
constexpr std::string_view nested_failure =
    "fn inner() {\n    trap();\n}\nfn outer() { inner(); }\nouter();";
} // namespace

MYB_TEST(diagnostic_host_boundaries) {
    const std::array<NativeFn, 3> functions{capacity_failure, unknown_failure, allocation_failure};
    const std::array domains{FailureDomain::Resource, FailureDomain::Internal, FailureDomain::Resource};
    const std::array reasons{ReasonCode::Capacity, ReasonCode::Invariant, ReasonCode::Allocation};
    for (std::size_t i = 0; i < functions.size(); ++i) {
        EmergencyFailure first;
        DiagnosticIO io;
        io.source = nested_failure;
        const std::array descriptors{BuiltinDescriptor{"trap", 0, functions[i]}};
        BuiltinRegistry registry(descriptors);
        Session session(first, io, registry);
        REQUIRE(session.run({RunMode::Run, "host.myb"}) == 3);
        REQUIRE(first.header().domain == domains[i]);
        REQUIRE(first.header().reason == reasons[i]);
        REQUIRE(first.header().span.has_value());
        REQUIRE(io.err.starts_with("host.myb:2:5: "));
        REQUIRE(io.reports == 1);
        REQUIRE(session.call_trace().size() == 2);
        REQUIRE(session.call_trace()[0].name == "inner");
        REQUIRE(session.call_trace()[1].name == "outer");
    }
}

MYB_TEST(diagnostic_fallback) {
    for (const auto occurrence : {1u, 2u}) {
        EmergencyFailure first;
        DiagnosticIO io;
        io.source = "fn inner(x) {\n    return x + 1;\n}\nfn outer() { return inner(null); }\nouter();";
        BuiltinRegistry registry;
        Session session(first, io, registry);
        TestHooks::fail_next(FaultPoint::DiagnosticSnapshot, occurrence);
        const auto result = session.run({RunMode::Run, "failure.myb"});
        TestHooks::clear_fault();
        REQUIRE(result == 1);
        REQUIRE(first.header().domain == FailureDomain::Runtime);
        REQUIRE(first.header().reason == ReasonCode::Type);
        REQUIRE(first.header().span.has_value());
        REQUIRE(io.reports == 1);
        if (occurrence == 2)
            REQUIRE(io.err == "myb: failure (diagnostic unavailable)\n");
    }
    EmergencyFailure first;
    DiagnosticIO io;
    io.source = nested_failure;
    io.reject_report = true;
    const std::array descriptors{BuiltinDescriptor{"trap", 0, original_then_allocation}};
    BuiltinRegistry registry(descriptors);
    Session session(first, io, registry);
    REQUIRE(session.run({RunMode::Run, "first.myb"}) == 1);
    REQUIRE(first.header().domain == FailureDomain::Runtime);
    REQUIRE(first.header().reason == ReasonCode::Type);
    REQUIRE(first.header().os_code == 17);
    REQUIRE(io.reports == 1);
    REQUIRE(io.err.empty());
}

MYB_TEST(diagnostic_command_output) {
    for (auto mode : {RunMode::Help, RunMode::Version}) {
        EmergencyFailure first;
        DiagnosticIO io;
        io.reject_output = true;
        BuiltinRegistry registry;
        Session session(first, io, registry);
        REQUIRE(session.run({mode, {}}) == 3);
        REQUIRE(first.is_set());
        REQUIRE(first.header().domain == FailureDomain::Internal);
        REQUIRE(first.header().reason == ReasonCode::IO);
        REQUIRE(!first.header().span);
        REQUIRE(io.reports == 1);
        REQUIRE(io.err.starts_with("myb: internal error [IO]: "));
    }
}

MYB_TEST(diagnostic_owned_text) {
    Diagnostic diagnostic;
    {
        EmergencyFailure first;
        SourceManager sources(first);
        const auto id = sources.add("demo\t.myb", "x\ny\n");
        std::vector<CallTraceEntry> trace{{"outer", {id, 0, 1}}};
        std::string message{"bad\0text\n\x1b", 10};
        diagnostic = make_diagnostic({FailureDomain::Runtime, ReasonCode::Assertion,
                                      SourceSpan{id, 2, 3}, 5}, message, sources, trace);
        message.assign("replaced");
        trace[0].name.assign("replaced");
    }
    DiagnosticIO io;
    report_failure(diagnostic, io);
    REQUIRE(io.reports == 1);
    REQUIRE(io.err == "demo\\t.myb:2:1: runtime error [Assertion]: "
                      "bad\\x00text\\n\\x1b (OS 5)\n  at outer (demo\\t.myb:1:1)\n");
}

MYB_TEST(diagnostic_emergency_io) {
    EmergencyFailure first;
    first.record_first({FailureDomain::Runtime, ReasonCode::Type, {}, 17});
    WindowsHostIO io(first);
    DiagnosticPipe pipe;
    report_minimal_failure(io);
    char bytes[80]{};
    DWORD available{}, read{};
    REQUIRE(PeekNamedPipe(pipe.reader, nullptr, 0, nullptr, &available, nullptr));
    REQUIRE(available == 38);
    REQUIRE(ReadFile(pipe.reader, bytes, available, &read, nullptr));
    REQUIRE(std::string_view(bytes, read) == "myb: failure (diagnostic unavailable)\n");
    TestHooks::fail_os_next(FaultPoint::OsWrite, ERROR_ACCESS_DENIED);
    report_minimal_failure(io);
    REQUIRE(PeekNamedPipe(pipe.reader, nullptr, 0, nullptr, &available, nullptr));
    REQUIRE(available == 0);
    TestHooks::fail_next(FaultPoint::OsWrite);
    report_minimal_failure(io);
    TestHooks::clear_fault();
    REQUIRE(PeekNamedPipe(pipe.reader, nullptr, 0, nullptr, &available, nullptr));
    REQUIRE(available == 0);
    REQUIRE(first.header().domain == FailureDomain::Runtime);
    REQUIRE(first.header().reason == ReasonCode::Type);
    REQUIRE(first.header().os_code == 17);
}
