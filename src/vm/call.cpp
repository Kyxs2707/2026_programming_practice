#include "vm/call.hpp"
#include "diagnostic/failure.hpp"
namespace myb {
std::size_t checked_region_end(std::size_t left, std::size_t right, std::size_t limit,
                               EmergencyFailure &first) {
    if (left > limit || right > limit - left)
        fail({first, {}}, FailureDomain::Resource, ReasonCode::Capacity,
             "VM region capacity exceeded");
    return left + right;
}
} // namespace myb
