#pragma once

#include "monitoring/host_frozen_collection.hpp"
#include "monitoring/host_production_authority.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string_view>

namespace epycbench::monitoring::qualification::detail {
enum class HostFrozenEvidenceFailure : unsigned char {
    unavailable, domain, association, uncertain_cleanup, unrepresented_failure,
    capacity, allocation, contained_exception,
};
struct HostFrozenEvidenceView final {
    std::string_view canonical_json;
    std::string_view canonical_common;
};
class HostFrozenEvidenceTestAccess;
class HostFrozenTerminationEvidence;
// Ordinary untimed projection, after confirmed native destruction and before
// consuming the cell. No input owner is consumed or producer authority decoded.
class HostFrozenCellEvidence final {
  public:
    static constexpr std::size_t hard_limit = HostProductionAuthority::hard_limit;
    HostFrozenCellEvidence(HostFrozenCellEvidence&&) noexcept;
    HostFrozenCellEvidence(const HostFrozenCellEvidence&) = delete;
    auto operator=(HostFrozenCellEvidence&&) -> HostFrozenCellEvidence& = delete;
    ~HostFrozenCellEvidence() noexcept;
    [[nodiscard]] static auto create(const HostFrozenCell&,
        const MonitoringQ1ValidatedPublicValues&, const MonitoringRunningBuildAuthority&,
        std::size_t allowance = hard_limit) noexcept
        -> std::expected<HostFrozenCellEvidence, HostFrozenEvidenceFailure>;
    [[nodiscard]] auto view() const noexcept -> std::optional<HostFrozenEvidenceView>;
  private:
    struct State;
    explicit HostFrozenCellEvidence(std::unique_ptr<State>) noexcept;
    static auto project(const HostFrozenCell&, const MonitoringQ1ValidatedPublicValues*,
        const MonitoringRunningBuildAuthority*, bool synthetic, std::size_t) noexcept
        -> std::expected<HostFrozenCellEvidence, HostFrozenEvidenceFailure>;
    static auto authority_component(const HostProductionAuthority&,
        void (*after_clone)() noexcept) noexcept
        -> std::expected<std::string, HostFrozenEvidenceFailure>;
    std::unique_ptr<State> state_;
    friend class HostFrozenEvidenceTestAccess;
};
class HostFrozenFailureEvidence final {
  public:
    HostFrozenFailureEvidence(HostFrozenFailureEvidence&&) noexcept;
    HostFrozenFailureEvidence(const HostFrozenFailureEvidence&) = delete;
    auto operator=(HostFrozenFailureEvidence&&) -> HostFrozenFailureEvidence& = delete;
    ~HostFrozenFailureEvidence() noexcept;
    [[nodiscard]] static auto create(const HostFrozenCollection&,
        const HostFrozenCollectionFailure&, const MonitoringQ1ValidatedPublicValues&,
        const MonitoringRunningBuildAuthority&,
        std::size_t allowance = HostFrozenCellEvidence::hard_limit) noexcept
        -> std::expected<HostFrozenFailureEvidence, HostFrozenEvidenceFailure>;
    [[nodiscard]] auto view() const noexcept -> std::optional<HostFrozenEvidenceView>;
  private:
    struct State;
    explicit HostFrozenFailureEvidence(std::unique_ptr<State>) noexcept;
    static auto project(const HostFrozenCollection&, const HostFrozenCollectionFailure&,
        const MonitoringQ1ValidatedPublicValues*, const MonitoringRunningBuildAuthority*,
        bool synthetic, std::size_t) noexcept
        -> std::expected<HostFrozenFailureEvidence, HostFrozenEvidenceFailure>;
    std::unique_ptr<State> state_;
    friend class HostFrozenEvidenceTestAccess;
    friend class HostFrozenTerminationEvidence;
};
// The existing FINISH body, derived from the actual consumed collection. This
// owns only untimed bytes; it supplies no publication or producer authority.
class HostFrozenTerminationEvidence final {
  public:
    HostFrozenTerminationEvidence(HostFrozenTerminationEvidence&&) noexcept;
    HostFrozenTerminationEvidence(const HostFrozenTerminationEvidence&) = delete;
    auto operator=(HostFrozenTerminationEvidence&&) -> HostFrozenTerminationEvidence& = delete;
    ~HostFrozenTerminationEvidence() noexcept;
    [[nodiscard]] static auto create(const HostFrozenCollection&,
        const HostFrozenCollectionFailure*, const MonitoringQ1ValidatedPublicValues&,
        const MonitoringRunningBuildAuthority&,
        std::size_t allowance = HostFrozenCellEvidence::hard_limit) noexcept
        -> std::expected<HostFrozenTerminationEvidence, HostFrozenEvidenceFailure>;
    [[nodiscard]] auto view() const noexcept -> std::optional<HostFrozenEvidenceView>;
  private:
    struct State;
    explicit HostFrozenTerminationEvidence(std::unique_ptr<State>) noexcept;
    static auto project(const HostFrozenCollection&, const HostFrozenCollectionFailure*,
        const MonitoringQ1ValidatedPublicValues*, const MonitoringRunningBuildAuthority*,
        bool synthetic, std::size_t) noexcept
        -> std::expected<HostFrozenTerminationEvidence, HostFrozenEvidenceFailure>;
    std::unique_ptr<State> state_;
    friend class HostFrozenEvidenceTestAccess;
};
#if defined(EPYCBENCH_HOST_FROZEN_COLLECTION_TEST)
// This target-only boundary accepts actual tiny synthetic owners, never JSON or
// a callback standing in for execution; it cannot publish source qualification.
class HostFrozenEvidenceTestAccess final {
  public:
    static auto authority_component(const HostProductionAuthority& owner,
        void (*after_clone)() noexcept = nullptr) noexcept {
        return HostFrozenCellEvidence::authority_component(owner, after_clone);
    }
    static auto cell(const HostFrozenCell& cell,
        std::size_t allowance = HostFrozenCellEvidence::hard_limit) noexcept {
        return HostFrozenCellEvidence::project(cell, nullptr, nullptr, true, allowance);
    }
    static auto failure(const HostFrozenCollection& origin,
        const HostFrozenCollectionFailure& failure,
        std::size_t allowance = HostFrozenCellEvidence::hard_limit) noexcept {
        return HostFrozenFailureEvidence::project(origin, failure, nullptr, nullptr, true, allowance);
    }
    static auto termination(const HostFrozenCollection& origin,
        const HostFrozenCollectionFailure* failure = nullptr,
        std::size_t allowance = HostFrozenCellEvidence::hard_limit) noexcept {
        return HostFrozenTerminationEvidence::project(origin, failure, nullptr, nullptr, true, allowance);
    }
};
#endif
} // namespace epycbench::monitoring::qualification::detail
