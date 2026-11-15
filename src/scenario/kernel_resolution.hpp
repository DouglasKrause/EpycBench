#pragma once

#include "epycbench/scenario/structural_validation.hpp"
#include "epycbench/kernel/kernel_id.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace epycbench::scenario::detail {

class KernelDiagnostics {
  public:
    explicit KernelDiagnostics(const AuthoredScenario& scenario) : scenario_{scenario} {}

    void add(std::string code, std::string pointer, std::string classification,
             std::map<std::string, std::string> context = {}, bool mapping_key = false);
    [[nodiscard]] auto capped() const noexcept -> bool { return capped_; }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return values_.size(); }
    [[nodiscard]] auto take() noexcept -> std::vector<ScenarioDiagnostic>;

  private:
    const AuthoredScenario& scenario_;
    std::vector<ScenarioDiagnostic> values_;
    bool capped_{};
};

struct ResolvedKernelToken {
    std::string token;
    kernel::KernelId id;
};

[[nodiscard]] auto kernel_projection_internal_error(std::string classification,
                                                     std::string operation,
                                                     std::string underlying = {}) -> core::Error;

[[nodiscard]] auto resolve_workload_kernel_id(const nlohmann::ordered_json& workload,
                                               const std::string& name,
                                               KernelDiagnostics& diagnostics)
    -> std::expected<std::optional<ResolvedKernelToken>, core::Error>;

template <typename ResolvedWorkload>
[[nodiscard]] auto resolve_experiment_workload_reference(
    const nlohmann::ordered_json& experiment, std::size_t experiment_index,
    const nlohmann::ordered_json& declarations,
    const std::map<std::string, ResolvedWorkload>& resolved,
    const std::set<std::string>& failed, KernelDiagnostics& diagnostics)
    -> std::expected<std::optional<std::pair<std::string, const ResolvedWorkload*>>, core::Error> {
    if (!experiment.at("workload").is_string())
        return std::unexpected{kernel_projection_internal_error(
            "workload_reference_shape", "resolve_experiment_workload_reference")};
    const auto name = experiment.at("workload").get<std::string>();
    const auto found = resolved.find(name);
    if (found != resolved.end()) return std::pair{name, &found->second};
    if (!declarations.contains(name)) {
        diagnostics.add("scenario.kernel.unknown_workload",
                        "/experiments/" + std::to_string(experiment_index) + "/workload",
                        "unknown_workload");
    } else if (!failed.contains(name)) {
        return std::unexpected{kernel_projection_internal_error(
            "unclassified_failed_workload", "resolve_experiment_workload_reference")};
    }
    return std::nullopt;
}

} // namespace epycbench::scenario::detail
