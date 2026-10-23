#include "vm/globals.hpp"
namespace myb {
void Globals::declare(NameId name, ErrorSite site) {
    if (!bindings_.try_emplace(name).second)
        fail(site, FailureDomain::Internal, ReasonCode::Invariant, "global binding already exists");
}
Value Globals::load(NameId name, ErrorSite site) const {
    const auto found = bindings_.find(name);
    if (found == bindings_.end())
        fail(site, FailureDomain::Runtime, ReasonCode::Name, "undeclared variable");
    if (found->second.state != BindingState::Initialized)
        fail(site, FailureDomain::Runtime, ReasonCode::Uninitialized, "uninitialized variable");
    return found->second.value;
}
void Globals::store(NameId name, Value value, ErrorSite site) {
    const auto found = bindings_.find(name);
    if (found == bindings_.end())
        fail(site, FailureDomain::Runtime, ReasonCode::Name, "undeclared variable");
    found->second.value = value;
    found->second.state = BindingState::Initialized;
}
void Globals::trace(void *self, Marker &marker) {
    for (const auto &[name, binding] : static_cast<Globals *>(self)->bindings_) {
        (void)name;
        if (binding.state == BindingState::Initialized)
            marker.mark(binding.value);
    }
}
} // namespace myb
