#pragma once

#include "monitoring/host_q4_collection_capacity_inputs.hpp"

#include <cstdint>
#include <expected>

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringQ4UnsupportedReason : unsigned char {
    no_resolvable_policy,
    no_current_attribute
};

struct MonitoringQ4CapacityAudit final {
    // The byte component fields through interval_summary_bytes are disjoint and sum to total_bytes.
    // The four diagnostics below reconcile selected spellings within those components and are
    // intentionally nonadditive. CPU/ordinal/token are totals; frequency is the expansion beyond
    // the one-digit spellings already owned by fixed record syntax.
    std::uint64_t common_root_bytes{};
    std::uint64_t scope_bytes{};
    std::uint64_t result_wrapper_bytes{};
    std::uint64_t discovery_bytes{};
    std::uint64_t domain_bytes{};
    std::uint64_t preparation_bytes{};
    std::uint64_t agreement_bytes{};
    std::uint64_t typed_read_bytes{};
    std::uint64_t unavailable_bytes{};
    std::uint64_t duration_wrapper_bytes{};
    std::uint64_t interval_summary_bytes{};
    std::uint64_t cpu_decimal_total_bytes{};
    std::uint64_t ordinal_decimal_total_bytes{};
    std::uint64_t token_escaping_total_bytes{};
    std::uint64_t frequency_decimal_extra_bytes{};
    std::uint64_t total_bytes{};
    std::uint64_t supported_count{};
    MonitoringQ4UnsupportedReason reason{MonitoringQ4UnsupportedReason::no_resolvable_policy};
};

class MonitoringQ4CollectionCapacity final {
  public:
    MonitoringQ4CollectionCapacity(const MonitoringQ4CollectionCapacity&) = default;
    MonitoringQ4CollectionCapacity(MonitoringQ4CollectionCapacity&&) noexcept = default;
    auto operator=(const MonitoringQ4CollectionCapacity&)
        -> MonitoringQ4CollectionCapacity& = delete;
    auto operator=(MonitoringQ4CollectionCapacity&&) -> MonitoringQ4CollectionCapacity& = delete;

    [[nodiscard]] constexpr auto pass_evidence_bytes() const noexcept { return pass_; }
    [[nodiscard]] constexpr auto unsupported_evidence_bytes() const noexcept {
        return unsupported_;
    }
    [[nodiscard]] constexpr auto evidence_bytes() const noexcept { return evidence_; }
    [[nodiscard]] constexpr auto success_stdout_bytes() const noexcept { return success_; }
    [[nodiscard]] constexpr auto pass_audit() const noexcept -> const MonitoringQ4CapacityAudit& {
        return pass_audit_;
    }
    [[nodiscard]] constexpr auto unsupported_audit() const noexcept
        -> const MonitoringQ4CapacityAudit& {
        return unsupported_audit_;
    }
    [[nodiscard]] constexpr auto backend_stderr_bytes() const noexcept {
        return MonitoringQ4CapacityBytes{792302};
    }
    [[nodiscard]] constexpr auto public_cleanup_wrapper_bytes() const noexcept {
        return MonitoringQ4CapacityBytes{796659};
    }
    [[nodiscard]] constexpr auto backend_error_bytes() const noexcept {
        return MonitoringQ4CapacityBytes{10892};
    }
    [[nodiscard]] constexpr auto contained_fallback_bytes() const noexcept {
        return MonitoringQ4CapacityBytes{4481};
    }
    [[nodiscard]] constexpr bool accepts_evidence(std::uint64_t bytes) const noexcept {
        return bytes <= evidence_.value;
    }
    [[nodiscard]] constexpr bool accepts_success_stdout(std::uint64_t bytes) const noexcept {
        return bytes <= success_.value;
    }

  private:
    friend class MonitoringQ4SourceReadyTestAccess;
    friend auto
    monitoring_q4_collection_capacity(const MonitoringQ4CollectionCapacityInputs&) noexcept
        -> std::expected<MonitoringQ4CollectionCapacity, MonitoringQ4CapacityFailure>;
    constexpr MonitoringQ4CollectionCapacity() = default;
    MonitoringQ4CapacityBytes pass_{};
    MonitoringQ4CapacityBytes unsupported_{};
    MonitoringQ4CapacityBytes evidence_{};
    MonitoringQ4CapacityBytes success_{};
    MonitoringQ4CapacityAudit pass_audit_{};
    MonitoringQ4CapacityAudit unsupported_audit_{};
};

[[nodiscard]] auto
monitoring_q4_collection_capacity(const MonitoringQ4CollectionCapacityInputs&) noexcept
    -> std::expected<MonitoringQ4CollectionCapacity, MonitoringQ4CapacityFailure>;

} // namespace epycbench::monitoring::qualification::detail
