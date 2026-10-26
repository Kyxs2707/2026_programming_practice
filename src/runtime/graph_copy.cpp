#include "runtime/graph_copy.hpp"
#include "runtime/heap.hpp"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace myb {
namespace {
/** @brief 保存一个已登记副本及其源对象；对象槽由完整根工作区保活。 */
struct CopyWork {
    Value source;
    Value target;
};
/**
 * @brief 保存原对象到唯一副本的映射与迭代工作队列。
 * 根回调在每次 GC 时重新枚举当前容器，不保存可增长容器内元素的地址。
 */
struct CopyWorkspace {
    std::unordered_map<HeapObject *, Value> memo;
    std::vector<CopyWork> work;

    /** @brief 标记映射键、全部副本及待处理工作项，不分配受管对象。 */
    static void trace(void *context, Marker &marker) {
        auto &self = *static_cast<CopyWorkspace *>(context);
        for (const auto &[source, target] : self.memo) {
            if (source->kind == ObjectKind::Array)
                marker.mark(Value{ArrayRef{static_cast<ArrayObject *>(source)}});
            else
                marker.mark(Value{StructRef{static_cast<StructObject *>(source)}});
            marker.mark(target);
        }
        for (const auto &item : self.work) {
            marker.mark(item.source);
            marker.mark(item.target);
        }
    }

    /** @brief 先登记映射，再登记待处理项；两步间仅发生宿主分配。 */
    void add(HeapObject *source, Value input, Value target) {
#ifdef MYB_TEST_HOOKS
        TestHooks::hit(FaultPoint::NativeWorkspace);
#endif
        memo.emplace(source, target);
        if (work.size() == work.capacity()) {
#ifdef MYB_TEST_HOOKS
            TestHooks::hit(FaultPoint::NativeWorkspace);
#endif
            const auto capacity = work.capacity();
            const auto extra = std::max<std::size_t>(capacity / 2, 1);
            const auto next = capacity > work.max_size() - extra ? work.max_size()
                                                                : capacity + extra;
            if (work.size() == work.max_size())
                throw std::length_error("deep copy workspace capacity exceeded");
            work.reserve(next);
        }
        work.push_back({input, target});
    }
};

/**
 * @brief 返回已有副本，或创建可扫描的空副本并先登记后排队。
 * @pre 输入及已创建副本由调用方根保护；返回值交接前不得触发 GC。
 */
Value copy_edge(Heap &heap, Value source, CopyWorkspace &workspace, RootedValue &fresh) {
    HeapObject *object = nullptr;
    if (auto array = std::get_if<ArrayRef>(&source.data))
        object = array->ptr;
    else if (auto structure = std::get_if<StructRef>(&source.data))
        object = structure->ptr;
    else
        return source;
    if (auto found = workspace.memo.find(object); found != workspace.memo.end())
        return found->second;
    if (object->kind == ObjectKind::Array)
        heap.make_array(static_cast<const ArrayObject *>(object)->elements.size(), fresh);
    else
        heap.make_struct(*static_cast<const StructObject *>(object)->schema, fresh);
    // fresh 跨宿主容器增长保护新对象；加入映射后工作区接替长期保活责任。
    const Value target = fresh.get();
    workspace.add(object, source, target);
    return target;
}
} // namespace

void deep_copy(Heap &heap, RootRegistry &roots, Value source, RootedValue &out) {
    CopyWorkspace workspace;
    NativeRootSet keep_workspace(roots, &workspace, CopyWorkspace::trace);
    RootedValue fresh(roots), result(roots);
    result.set(copy_edge(heap, source, workspace, fresh));
    for (std::size_t pending = 0; pending < workspace.work.size(); ++pending) {
        // 子边发现新对象时可扩容队列，故不跨 copy_edge 保存队列元素的引用。
        const CopyWork item = workspace.work[pending];
        if (auto source_array = std::get_if<ArrayRef>(&item.source.data)) {
            const auto target = std::get<ArrayRef>(item.target.data);
            for (std::size_t i = 0; i < source_array->ptr->elements.size(); ++i) {
                const Value child = source_array->ptr->elements[i];
                const Value copied = copy_edge(heap, child, workspace, fresh);
                // 创建数组时已预留全部元素容量，此处不进入 GC，也不扩容目标负载。
                target.ptr->elements.push_back(copied);
            }
        } else {
            const auto source_struct = std::get<StructRef>(item.source.data);
            const auto target = std::get<StructRef>(item.target.data);
            for (std::size_t i = 0; i < source_struct.ptr->fields.size(); ++i)
                target.ptr->fields[i] =
                    copy_edge(heap, source_struct.ptr->fields[i], workspace, fresh);
        }
    }
    out.set(result.get());
}
} // namespace myb
