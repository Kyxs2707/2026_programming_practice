#include "fixtures.hpp"
#include "builtins/functions.hpp"
#include "runtime/sequence_ops.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
#include <iostream>
using namespace myb;

MYB_TEST(vm_objects) {
    VmFixture f;
    auto r = f.run("struct P{a:Int64;b:Int64;}let x=P{b:write(\"B\"),a:write(\"A\")};");
    REQUIRE(r.exit_code == 1 && r.stdout_bytes == "B" && r.failure->reason == ReasonCode::Type);
    r = f.run("struct P{a:Int64;b:Int64;}let x=P{bad:write(\"X\")};");
    REQUIRE(r.exit_code == 1 && r.stdout_bytes.empty() && r.failure->reason == ReasonCode::Type);
    r = f.check("struct P{a:Int64;b:Int64;}let x=P{bad:write(\"X\")};");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes.empty() && r.native_calls == 0);
    r = f.run("let a=[[1]];let b=a[:];append(b[0],2);print(a[0][1]);print(a==b);");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "2\nfalse\n");
    r = f.run("print(\"北邮😀\"[-1]);");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "😀\n");
    r = f.run("1[write(\"I\")]=write(\"R\");");
    REQUIRE(r.exit_code == 1 && r.stdout_bytes == "RI" && r.failure->reason == ReasonCode::Type);
    r = f.run("print(1[write(\"S\"):write(\"E\")]);");
    REQUIRE(r.exit_code == 1 && r.stdout_bytes == "SE" && r.failure->reason == ReasonCode::Type);
    r = f.run("fn make(){return [];}struct Z{}print(make()==make());print(Z{}==Z{});");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "false\nfalse\n");
    r = f.run("struct P{x:Float64;next:P?;data:Array;}let p=P{data:[1,true,\"x\"],"
              "next:null,x:2};print(p.x);p.x=3;p.next=p;print(p.x);print(p.next==p);");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "2.0\n3.0\ntrue\n");
    r = f.run("fn change(x){x[0]=7;x=[];append(x,9);}let a=[1];change(a);print(a[0]);"
              "print(len(a));");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "7\n1\n");
}

MYB_TEST(vm_objects_gc) {
    TestHooks::force_gc(true);
    VmFixture f;
    auto r = f.run("struct P{data:Array;}fn make(){return P{data:[[\"北\"+\"邮\"]]};}"
                   "let p=make();let b=p.data[:];let removed=pop(b);append(removed,\"😀\");"
                   "print(p.data[0][0]);print(remove_at(removed,1));"
                   "print((\"北邮😀\"[1:])[1]);");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "北邮\n😀\n😀\n");
    r = f.run("let a=[[7]];fn index(){a=[];return 0;}print(a[index()][0]);"
              "let b=[9];fn end(){b=[];return 1;}print(b[:end()][0]);");
    TestHooks::force_gc(false);
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "7\n9\n");
    TestHooks::fail_next(FaultPoint::HeapObject);
    r = f.run("let a=[];print(\"unreachable\");");
    TestHooks::clear_fault();
    REQUIRE(r.exit_code == 3 && r.stdout_bytes.empty());
}

MYB_TEST(object_result_roots) {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap(roots, first);
    WindowsHostIO io(first);
    NativeContext context{heap, roots, io, first, {}};
    const ErrorSite site{first, {}};
    RootedValue source(roots), child(roots), result(roots, Value{std::int64_t{42}});
    heap.make_array(1, source);
    heap.make_array(0, child);
    std::get<ArrayRef>(source.get().data).ptr->elements.push_back(child.get());
    child.set(Value{NullValue{}});
    TestHooks::force_gc(true);
    SequenceOps(heap, roots).get_slice(source.get(), {}, {}, site, result);
    REQUIRE(TestHooks::heap_counts(heap).live == 3);
    REQUIRE(std::get<ArrayRef>(result.get().data).ptr != std::get<ArrayRef>(source.get().data).ptr);
    Value args[] = {source.get()};
    builtin_pop(context, args, result);
    source.set(Value{NullValue{}});
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == 1);
    REQUIRE(std::holds_alternative<ArrayRef>(result.get().data));
    heap.make_array(0, source);
    TestHooks::fail_next(FaultPoint::HeapObject);
    const auto retained = std::get<ArrayRef>(result.get().data).ptr;
    bool failed = false;
    try {
        SequenceOps(heap, roots).get_slice(source.get(), {}, {}, site, result);
    } catch (const Failure &failure) {
        failed = failure.header().domain == FailureDomain::Resource;
    }
    TestHooks::force_gc(false);
    TestHooks::clear_fault();
    REQUIRE(failed && std::get<ArrayRef>(result.get().data).ptr == retained);
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == 2);
}

