#include "builtins/registry.hpp"
#include "builtins/functions.hpp"
namespace myb {
namespace {
constexpr BuiltinDescriptor descriptors[] = {
    {"print", 1, builtin_print}, {"write", 1, builtin_write}, {"len", 1, builtin_len},
    {"append", 2, builtin_append}, {"insert", 3, builtin_insert}, {"pop", 1, builtin_pop},
    {"remove_at", 2, builtin_remove_at}, {"deep_copy", 1, builtin_deep_copy},
    {"read_line", 0, builtin_read_line}, {"int", 1, builtin_int}, {"float", 1, builtin_float},
    {"str", 1, builtin_str}, {"try_int", 1, builtin_try_int},
    {"try_float", 1, builtin_try_float}, {"assert", 2, builtin_assert},
    {"read_text", 1, builtin_read_text}, {"write_text", 2, builtin_write_text},
    {"split", 2, builtin_split}, {"join", 2, builtin_join}, {"trim", 1, builtin_trim},
    {"split_lines", 1, builtin_split_lines}};
}
std::span<const BuiltinDescriptor> builtin_descriptors() noexcept {
    return descriptors;
}
void invoke_builtin(const BuiltinDescriptor &descriptor, NativeContext &context,
                    std::span<const Value> args, RootedValue &out) {
    const ErrorSite site{context.failure, context.span};
    if (descriptor.arity != args.size() || !descriptor.invoke || !out.belongs_to(context.roots))
        fail(site, FailureDomain::Internal, ReasonCode::Invariant, "invalid builtin call protocol");
#ifndef NDEBUG
    out.begin_output();
#endif
    descriptor.invoke(context, args, out);
#ifndef NDEBUG
    if (!out.was_written())
        fail(site, FailureDomain::Internal, ReasonCode::Invariant,
             "builtin callback did not write its result");
#endif
}
const BuiltinDescriptor *BuiltinRegistry::find(std::string_view name) const noexcept {
    for (const auto &descriptor : table_)
        if (descriptor.name == name)
            return &descriptor;
    return nullptr;
}
const BuiltinDescriptor &BuiltinRegistry::precheck(std::string_view name, std::uint32_t count,
                                                   ErrorSite site) const {
    auto descriptor = find(name);
    if (!descriptor)
        fail(site, FailureDomain::Runtime, ReasonCode::Name, "unknown call target");
    if (descriptor->arity != count)
        fail(site, FailureDomain::Runtime, ReasonCode::Type, "wrong number of arguments");
    return *descriptor;
}
} // namespace myb
