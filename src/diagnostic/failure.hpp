#pragma once
#include "source/span.hpp"
#include <exception>
#include <optional>
#include <string>
#include <string_view>
namespace myb {
/** @brief 失败所属阶段，用于区分 MYB 错误、入口错误与宿主故障。 */
enum class FailureDomain { Usage, EntryIO, Frontend, Structure, Runtime, Resource, Internal };
/** @brief 失败的具体原因，供内部分类和诊断使用。 */
enum class ReasonCode {
    Encoding,
    Syntax,
    Declaration,
    Name,
    Uninitialized,
    Type,
    Range,
    Arithmetic,
    IterationMutation,
    IO,
    Assertion,
    Capacity,
    Allocation,
    Invariant
};
/** @brief 无动态分配的错误元数据，不持有源码或受管对象。 */
struct FailureHeader {
    FailureDomain domain;
    ReasonCode reason;
    std::optional<SourceSpan> span{};
    std::optional<std::uint32_t> os_code{};
};
/**
 * @brief 无分配地保存会话的第一个错误，后续记录保持首错不变。
 *
 * 详细消息构造或诊断输出再次失败时，统一边界仍可据此决定退出状态。
 */
class EmergencyFailure {
  public:
    /** @brief 仅在尚无首错时复制错误头。 */
    void record_first(FailureHeader h) noexcept {
        if (!set_) {
            header_ = h;
            set_ = true;
        }
    }
    /** @brief 查询是否已记录首错。 */
    bool is_set() const noexcept {
        return set_;
    }
    /**
     * @brief 借用已记录的首错头。
     * @pre is_set() 为 true，方可将结果用作本次会话的错误。
     */
    const FailureHeader &header() const noexcept {
        return header_;
    }

  private:
    FailureHeader header_{FailureDomain::Internal, ReasonCode::Invariant, {}, {}};
    bool set_{};
};
/** @brief 借用首错记录并携带当前操作的可选源码范围。 */
struct ErrorSite {
    EmergencyFailure &failure;
    std::optional<SourceSpan> span{};
};
/** @brief 拥有错误头与消息文本的 C++ 异常；消息构造可能分配宿主内存。 */
class Failure : public std::exception {
  public:
    /** @brief 复制错误头和消息；首错记录由调用方在构造前写入。 */
    Failure(FailureHeader h, std::string_view message) : header_(h), message_(message) {}
    /** @brief 借用随异常存活的错误头。 */
    const FailureHeader &header() const noexcept {
        return header_;
    }
    /** @brief 借用随异常存活的 NUL 终止消息。 */
    const char *what() const noexcept override {
        return message_.c_str();
    }
    /** @brief 借用完整消息字节，保留内嵌 NUL；视图有效至本异常销毁。 */
    std::string_view message() const noexcept {
        return message_;
    }

  private:
    FailureHeader header_;
    std::string message_;
};
/** @brief 表示词法、语法或全文件结构检查失败。 */
class FrontendFailure final : public Failure {
  public:
    using Failure::Failure;
};
/** @brief 表示执行 MYB 时发生的动态检查或程序 I/O 失败。 */
class MybRuntimeFailure final : public Failure {
  public:
    using Failure::Failure;
};
/** @brief 表示用法、入口 I/O、资源或内部实现故障。 */
class HostFailure final : public Failure {
  public:
    using Failure::Failure;
};
/**
 * @brief 先无分配地保存首错，再按 domain 构造并抛出对应异常。
 *
 * Frontend/Structure 抛 FrontendFailure，Runtime 抛 MybRuntimeFailure，
 * 其余阶段抛 HostFailure；消息分配失败时已保存的首错仍有效。
 */
[[noreturn]] void fail(ErrorSite, FailureDomain, ReasonCode, std::string_view,
                       std::optional<std::uint32_t> os_code = {});
/** @brief 映射失败退出状态：Usage/EntryIO 为 2，Resource/Internal 为 3，其余为 1。 */
int exit_code(const FailureHeader &) noexcept;
/** @brief 返回具有静态存储期的错误阶段名称。 */
std::string_view domain_name(FailureDomain) noexcept;
/** @brief 返回具有静态存储期的错误原因名称。 */
std::string_view reason_name(ReasonCode) noexcept;
} // namespace myb