MYB_TEST(array_iteration_mutation) {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap(roots, first);
    WindowsHostIO io(first);
    NativeContext context{heap, roots, io, first, {}};
    RootedValue source(roots), result(roots);
    heap.make_array(1, source);
    auto array = std::get<ArrayRef>(source.get().data);
    array.ptr->elements.push_back(Value{std::int64_t{1}});
    array.ptr->active_iteration_count = 1;
    const Value append_args[] = {source.get(), Value{std::int64_t{2}}};
    const Value insert_args[] = {source.get(), Value{std::int64_t{0}}, Value{NullValue{}}};
    const Value pop_args[] = {source.get()};
    const Value remove_args[] = {source.get(), Value{std::int64_t{0}}};
    struct Invocation {
        NativeFn fn;
        std::span<const Value> args;
    };
    const Invocation cases[] = {{builtin_append, append_args}, {builtin_insert, insert_args},
                                {builtin_pop, pop_args}, {builtin_remove_at, remove_args}};
    for (const auto &invocation : cases) {
        bool caught = false;
        try {
            invocation.fn(context, invocation.args, result);
        } catch (const MybRuntimeFailure &failure) {
            caught = failure.header().reason == ReasonCode::IterationMutation;
        }
        REQUIRE(caught && array.ptr->elements.size() == 1);
    }
    SequenceOps(heap, roots).set_index(source.get(), Value{std::int64_t{0}},
                                       Value{std::int64_t{7}}, {first, {}});
    REQUIRE(std::get<std::int64_t>(array.ptr->elements[0].data) == 7);
    array.ptr->active_iteration_count = 0;
}

MYB_TEST(array_append_growth) {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap(roots, first);
    WindowsHostIO io(first);
    NativeContext context{heap, roots, io, first, {}};
    RootedValue source(roots), child(roots), result(roots);
    heap.make_array(0, source);
    heap.make_array(0, child);
    const auto shared = std::get<ArrayRef>(child.get().data);
    const auto array = std::get<ArrayRef>(source.get().data);
    constexpr std::size_t count = 4096;
    std::size_t capacity_changes = 0;
    TestHooks::force_gc(true);
    for (std::size_t i = 0; i < count; ++i) {
        const auto capacity = array.ptr->elements.capacity();
        const Value item = i % 257 == 0 ? Value{shared} : Value{static_cast<std::int64_t>(i)};
        const Value args[] = {source.get(), item};
        builtin_append(context, args, result);
        capacity_changes += array.ptr->elements.capacity() != capacity;
        REQUIRE(std::holds_alternative<NullValue>(result.get().data));
        child.set(Value{NullValue{}}); // 首次追加后仅由原数组保活共享对象。
    }
    TestHooks::force_gc(false);
    REQUIRE(array.ptr->elements.size() == count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = array.ptr->elements[i];
        if (i % 257 == 0)
            REQUIRE(std::get<ArrayRef>(value.data).ptr == shared.ptr);
        else
            REQUIRE(std::get<std::int64_t>(value.data) == static_cast<std::int64_t>(i));
    }
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == 2);
    std::cout << "array capacity changes: " << capacity_changes << " for " << count
              << " appends\n";
    // 允许增长策略调整，数千次追加只应产生少量扩容，避免逐元素重新搬移。
    REQUIRE(capacity_changes < 64);
}

MYB_TEST(array_append_growth_failure) {
    EmergencyFailure first;
    RootRegistry roots;
    Heap heap(roots, first);
    WindowsHostIO io(first);
    NativeContext context{heap, roots, io, first, {}};
    RootedValue source(roots), child(roots), pending(roots), result(roots);
    heap.make_array(1, source);
    heap.make_array(0, child);
    const auto original = std::get<ArrayRef>(child.get().data);
    const auto array = std::get<ArrayRef>(source.get().data);
    const Value first_args[] = {source.get(), child.get()};
    builtin_append(context, first_args, result);
    while (array.ptr->elements.size() < array.ptr->elements.capacity()) {
        const Value args[] = {source.get(), Value{std::int64_t{7}}};
        builtin_append(context, args, result);
    }
    const auto size = array.ptr->elements.size(), capacity = array.ptr->elements.capacity();
    heap.make_array(0, pending);
    result.set(Value{std::int64_t{42}});
    TestHooks::force_gc(true);
    TestHooks::fail_next(FaultPoint::Payload);
    bool failed = false;
    try {
        const Value args[] = {source.get(), pending.get()};
        builtin_append(context, args, result);
    } catch (const Failure &failure) {
        failed = failure.header().domain == FailureDomain::Resource &&
                 failure.header().reason == ReasonCode::Allocation;
    }
    TestHooks::force_gc(false);
    TestHooks::clear_fault();
    REQUIRE(failed && array.ptr->elements.size() == size);
    REQUIRE(array.ptr->elements.capacity() == capacity);
    REQUIRE(std::get<ArrayRef>(array.ptr->elements[0].data).ptr == original.ptr);
    for (std::size_t i = 1; i < size; ++i)
        REQUIRE(std::get<std::int64_t>(array.ptr->elements[i].data) == 7);
    REQUIRE(std::get<std::int64_t>(result.get().data) == 42);
    child.set(Value{NullValue{}});
    pending.set(Value{NullValue{}});
    heap.collect();
    REQUIRE(TestHooks::heap_counts(heap).live == 2);
}
