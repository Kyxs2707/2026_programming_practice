#include "cli/session.hpp"
#include "bytecode/compiler.hpp"
#include "diagnostic/diagnostic.hpp"
#include "frontend/driver.hpp"
#include "runtime/decimal_codec.hpp"
#include "semantic/checker.hpp"
#include "vm/vm.hpp"
#include <new>
#include <stdexcept>
namespace myb {
Session::Session(EmergencyFailure &first, HostIO &io, const BuiltinRegistry &builtins)
    : first_(first), io_(io), builtins_(builtins), sources_(first), program_(roots_),
      heap_(roots_, first) {}
Session::~Session() noexcept {
    program_.detach_roots();
    heap_.clear(); // 对象先释放，此时其借用的元数据、结构描述与源码记录仍存活。
}
const CompiledProgram &Session::compile_entry(std::string_view path) {
    DecimalCodec::initialize(first_);
    auto entry = io_.read_entry(path);
    auto source = sources_.add(std::move(entry.path_utf8), std::move(entry.bytes));
    Driver driver(sources_, source);
    auto ast = driver.parse();
    Checker checker(first_);
    auto checked = checker.check(*ast);
    BytecodeCompiler compiler(heap_, roots_, first_);
    compiler.compile(checked, program_);
    return program_;
}
int Session::run(const Options &options) noexcept {
    try {
        if (options.mode == RunMode::Help || options.mode == RunMode::Version) {
            try {
                io_.write_stdout(options.mode == RunMode::Help ? help_text : version_text, false);
            } catch (const IoFailure &failure) {
                // 帮助与版本属于宿主命令，其输出失败使用宿主故障退出状态。
                first_.record_first({FailureDomain::Internal, failure.reason(), {}, failure.os_code()});
                report(first_.header(), failure.what());
                return exit_code(first_.header());
            }
            return 0;
        }
        const auto &program = compile_entry(options.file);
        if (options.mode == RunMode::Check)
            return 0;
        VM vm(heap_, roots_, io_, first_, builtins_, &call_trace_);
        vm.execute(program);
        return 0;
    } catch (const Failure &failure) {
        first_.record_first(failure.header());
        if (first_.header().domain == failure.header().domain &&
            first_.header().reason == failure.header().reason)
            first_.complete_location(failure.header().span);
        report(first_.header(), failure.message());
    } catch (const std::bad_alloc &) {
        first_.record_first({FailureDomain::Resource, ReasonCode::Allocation});
        report(first_.header(), "host allocation failed");
    } catch (const std::length_error &) {
        first_.record_first({FailureDomain::Resource, ReasonCode::Capacity});
        report(first_.header(), "host capacity exceeded");
    } catch (...) {
        first_.record_first({FailureDomain::Internal, ReasonCode::Invariant});
        report(first_.header(), "unexpected host implementation failure");
    }
    return exit_code(first_.header());
}
void Session::report(const FailureHeader &header, std::string_view message) noexcept {
    try {
        const auto diagnostic = make_diagnostic(header, message, sources_, call_trace_);
        report_failure(diagnostic, io_);
    } catch (...) {
        report_minimal_failure(io_);
    }
}
} // namespace myb
