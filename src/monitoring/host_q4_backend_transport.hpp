#pragma once
#include "epycbench/core/error.hpp"
#include "monitoring/governed_plan_decode.hpp"
#include "monitoring/host_q4_cpufreq_collector.hpp"
#include "monitoring/host_q4_cpufreq_evidence_value.hpp"
#include "monitoring/host_q4_source_ready.hpp"
#include <array>
#include <expected>
#include <optional>
#include <string>
#include <vector>
namespace epycbench::monitoring::qualification::detail {
enum class MonitoringHostQ4BackendPhase : unsigned char {
    repository,
    collection_plan,
    predecessor,
    build_attestation,
    source_discovery,
    oracle,
    source_read,
    agreement,
    clock,
    deadline,
    capacity,
    finalization,
    evidence_construction
};
struct MonitoringHostQ4BackendFailure final {
    MonitoringHostQ4BackendPhase phase{MonitoringHostQ4BackendPhase::finalization};
    std::string underlying_code;
    std::optional<std::string> repository_classification;
    std::optional<int> native_error;
    std::array<std::optional<core::Error>, 31> cleanup{};
    std::size_t cleanup_count{};
    bool cleanup_overflowed{}, terminal_containment{}, usage{};
};
struct MonitoringHostQ4BackendAuthored final {
    std::vector<MonitoringGovernedPlanAuthoredDiagnostic> diagnostics;
};
[[nodiscard]] auto assemble_monitoring_host_q4_backend_result(const MonitoringQ4SourceReady&,
                                                              std::string_view) noexcept
    -> std::expected<std::string, core::Error>;
[[nodiscard]] auto validate_monitoring_host_q4_backend_result(const MonitoringQ4SourceReady&,
                                                              std::string_view) noexcept -> bool;
[[nodiscard]] auto
serialize_monitoring_host_q4_backend_failure(const MonitoringQ4SourceReady&,
                                             const MonitoringHostQ4BackendFailure&) noexcept
    -> std::expected<std::string, core::Error>;
[[nodiscard]] auto
serialize_monitoring_host_q4_backend_failure_pre_source(
    const MonitoringHostQ4BackendFailure&) noexcept -> std::expected<std::string, core::Error>;
[[nodiscard]] auto validate_monitoring_host_q4_backend_failure(const MonitoringQ4SourceReady&,
                                                               std::string_view) noexcept -> bool;
[[nodiscard]] auto
serialize_monitoring_host_q4_backend_authored(const MonitoringQ4SourceReady&,
                                              const MonitoringHostQ4BackendAuthored&) noexcept
    -> std::expected<std::string, core::Error>;
[[nodiscard]] auto
serialize_monitoring_host_q4_backend_authored_pre_source(
    const MonitoringHostQ4BackendAuthored&) noexcept -> std::expected<std::string, core::Error>;
[[nodiscard]] auto validate_monitoring_host_q4_backend_authored(const MonitoringQ4SourceReady&,
                                                                std::string_view) noexcept -> bool;
[[nodiscard]] auto
project_monitoring_host_q4_collection_failure(const MonitoringQ4CpuFreqCollectionFailure&) noexcept
    -> MonitoringHostQ4BackendFailure;
void monitoring_host_q4_backend_serialization_test_fault(void (*)(std::size_t));
void monitoring_host_q4_backend_serialization_test_fault_reset() noexcept;
struct MonitoringHostQ4BackendOperations final {
    struct Failure final {
        bool terminal{};
        std::optional<core::Error> ordinary;
    };
    void* context{};
    std::expected<MonitoringQ4CpuFreqCollection, MonitoringQ4CpuFreqCollectionFailure> (*collect)(
        void*, const MonitoringQ4SourceReady&){};
    std::expected<MonitoringQ4CpuFreqEvidenceValue, Failure> (*build)(
        void*, const MonitoringQ4SourceReady&, MonitoringQ4CpuFreqCollection&){};
    std::expected<std::string, Failure> (*encode)(void*, const MonitoringQ4CpuFreqEvidenceValue&){};
    std::expected<std::string, Failure> (*assemble)(void*, const MonitoringQ4SourceReady&,
                                                    std::string_view){};
};
[[nodiscard]] auto
run_monitoring_host_q4_backend_adapter_with(MonitoringQ4SourceReady&,
                                            MonitoringHostQ4BackendOperations) noexcept
    -> std::expected<std::string, MonitoringHostQ4BackendFailure>;
[[nodiscard]] auto run_monitoring_host_q4_backend_adapter(MonitoringQ4SourceReady&) noexcept
    -> std::expected<std::string, MonitoringHostQ4BackendFailure>;
} // namespace epycbench::monitoring::qualification::detail
