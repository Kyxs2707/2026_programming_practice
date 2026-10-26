#include "cli/session.hpp"
#include "runtime/text_builder.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"

using namespace myb;
namespace {
class NativeTestIO final : public HostIO {
  public:
    explicit NativeTestIO(EmergencyFailure &first) : first(first) {}
    EmergencyFailure &first;
    std::string source, out, err;
    bool fail_output{};
    EntryBytes read_entry(std::string_view) override {
        return {"native.myb", source};
    }
    LineStatus read_line(Utf8Sink &) override {
        throw std::logic_error("unexpected stdin read");
    }
    void read_text(std::string_view, Utf8Sink &) override {
        throw std::logic_error("unexpected file read");
    }
    void write_text(std::string_view, std::string_view) override {
        throw std::logic_error("unexpected file write");
    }
    void write_stdout(std::string_view s, bool lf) override {
        if (fail_output)
            fail({first, {}}, FailureDomain::Runtime, ReasonCode::IO, "output failed");
        out += s;
        if (lf)
            out += '\n';
    }
    void write_stderr(std::string_view s) override {
        err += s;
    }
};
void make_text(NativeContext &ctx, std::span<const Value>, RootedValue &out) {
    TextBuilder builder(ctx.heap);
    builder.append(std::span<const char>("kept", 4));
    ctx.heap.make_string(std::move(builder), out);
    ctx.heap.collect(); // result out must already be a root before this collection
}
void echo(NativeContext &ctx, std::span<const Value> args, RootedValue &out) {
    // No Program constant or native result root owns this value now: only the VM argument region.
    ctx.heap.collect();
    REQUIRE(TestHooks::heap_counts(ctx.heap).live == 1);
    out.set(args[0]);
}
} // namespace
MYB_TEST(native_root_handoff) {
    EmergencyFailure first;
    NativeTestIO io(first);
    io.source = "print(echo(made()));";
    const BuiltinDescriptor table[] = {
        builtin_descriptors()[0], {"made", 0, make_text}, {"echo", 1, echo}};
    BuiltinRegistry builtins(table);
    Session session(first, io, builtins);
    TestHooks::force_gc(true);
    const auto status = session.run({RunMode::Run, "native.myb"});
    TestHooks::force_gc(false);
    REQUIRE(status == 0);
    REQUIRE(io.out == "kept\n" && io.err.empty());
}
MYB_TEST(native_failure_location) {
    EmergencyFailure first;
    NativeTestIO io(first);
    io.source = "\nprint(1);";
    io.fail_output = true;
    BuiltinRegistry builtins;
    Session session(first, io, builtins);
    REQUIRE(session.run({RunMode::Run, "native.myb"}) == 1);
    REQUIRE(io.err.starts_with("native.myb:2:1: runtime error [IO]:"));
    REQUIRE(io.err.find("call chain") == std::string::npos);
}

#ifndef NDEBUG
MYB_TEST(native_requires_result) {
    EmergencyFailure first;
    NativeTestIO io(first);
    io.source = "missing_result();";
    const BuiltinDescriptor descriptors[] = {
        {"missing_result", 0, [](NativeContext &, std::span<const Value>, RootedValue &) {}}};
    BuiltinRegistry registry(descriptors);
    Session session(first, io, registry);
    REQUIRE(session.run({RunMode::Run, "native.myb"}) == 3);
    REQUIRE(first.header().reason == ReasonCode::Invariant);
}
#endif
