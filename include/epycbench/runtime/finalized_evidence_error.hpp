#pragma once

#include "epycbench/runtime/pairwise_finalized_error_profile.hpp"

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace epycbench::runtime {

class FinalizedEvidenceError;

class FinalizedEvidenceOsError final {
  public:
    FinalizedEvidenceOsError(const FinalizedEvidenceOsError&) noexcept = default;
    FinalizedEvidenceOsError(FinalizedEvidenceOsError&&) noexcept = default;
    auto operator=(const FinalizedEvidenceOsError&) -> FinalizedEvidenceOsError& = delete;
    auto operator=(FinalizedEvidenceOsError&&) -> FinalizedEvidenceOsError& = delete;

    [[nodiscard]] constexpr auto domain() const noexcept -> PairwiseOsDomain { return domain_; }
    [[nodiscard]] constexpr auto value() const noexcept -> int { return value_; }

    auto operator<=>(const FinalizedEvidenceOsError&) const = default;

  private:
    friend class FinalizedEvidenceError;

    constexpr FinalizedEvidenceOsError(PairwiseOsDomain domain, int value) noexcept
        : domain_{domain}, value_{value} {}

    PairwiseOsDomain domain_;
    int value_;
};

class FinalizedEvidenceError final {
  public:
    [[nodiscard]] static auto from_failure(PairwiseEvidenceFailure failure) noexcept
        -> FinalizedEvidenceError;

    FinalizedEvidenceError(const FinalizedEvidenceError&) noexcept = default;
    FinalizedEvidenceError(FinalizedEvidenceError&&) noexcept = default;
    auto operator=(const FinalizedEvidenceError&) -> FinalizedEvidenceError& = delete;
    auto operator=(FinalizedEvidenceError&&) -> FinalizedEvidenceError& = delete;

    [[nodiscard]] auto subsystem() const noexcept -> PairwiseFailureSubsystem;
    [[nodiscard]] auto category() const noexcept -> core::ErrorCategory;
    [[nodiscard]] constexpr auto code() const noexcept -> PairwiseFailureCode { return code_; }
    [[nodiscard]] auto stable_code() const noexcept -> std::string_view;
    [[nodiscard]] constexpr auto operation() const noexcept -> PairwiseFailureOperation {
        return operation_;
    }
    [[nodiscard]] auto operation_token() const noexcept -> std::string_view;
    [[nodiscard]] constexpr auto reason() const noexcept -> PairwiseFailureReason {
        return reason_;
    }
    [[nodiscard]] auto reason_token() const noexcept -> std::string_view;
    [[nodiscard]] constexpr auto context() const noexcept -> const PairwiseFailureContext& {
        return context_;
    }
    [[nodiscard]] constexpr auto context_kind() const noexcept -> PairwiseContextKind {
        return runtime::context_kind(context_);
    }
    [[nodiscard]] auto severity() const noexcept -> core::ErrorSeverity;
    [[nodiscard]] auto continuation() const noexcept -> core::ContinuationPolicy;
    [[nodiscard]] constexpr auto os_error() const noexcept
        -> const std::optional<FinalizedEvidenceOsError>& {
        return os_error_;
    }
    [[nodiscard]] auto renderer() const noexcept -> PairwiseRendererId;

    auto operator<=>(const FinalizedEvidenceError&) const = default;

  private:
    constexpr FinalizedEvidenceError(PairwiseFailureCode code, PairwiseFailureOperation operation,
                                     PairwiseFailureReason reason, PairwiseFailureContext context,
                                     std::optional<FinalizedEvidenceOsError> os_error) noexcept
        : code_{code}, operation_{operation}, reason_{reason}, context_{context},
          os_error_{os_error} {}

    PairwiseFailureCode code_;
    PairwiseFailureOperation operation_;
    PairwiseFailureReason reason_;
    PairwiseFailureContext context_;
    std::optional<FinalizedEvidenceOsError> os_error_;
};

[[nodiscard]] auto render_finalized_evidence_error(const FinalizedEvidenceError& error)
    -> std::string;

} // namespace epycbench::runtime
