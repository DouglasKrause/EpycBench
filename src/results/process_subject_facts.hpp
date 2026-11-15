#pragma once
#include "results/private_error_owner.hpp"
#include "results/build_manifest_owner.hpp"
#include <compare>
#include <variant>
#include <vector>
namespace epycbench::results::process_subject {
enum class Domain { supplied_observation, synthetic_execution };
enum class Producer { controller, runner, results };
struct NoCreation {};
struct ControllerCreated {};
struct RunnerCreated { std::uint64_t pair_ordinal, controller_sequence; };
using Creation = std::variant<NoCreation, ControllerCreated, RunnerCreated>;
enum class Category { topology, system_controls, kernel_identity, pu_identity, frequency, anonymous_thp, process_thp, authored_source, base_build, preflight_temperature };
enum class Selector { owner_preparation, smt_control, smt_active, numa_mask, thp_enabled, thp_defrag,
    sysname, release, version, signature, microcode, uncovered_cpu, enumeration, page_size, process_observation, source_acquisition, base_build_acquisition, cpu_control_observation };
enum class FailureReason { none, read_failed, malformed_source, unsupported_value };
struct Target {
    Category category;
    Selector selector{Selector::owner_preparation};
    std::uint64_t ordinal{}, key{};
    FailureReason reason{FailureReason::none};
    auto operator<=>(const Target&) const = default;
};
struct FailureFact {
    private_error::Owner error;
    Producer producer;
    // Absence is run scope / independent primary, respectively.
    std::optional<std::uint64_t> process_sequence;
    std::optional<std::uint64_t> secondary_of;
    std::vector<Target> targets;
    std::optional<std::uint64_t> secondary_creation_ordinal{};
};
enum class BuildState { not_attempted, not_obtained, available };
struct BaseBuild {
    BuildState state;
    std::optional<build_manifest::Owner> manifest{};
    std::optional<std::uint64_t> acquisition_failure{};
};
namespace detail {
[[nodiscard]] auto valid_target(const Target&, const FailureFact&) noexcept -> bool;
}
} // namespace epycbench::results::process_subject
