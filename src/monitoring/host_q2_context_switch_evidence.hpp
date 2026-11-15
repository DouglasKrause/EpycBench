#pragma once

#include "monitoring/host_q2_context_switch_collector.hpp"

#include <expected>
#include <string>

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ2EvidenceBytes final { std::string canonical_json; };
struct MonitoringQ2EvidenceAllowances final {
    std::uint64_t fixed{65536}, worker{2048}, trial{768}, summary{1024}, total{16777216};
};
[[nodiscard]] auto validate_monitoring_q2_evidence_capacity_proof(
    MonitoringQ2EvidenceAllowances = {}) noexcept -> bool;

[[nodiscard]] auto construct_monitoring_q2_context_switch_evidence(
    MonitoringQ2ContextSwitchCollectionOutcome&&) noexcept
    -> std::expected<MonitoringQ2EvidenceBytes, core::Error>;
[[nodiscard]] auto validate_monitoring_q2_context_switch_evidence(std::string_view) noexcept
    -> std::expected<void, core::Error>;

} // namespace epycbench::monitoring::qualification::detail
