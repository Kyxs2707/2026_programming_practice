#include "vm/value_store.hpp"
#include <algorithm>
#include <new>
#ifdef MYB_TEST_HOOKS
#include "test_hooks.hpp"
#endif
namespace myb {
void ValueStore::ensure(std::size_t required, ErrorSite site) {
    if (required <= size())
        return;
    const auto limit = std::min(values_.max_size(), states_.max_size());
    if (required > limit)
        fail(site, FailureDomain::Resource, ReasonCode::Capacity, "VM value region is too large");
    const auto old = size();
    const auto grown = old > limit - old / 2 ? limit : old + old / 2;
    const auto target = std::min(limit, std::max({required, grown, std::size_t{64}}));
    try {
#ifdef MYB_TEST_HOOKS
        TestHooks::hit(FaultPoint::VmValues);
#endif
        values_.reserve(target);
#ifdef MYB_TEST_HOOKS
        TestHooks::hit(FaultPoint::VmStates);
#endif
        states_.reserve(target);
        // reserve 后两次 resize 仅执行无异常的 Value/枚举初始化。
        values_.resize(target);
        states_.resize(target, BindingState::Dormant);
    } catch (const std::bad_alloc &) {
        site.failure.record_first({FailureDomain::Resource, ReasonCode::Allocation, site.span});
        throw;
    }
}
void ValueStore::clear(std::size_t begin, std::size_t end) noexcept {
    for (auto i = begin; i < end; ++i) {
        values_[i] = Value{NullValue{}};
        states_[i] = BindingState::Dormant;
    }
}
} // namespace myb
