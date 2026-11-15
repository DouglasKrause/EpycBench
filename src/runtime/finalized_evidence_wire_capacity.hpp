#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::runtime::wire_v1 {

inline constexpr std::string_view wire_profile = "epycbench.finalized-pairwise-evidence-wire/v1";
inline constexpr std::string_view capacity_profile =
    "epycbench.finalized-pairwise-evidence-capacity/v1";
inline constexpr std::string_view registry_identity =
    "epycbench.finalized-pairwise-evidence-wire-registry/v1";

struct CapacityCount final {
    std::uint64_t value{};
    friend constexpr auto operator==(const CapacityCount&, const CapacityCount&) noexcept
        -> bool = default;
    friend constexpr auto operator<=>(const CapacityCount&,
                                      const CapacityCount&) noexcept = default;
};

template <typename Tag> struct Quantity final {
    std::uint64_t value{};
    friend constexpr auto operator==(const Quantity&, const Quantity&) noexcept -> bool = default;
    friend constexpr auto operator<=>(const Quantity&, const Quantity&) noexcept = default;
};
using WorkerCount = Quantity<struct WorkerCountTag>;
using BlockCount = Quantity<struct BlockCountTag>;
using WarmupTrialCount = Quantity<struct WarmupTrialCountTag>;
using BasePageBytes = Quantity<struct BasePageBytesTag>;
using MonitoringRounds = Quantity<struct MonitoringRoundsTag>;
using CpuId = Quantity<struct CpuIdTag>;
using MaxCpuId = Quantity<struct MaxCpuIdTag>;
using NumaMaxNode = Quantity<struct NumaMaxNodeTag>;
using PageCount = Quantity<struct PageCountTag>;
using TargetCount = Quantity<struct TargetCountTag>;
struct ChannelFlags final {
    bool page{}, context{}, temperature{}, frequency{};
    friend constexpr auto operator==(const ChannelFlags&, const ChannelFlags&) noexcept
        -> bool = default;
};

struct RequestKnownEscapedBytes final {
    CapacityCount scenario_profile, scenario_digest, resolved_plan_digest, workload_id,
        first_variant_id, second_variant_id, monitoring_implementation_ids,
        frequency_driver_ids, frequency_governor_ids, stable_numa_node_ids;
    friend constexpr auto operator==(const RequestKnownEscapedBytes&,
                                     const RequestKnownEscapedBytes&) noexcept -> bool = default;
};

struct CapacityParameterCandidate final {
    WorkerCount workers;
    BlockCount maximum_blocks;
    WarmupTrialCount maximum_warmup_trials;
    BasePageBytes base_page_bytes;
    MaxCpuId maximum_cpu_index;
    NumaMaxNode numa_domain_maxnode;
    MonitoringRounds monitoring_rounds;
    ChannelFlags channels;
    RequestKnownEscapedBytes request_known_escaped_bytes;
    std::span<const CpuId> present_cpu_ids;
    std::span<const std::span<const CpuId>> domain_cpu_ids;
    std::span<const PageCount> first_total_pages;
    std::span<const TargetCount> first_target_counts;
    std::span<const PageCount> second_total_pages;
    std::span<const TargetCount> second_target_counts;
};

struct CapacityError;

struct NormalizedCapacityParameterValue final {
    WorkerCount workers;
    BlockCount maximum_blocks;
    WarmupTrialCount maximum_warmup_trials;
    BasePageBytes base_page_bytes;
    MaxCpuId maximum_cpu_index;
    NumaMaxNode numa_domain_maxnode;
    MonitoringRounds monitoring_rounds;
    ChannelFlags channels;
    RequestKnownEscapedBytes request_known_escaped_bytes;
    std::vector<CpuId> present_cpu_ids;
    std::vector<std::vector<CpuId>> domain_cpu_ids;
    std::vector<PageCount> first_total_pages, second_total_pages;
    std::vector<TargetCount> first_target_counts, second_target_counts;
    friend auto operator==(const NormalizedCapacityParameterValue&,
                           const NormalizedCapacityParameterValue&) noexcept -> bool = default;
};

