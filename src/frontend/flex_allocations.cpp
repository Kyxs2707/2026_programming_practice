#include "frontend/flex_allocations.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#ifdef MYB_TEST_HOOKS
#include "test_hooks.hpp"
#endif

namespace myb {
void *FlexAllocationLedger::allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() - sizeof(Block))
        fail({failure_, {}}, FailureDomain::Resource, ReasonCode::Capacity,
             "scanner allocation exceeds capacity");
#ifdef MYB_TEST_HOOKS
    try {
        TestHooks::hit(FaultPoint::FlexAllocation);
    } catch (const std::bad_alloc &) {
        fail({failure_, {}}, FailureDomain::Resource, ReasonCode::Allocation,
             "scanner allocation failed");
    }
#endif
    auto *b = static_cast<Block *>(std::malloc(sizeof(Block) + n));
    if (!b)
        fail({failure_, {}}, FailureDomain::Resource, ReasonCode::Allocation,
             "scanner allocation failed");
    // 分配成功后无分配地接入登记链，使后端尚未接管的块也能被异常清理找到。
    *b = {nullptr, head_, n};
    if (head_)
        head_->prev = b;
    head_ = b;
    ++live_;
    return b + 1;
}

void FlexAllocationLedger::release(void *p) noexcept {
    if (!p)
        return;
    auto *b = static_cast<Block *>(p) - 1;
    // 先摘除前后链接再释放块，后续 clear 不会再沿已释放的块读取链接。
    if (b->prev)
        b->prev->next = b->next;
    else
        head_ = b->next;
    if (b->next)
        b->next->prev = b->prev;
    std::free(b);
    --live_;
}

void *FlexAllocationLedger::reallocate(void *p, std::size_t n) {
    if (!p)
        return allocate(n);
    auto *old = static_cast<Block *>(p) - 1;
    // 新块分配失败时仍保留旧块；成功登记并复制有效前缀后才撤销旧块。
    auto *fresh = allocate(n);
    std::memcpy(fresh, p, std::min(old->size, n));
    release(p);
    return fresh;
}
} // namespace myb
