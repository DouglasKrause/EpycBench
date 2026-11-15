#pragma once

#include "runtime/finalized_evidence_prepared_resources.hpp"
#include "runtime/pairwise_v2_progress.hpp"
#include "runtime/pairwise_preparation_graph.hpp"
#include "platform_linux/hwmon_temperature_access.hpp"

#include <cstdint>
#include <expected>
#include <string_view>

namespace epycbench::runtime::detail { struct PreparationGraphLimits; }
namespace epycbench::runtime::detail::stage_b_policy {
inline constexpr std::string_view profile = "epycbench.stage-b-resource-policy/v2";
inline constexpr std::uint64_t frame = 1'048'576;
inline constexpr std::uint64_t startup = 8'388'608;
inline constexpr std::uint64_t boot_nodes = 32'768;
inline constexpr std::uint64_t boot_depth = 64;
inline constexpr std::uint64_t boot_text = 1'048'576;
inline constexpr std::uint64_t logical = 1'073'741'824;
inline constexpr std::uint64_t text = 16'777'216;
inline constexpr std::uint64_t count = 33'554'432;
inline constexpr std::uint64_t elements = 16'777'216;
inline constexpr std::uint64_t depth = 128;
inline constexpr std::uint64_t definitions = 1'024;
inline constexpr std::uint64_t replicates = 100'000;
inline constexpr std::uint64_t component = 33'554'432;
inline constexpr std::uint64_t arena = 1'073'741'824;
inline constexpr std::uint64_t wire = 4'294'967'296;
inline constexpr std::uint64_t workers = 256;
inline constexpr std::uint64_t regions = 256;
inline constexpr std::uint64_t blocks = 65'536;
inline constexpr std::uint64_t trials = 65'536;
inline constexpr std::uint64_t page_entries = 16'777'216;
inline constexpr std::uint64_t node_bits = 1'048'576;
inline constexpr std::uint64_t node_entries = 65'536;
inline constexpr std::uint64_t monitoring_domains = 65'536;
inline constexpr std::uint64_t monitoring_members = 8'388'608;
inline constexpr std::uint64_t monitoring_text = 1'048'576;
inline constexpr std::uint64_t hwmon_entry_bytes = 67'108'864;
inline constexpr std::uint64_t hwmon_operation_slots = 2'097'152;
inline constexpr std::uint64_t graph_rows = 65'536;
inline constexpr std::uint64_t graph_text = 16'777'216;
inline constexpr std::uint64_t verification = 16'777'216;
inline constexpr std::uint64_t lifecycle = 16;

// This private selector retains its original name; profile identifies the
// current reviewed dimensions. It is not an argv/environment or wire override.
enum class Selection : std::uint8_t { existing_caller, stage_b_v1 };
// Applied before the existing default capacity resource probe reserves storage.
[[nodiscard]] inline auto capacity_limits(Selection selection) noexcept -> wire_v1::LocalResourceLimits {
    if (selection == Selection::existing_caller) return {};
    wire_v1::LocalResourceLimits limits;
    limits.byte_container_max = logical + 1U;
    limits.node_container_max = count;
    limits.parser_depth_max = depth;
    return limits;
}
enum class Error : std::uint8_t { limit, shape, overflow };
using Admission = std::expected<void, Error>;
// Non-owning lexical bounds only; the existing parser still validates JSON.
[[nodiscard]] auto admit_bootstrap(std::string_view) noexcept -> Admission;
// Encoded startup bodies/copies only; this is not an aggregate heap/RSS budget.
[[nodiscard]] auto admit_startup_retention(std::span<const std::uint64_t>) noexcept -> Admission;
struct ProgressPreparation final {
    progress::Limits limits;
    PreparationGraphLimits graph;
};
[[nodiscard]] auto derive_progress(const PairwiseHostCapacityRequestTranscript&,
    const wire_v1::CalculatedEvidenceCapacity&) noexcept -> std::expected<ProgressPreparation, Error>;
[[nodiscard]] auto admit_hwmon(platform_linux::detail::BoundedHwmonCapacity) noexcept -> Admission;
[[nodiscard]] auto admit(const wire_v1::CalculatedEvidenceCapacity&) noexcept -> Admission;
[[nodiscard]] auto admit(const PreparationGraphLimits&) noexcept -> Admission;
[[nodiscard]] auto admit(const AdmittedPairwiseProductionInputs&) noexcept -> Admission;
[[nodiscard]] auto admit(const ipc::PlacementRequest&) noexcept -> Admission;
[[nodiscard]] auto admit(const PairwiseHostCapacityRequestTranscript&) noexcept -> Admission;
// Additional caller policy, not a substitute for lower semantic admission.
[[nodiscard]] auto admit(const prepared_resources_v1::PreparedScalarStocks&) noexcept
    -> Admission;
[[nodiscard]] auto admit(const prepared_resources_v1::PreparedResourceValue&) noexcept
    -> Admission;
[[nodiscard]] auto admit(const progress::Limits&) noexcept -> Admission;
} // namespace epycbench::runtime::detail::stage_b_policy