class NormalizedCapacityParameters final {
  public:
    [[nodiscard]] const auto& value() const noexcept { return value_; }

  private:
    explicit NormalizedCapacityParameters(NormalizedCapacityParameterValue value) noexcept
        : value_(std::move(value)) {}
    NormalizedCapacityParameterValue value_;
    friend auto normalize_capacity_parameters(const CapacityParameterCandidate&) noexcept
        -> std::expected<NormalizedCapacityParameters, CapacityError>;
};

struct ComponentMaxima final {
    CapacityCount blocks, factual_observations, failed_observations, stopping_evaluations, workers,
        placements, migrations, validity_checks, finalized_workers, failed_observation_errors,
        operational_terminal_errors, raw_page_acquisitions, raw_context_acquisitions,
        context_events, context_offsets, raw_temperature_acquisitions, frequency_domains,
        frequency_acquisitions, frequency_cpu_memberships, frequency_metadata,
        classified_page_workers, classified_context_workers, classified_frequency_domains,
        first_resident_page_counts, second_resident_page_counts;
    friend constexpr auto operator==(const ComponentMaxima&, const ComponentMaxima&) noexcept
        -> bool = default;
};

struct MonitoringLowerStocks final {
    CapacityCount persistent_worker_scopes, page_sources, context_sources, context_coverage_slots,
        temperature_sources, frequency_sources, frequency_metadata_slots,
        persistent_frequency_cpu_ids, preflight_channel_dispositions, page_pair_scratch,
        context_array_scratch, temperature_view_scratch, frequency_view_scratch,
        frequency_result_scratch, whole_stop_cleanup, preparation_rollback_cleanup,
        completion_abort_cleanup, cpufreq_paths, cpufreq_directory_entries, cpufreq_operation_slots,
        hwmon_directory_entries, hwmon_operation_slots, dynamic_frequency_raw_bytes,
        dynamic_frequency_escaped_bytes;
    friend constexpr auto operator==(const MonitoringLowerStocks&,
                                     const MonitoringLowerStocks&) noexcept -> bool = default;
};

struct EvidenceDestinations final {
    CapacityCount provenance, policies, calibration, warmup, blocks, stopping_evaluations, terminal,
        finalization, monitoring_raw, monitoring_classified, errors, memory_finalization;
    friend constexpr auto operator==(const EvidenceDestinations&,
                                     const EvidenceDestinations&) noexcept -> bool = default;
};

// This deliberately excludes logical bytes/nodes/depth and byte/node resource
// reservations. Only the later recursive shape evaluator may construct those.
struct CalculatedCapacityPrerequisite final {
    NormalizedCapacityParameterValue parameters;
    ComponentMaxima components;
    MonitoringLowerStocks monitoring_lower_stocks;
    EvidenceDestinations evidence_destinations;
    friend constexpr auto operator==(const CalculatedCapacityPrerequisite&,
                                     const CalculatedCapacityPrerequisite&) noexcept
        -> bool = default;
};

struct LogicalShape final {
    CapacityCount canonical_logical_bytes, typed_nodes, parser_depth;
    friend constexpr auto operator==(const LogicalShape&, const LogicalShape&) noexcept
        -> bool = default;
};

struct ResourcePlan final {
    CapacityCount controller_receive_bytes, controller_overflow_probe_bytes,
        controller_node_slots;
    EvidenceDestinations controller_decode_destinations;
    CapacityCount runner_encode_bytes, runner_overflow_probe_bytes, runner_node_slots;
    EvidenceDestinations runner_evidence_destinations;
    friend constexpr auto operator==(const ResourcePlan&, const ResourcePlan&) noexcept
        -> bool = default;
};

enum class KernelIdentityCapacity { plugin_tokens, legacy_builtin_tokens };

