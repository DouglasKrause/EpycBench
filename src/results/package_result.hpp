#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/statistics/block_bootstrap.hpp"
#include <optional>

namespace epycbench::kernel::packages {
class Catalog;
}

namespace epycbench::results {
inline constexpr std::string_view package_result_schema = "epycbench.package-result/v3";
inline constexpr std::string_view previous_package_result_schema = "epycbench.package-result/v2";
struct PackageResult {
    std::string run_id, scenario_digest, resolved_plan_digest, physical_topology,
        effective_topology;
    std::string execution, validity, conclusion;
    core::Sha256Digest digest, build;
    std::size_t eligible_blocks;
    nlohmann::json document, summary, scaling;
    std::optional<std::uint64_t> calibrated_repetitions{};
    std::optional<core::Sha256Digest> observed_inputs_sha256{};
    nlohmann::json mixed_blocks{};
};

class PackagePrecision {
  public:
    explicit PackagePrecision(const nlohmann::json& execution);
    [[nodiscard]] auto observe(std::size_t ordinal, std::uint64_t repetitions,
                               std::optional<std::pair<std::uint64_t, std::uint64_t>> durations)
        -> std::expected<std::optional<nlohmann::json>, core::Error>;
    [[nodiscard]] bool attained() const noexcept { return attained_; }
    [[nodiscard]] bool enough_eligible_blocks() const noexcept { return effects_.size() >= minimum_valid_blocks_; }

  private:
    std::size_t minimum_blocks_, minimum_valid_blocks_;
    std::uint64_t seed_;
    double confidence_, target_;
    std::vector<statistics::PairedLogEffect> effects_;
    bool attained_{};
};

[[nodiscard]] auto package_precision_stop_reason(bool calibrated, bool qualified, bool attained,
                                                 const nlohmann::json& document) -> std::string_view;

[[nodiscard]] bool is_package_result(std::string_view);
[[nodiscard]] auto qualified_package_intervals(const nlohmann::json& plan,
                                               const kernel::packages::Catalog&,
                                               const nlohmann::json& block,
                                               std::uint64_t repetitions, std::size_t ordinal,
                                               std::uint64_t previous_end, bool warm, bool supplied,
                                               bool calibrating = false,
                                               bool* factual_monitoring_rejection = nullptr)
    -> std::expected<std::vector<std::pair<std::uint64_t, std::uint64_t>>, core::Error>;
[[nodiscard]] auto validate_package_result(std::string_view)
    -> std::expected<PackageResult, core::Error>;
} // namespace epycbench::results
