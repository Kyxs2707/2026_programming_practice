#include "vm/vm.hpp"
#include "runtime/text_builder.hpp"
#include "source/utf8.hpp"
#include <algorithm>
#include <limits>

namespace myb {
void VM::ensure_iterations(std::size_t required, ErrorSite site) {
    const auto capacity = iterations_.capacity(), limit = iterations_.max_size();
    if (required <= capacity)
        return;
    if (required > limit)
        fail(site, FailureDomain::Resource, ReasonCode::Capacity,
             "iteration record capacity exceeded");
    const auto grown = capacity > limit - capacity / 2 ? limit : capacity + capacity / 2;
    try {
#ifdef MYB_TEST_HOOKS
        TestHooks::hit(FaultPoint::IterationRecords);
#endif
        iterations_.reserve(std::min(limit, std::max({required, grown, std::size_t{16}})));
    } catch (const std::bad_alloc &) {
        first_.record_first({FailureDomain::Resource, ReasonCode::Allocation, site.span});
        throw;
    }
}
void VM::begin_iteration(IterationSiteId id) {
    auto &frame = frames_.back();
    const auto &unit = program_->units[frame.unit_id];
    const ErrorSite site{first_, unit.locations[frame.executing_ip]};
    if (!frame.sp || id >= unit.iterations.size())
        invalid();
    const auto top = frame.operand_base + frame.sp - 1;
    const auto source = store_.value(top);
    const auto array = std::get_if<ArrayRef>(&source.data);
    if (!array && !std::holds_alternative<StringRef>(source.data))
        fail(site, FailureDomain::Runtime, ReasonCode::Type,
             "iteration source must be Array or String");
    if (array && array->ptr->active_iteration_count == std::numeric_limits<std::size_t>::max())
        fail(site, FailureDomain::Resource, ReasonCode::Capacity,
             "active array iteration count exceeded");
    if (iterations_.size() == iterations_.max_size())
        fail(site, FailureDomain::Resource, ReasonCode::Capacity,
             "iteration record capacity exceeded");
    ensure_iterations(iterations_.size() + 1, site);
    // 来源仍由原操作数保护；容量就绪后发布记录、接管计数并消费来源，期间无分配。
    iterations_.push_back({frames_.size() - 1, id, source, 0, array != nullptr});
    if (array)
        ++array->ptr->active_iteration_count;
    store_.clear(top, top + 1);
    --frame.sp;
}
bool VM::next_iteration(IterationSiteId id) {
    auto &frame = frames_.back();
    const auto &unit = program_->units[frame.unit_id];
    const ErrorSite site{first_, unit.locations[frame.executing_ip]};
    if (iterations_.empty() || iterations_.back().frame_index != frames_.size() - 1 ||
        iterations_.back().site != id || id >= unit.iterations.size())
        invalid();
    auto &record = iterations_.back();
    const auto slot = unit.iterations[id].slot;
    if (slot >= unit.local_count || store_.state(frame.base + slot) != BindingState::Dormant)
        invalid();
    RootedValue result(roots_);
    std::size_t next;
    if (const auto array = std::get_if<ArrayRef>(&record.source.data)) {
        if (record.cursor >= array->ptr->elements.size())
            return false;
        result.set(array->ptr->elements[record.cursor]);
        next = record.cursor + 1;
    } else if (const auto string = std::get_if<StringRef>(&record.source.data)) {
        const auto &text = string->ptr->utf8;
        if (record.cursor == text.size())
            return false;
        const auto decoded = utf8::decode_one(text, record.cursor);
        if (!decoded)
            fail(site, FailureDomain::Internal, ReasonCode::Invariant, "invalid String payload");
        next = record.cursor + decoded->width;
        TextBuilder builder(heap_);
        builder.append({text.data() + record.cursor, decoded->width});
        // 分配期间来源仍属于记录根；结果发布到本轮局部之前由独立输出根保护。
        heap_.make_string(std::move(builder), result);
    } else {
        invalid();
    }
    store_.value(frame.base + slot) = result.get();
    store_.state(frame.base + slot) = BindingState::Initialized;
    record.cursor = next;
    return true;
}
void VM::end_iteration(IterationSiteId id) noexcept {
    if (iterations_.empty() || frames_.empty() ||
        iterations_.back().frame_index != frames_.size() - 1 || iterations_.back().site != id) {
        first_.record_first({FailureDomain::Internal, ReasonCode::Invariant, {}});
        return;
    }
    unwind_iterations(iterations_.size() - 1);
}
void VM::unwind_iterations(std::size_t base) noexcept {
    while (iterations_.size() > base) {
        const auto &record = iterations_.back();
        if (record.registered) {
            auto array = std::get_if<ArrayRef>(&record.source.data);
            if (array && array->ptr->active_iteration_count)
                --array->ptr->active_iteration_count;
            else
                first_.record_first({FailureDomain::Internal, ReasonCode::Invariant, {}});
        }
        iterations_.pop_back();
    }
}
} // namespace myb
