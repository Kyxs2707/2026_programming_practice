#include "test_hooks.hpp"
#include "vm/vm.hpp"
#include <optional>

namespace myb::TestHooks {
namespace {
bool forced_gc = false;
std::optional<FaultPoint> next_failure;
std::optional<FaultPoint> next_os_failure;
std::uint32_t next_os_code{};
} // namespace
void force_gc(bool enabled) noexcept {
    forced_gc = enabled;
}
bool gc_forced() noexcept {
    return forced_gc;
}
void fail_next(FaultPoint point) noexcept {
    next_failure = point;
}
void clear_fault() noexcept {
    next_failure.reset();
    next_os_failure.reset();
}
void hit(FaultPoint point) {
    if (next_failure == point) {
        next_failure.reset();
        throw std::bad_alloc{};
    }
}
void fail_os_next(FaultPoint point, std::uint32_t code) noexcept {
    next_os_failure = point;
    next_os_code = code;
}
std::optional<std::uint32_t> take_os_failure(FaultPoint point) noexcept {
    if (next_os_failure != point)
        return {};
    next_os_failure.reset();
    return next_os_code;
}
std::size_t active_iteration_count(const VM &vm) noexcept {
    return vm.iterations_.size();
}
} // namespace myb::TestHooks
