#include "test_hooks.hpp"
#include "vm/vm.hpp"
#include <optional>

namespace myb::TestHooks {
namespace {
bool forced_gc = false;
std::optional<FaultPoint> next_failure;
std::size_t remaining_hits{};
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
    fail_next(point, 1);
}
void fail_next(FaultPoint point, std::size_t occurrence) noexcept {
    next_failure = point;
    remaining_hits = occurrence ? occurrence : 1;
}
void clear_fault() noexcept {
    next_failure.reset();
    remaining_hits = 0;
    next_os_failure.reset();
}
void hit(FaultPoint point) {
    if (next_failure == point) {
        if (remaining_hits > 1) {
            --remaining_hits;
            return;
        }
        next_failure.reset();
        remaining_hits = 0;
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
