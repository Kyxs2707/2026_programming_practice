#include "fixtures.hpp"
#include "cli/session.hpp"
namespace myb {
namespace {
class MemoryHostIO final : public HostIO {
  public:
    std::string source, out, err;
    std::size_t reads{}, calls{};
    EntryBytes read_entry(std::string_view path) override {
        return {std::string(path), source};
    }
    LineStatus read_line(Utf8Sink &) override {
        ++reads;
        return LineStatus::End;
    }
    void read_text(std::string_view, Utf8Sink &) override {
        throw std::logic_error("unexpected read_text");
    }
    void write_text(std::string_view, std::string_view) override {
        throw std::logic_error("unexpected write_text");
    }
    void write_stdout(std::string_view text, bool newline) override {
        out += text;
        if (newline)
            out += '\n';
    }
    void write_stderr(std::string_view text) override {
        err += text;
    }
};
void counted_print(NativeContext &ctx, std::span<const Value> args, RootedValue &result) {
    ++static_cast<MemoryHostIO &>(ctx.io).calls;
    builtin_descriptors()[0].invoke(ctx, args, result);
}
void counted_write(NativeContext &ctx, std::span<const Value> args, RootedValue &result) {
    ++static_cast<MemoryHostIO &>(ctx.io).calls;
    builtin_descriptors()[1].invoke(ctx, args, result);
}
constexpr BuiltinDescriptor counted[] = {{"print", 1, counted_print}, {"write", 1, counted_write}};
} // namespace
struct VmFixture::State {
    EmergencyFailure first;
    MemoryHostIO io;
    BuiltinRegistry registry{counted};
    Session session{first, io, registry};
};
VmFixture::VmFixture() = default;
VmFixture::~VmFixture() = default;
const CompiledProgram &VmFixture::compile(std::string_view source) {
    state_ = std::make_unique<State>();
    state_->io.source = source;
    return state_->session.compile_entry("fixture.myb");
}
TestRun VmFixture::execute(std::string_view source, bool check) {
    state_ = std::make_unique<State>();
    state_->io.source = source;
    TestRun result;
    result.exit_code = state_->session.run({check ? RunMode::Check : RunMode::Run, "fixture.myb"});
    result.stdout_bytes = state_->io.out;
    result.stderr_bytes = state_->io.err;
    if (state_->first.is_set())
        result.failure = state_->first.header();
    result.stdin_reads = state_->io.reads;
    result.native_calls = state_->io.calls;
    for (const auto &entry : state_->session.call_trace())
        result.call_names.push_back(entry.name);
    return result;
}
TestRun VmFixture::run(std::string_view source) {
    return execute(source, false);
}
TestRun VmFixture::check(std::string_view source) {
    return execute(source, true);
}
} // namespace myb
