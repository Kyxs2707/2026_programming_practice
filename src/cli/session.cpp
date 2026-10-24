#include "cli/session.hpp"
#include "bytecode/compiler.hpp"
#include "frontend/driver.hpp"
#include "runtime/decimal_codec.hpp"
#include "semantic/checker.hpp"
#include "vm/vm.hpp"
#include <new>
#include <stdexcept>
namespace myb {
namespace {
/** @brief 转义诊断中的控制字节，保持路径和原因文本位于同一诊断行。 */
std::string escaped(std::string_view text) {
    std::string out;
    for (unsigned char c : text) {
        if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c < 32 || c == 127) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out;
}
} // namespace
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
            } catch (...) {
                // 帮助与版本属于宿主命令，其输出失败使用宿主故障退出状态。
                FailureHeader h{FailureDomain::Internal, ReasonCode::IO};
                report(h, "cannot write command output");
                return 3;
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
        auto header = first_.header();
        // 保留首错的 domain、reason 和 OS 信息，只从同类捕获异常补齐缺失的 span。
        if (!header.span && header.domain == failure.header().domain &&
            header.reason == failure.header().reason)
            header.span = failure.header().span;
        report(header, failure.what());
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
        std::string text;
        if (header.span) {
            const auto &file = sources_.get(header.span->source);
            auto position = sources_.locate(header.span->source, header.span->begin);
            text = escaped(file.path_utf8) + ':' + std::to_string(position.line) + ':' +
                   std::to_string(position.column) + ": ";
        } else
            text = "myb: ";
        text += domain_name(header.domain);
        text += " [";
        text += reason_name(header.reason);
        text += "]: ";
        text += escaped(message);
        if (header.os_code) {
            text += " (OS ";
            text += std::to_string(*header.os_code);
            text += ')';
        }
        text += '\n';
        for (const auto &entry : call_trace_) {
            const auto &source = sources_.get(entry.span.source);
            const auto position = sources_.locate(entry.span.source, entry.span.begin);
            text += "  at " + escaped(entry.name) + " (" + escaped(source.path_utf8) + ':' +
                    std::to_string(position.line) + ':' + std::to_string(position.column) + ")\n";
        }
        // stderr 失败后停止报告，避免诊断通路再次调用报告器。
        try {
            io_.write_stderr(text);
        } catch (...) {
        }
    } catch (...) {
        try {
            io_.write_stderr("myb: failure (diagnostic unavailable)\n");
        } catch (...) {
        }
    }
}
} // namespace myb