struct CalculatedEvidenceCapacity final {
    CalculatedCapacityPrerequisite prerequisite;
    LogicalShape logical_shape;
    ResourcePlan resource_plan;
    KernelIdentityCapacity identities{KernelIdentityCapacity::plugin_tokens};
    friend constexpr auto operator==(const CalculatedEvidenceCapacity&,
                                     const CalculatedEvidenceCapacity&) noexcept -> bool = default;
};

enum class CapacityErrorCode : std::uint8_t {
    invalid_input,
    inconsistent_parameters,
    arithmetic_overflow,
    allocation_failure,
    size_unrepresentable,
    mismatch,
};

enum class CapacityField : std::uint8_t {
    input,
    present_cpu_universe,
    cpu_partition,
    first_page_vector,
    second_page_vector,
    component_maxima,
    monitoring_lower_stocks,
    evidence_destinations,
    request_known_escaped_bytes,
    logical_shape,
    resource_plan,
    resource_probe,
};

struct CapacityError final {
    CapacityErrorCode code;
    CapacityField field;
    CapacityCount expected{};
    CapacityCount observed{};
    friend constexpr auto operator==(const CapacityError&, const CapacityError&) noexcept
        -> bool = default;
};

struct LocalCapacityLimits final {
    std::uint64_t size_max{static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())};
};

using ResourceProbe = auto (*)(std::size_t bytes_with_probe, std::size_t node_slots) noexcept
    -> bool;

struct LocalResourceLimits final {
    std::uint64_t size_max{static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())};
    std::uint64_t byte_container_max{static_cast<std::uint64_t>(
        std::vector<std::byte>{}.max_size())};
    std::uint64_t node_container_max{static_cast<std::uint64_t>(
        std::vector<std::uint64_t>{}.max_size())};
    std::uint64_t parser_depth_max{static_cast<std::uint64_t>(
        std::numeric_limits<std::size_t>::max())};
    ResourceProbe probe{};
};

[[nodiscard]] auto
normalize_capacity_parameters(const CapacityParameterCandidate& candidate) noexcept
    -> std::expected<NormalizedCapacityParameters, CapacityError>;

[[nodiscard]] auto calculate_capacity_prerequisite(const NormalizedCapacityParameters& parameters,
                                                   LocalCapacityLimits limits = {}) noexcept
    -> std::expected<CalculatedCapacityPrerequisite, CapacityError>;

[[nodiscard]] auto
validate_exact_capacity_prerequisite(const CalculatedCapacityPrerequisite& expected,
                                     const CalculatedCapacityPrerequisite& observed) noexcept
    -> std::expected<void, CapacityError>;

enum class EvidenceComponentKind : std::uint8_t { block_plan, factual_observation, failed_observation, stopping_evaluation, finalization, calibration, warmup, monitoring_result, terminal, finalized_error };
enum class EvidenceComponentSide : std::uint8_t { first, second };
namespace detail {
// Closed generated-edge selection shared by decoder and component-size preflight.
[[nodiscard]] auto evidence_component_node(EvidenceComponentKind) noexcept
    -> std::optional<std::uint32_t>;
}
[[nodiscard]] auto maximum_component_shape(EvidenceComponentKind, EvidenceComponentSide,
    const CalculatedEvidenceCapacity&) noexcept -> std::expected<LogicalShape, CapacityError>;

[[nodiscard]] auto minimum_logical_shape() noexcept
    -> std::expected<LogicalShape, CapacityError>;

[[nodiscard]] auto calculate_evidence_capacity(const NormalizedCapacityParameters& parameters,
                                                LocalResourceLimits limits = {},
                                                KernelIdentityCapacity identities =
                                                    KernelIdentityCapacity::plugin_tokens) noexcept
    -> std::expected<CalculatedEvidenceCapacity, CapacityError>;

[[nodiscard]] auto
validate_exact_evidence_capacity(const CalculatedEvidenceCapacity& expected,
                                 const CalculatedEvidenceCapacity& observed) noexcept
    -> std::expected<void, CapacityError>;

} // namespace epycbench::runtime::wire_v1
