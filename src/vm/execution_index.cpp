#include "vm/execution_index.hpp"
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>
#ifdef MYB_TEST_HOOKS
#include "test_hooks.hpp"
#endif

namespace myb {
namespace {
[[noreturn]] void invalid(ErrorSite site) {
    fail(site, FailureDomain::Internal, ReasonCode::Invariant, "invalid execution index metadata");
}
/** @brief 在会分配的操作前暴露独立故障位置；正式构建无此状态。 */
void allocating() {
#ifdef MYB_TEST_HOOKS
    TestHooks::hit(FaultPoint::ExecutionIndex);
#endif
}
/** @brief 将准备阶段的实际操作与执行阶段统计分开；异常退出也恢复归属。 */
struct Preparation {
    Preparation() noexcept {
#ifdef MYB_TEST_HOOKS
        TestHooks::preparing_execution_index(true);
#endif
    }
    ~Preparation() noexcept {
#ifdef MYB_TEST_HOOKS
        TestHooks::preparing_execution_index(false);
#endif
    }
};
void prepared_work() noexcept {
#ifdef MYB_TEST_HOOKS
    TestHooks::record_execution_work(TestHooks::ExecutionWork::FieldCandidate);
#endif
}
void bounded(std::size_t count, ErrorSite site) {
    if (count > std::numeric_limits<std::uint32_t>::max())
        fail(site, FailureDomain::Resource, ReasonCode::Capacity,
             "execution index exceeds uint32 capacity");
}
/** @brief 在准备期间任何下标访问前验证来源表；不检查 MYB 动态目标和值。 */
void validate(const CompiledProgram &program, ErrorSite site) {
    bounded(program.names.size(), site);
    bounded(program.units.size(), site);
    bounded(program.functions.size(), site);
    bounded(program.schemas.size(), site);
    for (const auto &function : program.functions) {
        if (function.name >= program.names.size() || function.unit >= program.units.size())
            invalid(site);
    }
    for (std::size_t i = 0; i < program.schemas.size(); ++i) {
        const auto &schema = program.schemas[i];
        if (!schema || schema->id != i)
            invalid(site);
        bounded(schema->fields.size(), site);
    }
    for (const auto &unit : program.units) {
        bounded(unit.constructions.size(), site);
        for (const auto &call : unit.calls)
            if (call.target >= program.names.size())
                invalid(site);
        for (const auto &plan : unit.constructions) {
            if (plan.type >= program.names.size())
                invalid(site);
            bounded(plan.fields.size(), site);
            for (const auto field : plan.fields)
                if (field >= program.names.size())
                    invalid(site);
        }
    }
}
} // namespace

bool ExecutionIndex::FieldEqual::operator()(const std::string &left,
                                            const std::string &right) const noexcept {
#ifdef MYB_TEST_HOOKS
    TestHooks::record_execution_work(TestHooks::ExecutionWork::FieldCandidate);
#endif
    return left == right;
}

void ExecutionIndex::prepare(const CompiledProgram &program, const BuiltinRegistry &builtins,
                             EmergencyFailure &first) {
    const ErrorSite site{first, {}};
    Preparation preparing;
    try {
        validate(program, site);
        ExecutionIndex next;
        next.program_ = &program;
        allocating();
        next.names_.resize(program.names.size());
        // 用户函数按精确 NameId 匹配；手造的同文本不同编号不能合并。
        for (std::size_t i = 0; i < program.functions.size(); ++i) {
            prepared_work();
            auto &target = next.names_[program.functions[i].name].call;
            if (target.kind == CallTargetKind::Missing)
                target = {CallTargetKind::User, static_cast<std::uint32_t>(i), nullptr};
        }
        std::unordered_map<std::string_view, SchemaId> schemas;
        for (const auto &schema : program.schemas) {
            prepared_work();
            allocating();
            schemas.try_emplace(schema->name, schema->id);
        }
        for (std::size_t i = 0; i < program.names.size(); ++i) {
            prepared_work();
            auto &resolved = next.names_[i];
            const auto &text = program.names[i];
            if (resolved.call.kind == CallTargetKind::Missing) {
                if (const auto descriptor = builtins.find(text))
                    resolved.call = {CallTargetKind::Builtin, 0, descriptor};
            }
            if (const auto found = schemas.find(text); found != schemas.end())
                resolved.schema = found->second;
            resolved.nonconstructible = resolved.call.kind != CallTargetKind::Missing ||
                                        text == "Bool" || text == "Int64" || text == "Float64" ||
                                        text == "String" || text == "Array";
        }
        allocating();
        next.fields_.resize(program.schemas.size());
        for (std::size_t i = 0; i < program.schemas.size(); ++i) {
            const auto &source = program.schemas[i]->fields;
            auto &fields = next.fields_[i];
            for (std::size_t slot = 0; slot < source.size(); ++slot) {
                prepared_work();
                allocating();
                if (!fields.emplace(source[slot].name, static_cast<std::uint32_t>(slot)).second)
                    invalid(site);
            }
        }
        allocating();
        next.constructions_.resize(program.units.size());
        for (std::size_t unit = 0; unit < program.units.size(); ++unit) {
            const auto &plans = program.units[unit].constructions;
            allocating();
            next.constructions_[unit].resize(plans.size());
            for (std::size_t p = 0; p < plans.size(); ++p) {
                const auto &plan = plans[p];
                const auto &type = next.names_[plan.type];
                auto &result = next.constructions_[unit][p];
                if (!type.schema) {
                    result.status = type.nonconstructible ? ConstructionStatus::NotConstructible
                                                          : ConstructionStatus::UnknownType;
                    continue;
                }
                result.schema = type.schema;
                result.status = ConstructionStatus::FieldSetMismatch;
                const auto &schema = *program.schemas[*type.schema];
                if (plan.fields.size() != schema.fields.size())
                    continue;
                allocating();
                std::vector<unsigned char> seen(schema.fields.size());
                allocating();
                result.slots.reserve(plan.fields.size());
                bool valid = true;
                for (const auto field : plan.fields) {
                    prepared_work();
                    const auto found = next.fields_[*type.schema].find(program.names[field]);
                    if (found == next.fields_[*type.schema].end() || seen[found->second]) {
                        valid = false;
                        break;
                    }
                    seen[found->second] = 1;
                    result.slots.push_back(found->second);
                }
                if (valid)
                    result.status = ConstructionStatus::Ready;
                else
                    result.slots.clear();
            }
        }
        swap(next);
    } catch (const std::bad_alloc &) {
        first.record_first({FailureDomain::Resource, ReasonCode::Allocation});
        throw;
    } catch (const std::length_error &) {
        first.record_first({FailureDomain::Resource, ReasonCode::Capacity});
        throw;
    }
}

const ExecutionIndex::NameResolution &ExecutionIndex::name(NameId id, ErrorSite site) const {
    if (!program_ || id >= names_.size())
        invalid(site);
    return names_[id];
}
const ResolvedCallTarget &ExecutionIndex::call(NameId id, ErrorSite site) const {
#ifdef MYB_TEST_HOOKS
    TestHooks::record_execution_work(TestHooks::ExecutionWork::CallCandidate);
#endif
    return name(id, site).call;
}
const StructSchema &ExecutionIndex::schema(NameId id, ErrorSite site) const {
    const auto &resolved = name(id, site);
#ifdef MYB_TEST_HOOKS
    TestHooks::record_execution_work(TestHooks::ExecutionWork::SchemaCandidate);
#endif
    if (resolved.schema)
        return *program_->schemas[*resolved.schema];
    fail(site, FailureDomain::Runtime,
         resolved.nonconstructible ? ReasonCode::Type : ReasonCode::Name,
         resolved.nonconstructible ? "name is not a constructible structure"
                                   : "unknown structure name");
}
std::size_t ExecutionIndex::field_slot(const StructSchema &schema, NameId id,
                                       ErrorSite site) const {
    (void)name(id, site);
    if (schema.id >= fields_.size() || program_->schemas[schema.id].get() != &schema)
        invalid(site);
#ifdef MYB_TEST_HOOKS
    TestHooks::record_execution_work(TestHooks::ExecutionWork::FieldCandidate);
#endif
    const auto &fields = fields_[schema.id];
    if (const auto found = fields.find(program_->names[id]); found != fields.end())
        return found->second;
    fail(site, FailureDomain::Runtime, ReasonCode::Name, "unknown structure field");
}
const PreparedConstruction &ExecutionIndex::construction(std::uint32_t unit, std::uint32_t plan,
                                                         ErrorSite site) const {
    if (!program_ || unit >= constructions_.size() || plan >= constructions_[unit].size())
        invalid(site);
#ifdef MYB_TEST_HOOKS
    TestHooks::record_execution_work(TestHooks::ExecutionWork::Construction);
#endif
    return constructions_[unit][plan];
}
void ExecutionIndex::swap(ExecutionIndex &other) noexcept {
    std::swap(program_, other.program_);
    names_.swap(other.names_);
    fields_.swap(other.fields_);
    constructions_.swap(other.constructions_);
}
void ExecutionIndex::clear() noexcept {
    constructions_.clear();
    fields_.clear();
    names_.clear();
    program_ = nullptr;
}
} // namespace myb
