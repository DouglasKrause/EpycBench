#pragma once

#include "epycbench/kernel/kernel_id.hpp"
#include "epycbench/scenario/identifier.hpp"
#include "epycbench/scenario/structural_validation.hpp"

#include <compare>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

namespace epycbench::scenario {

struct ScenarioKernelBinding {
    ScenarioIdentifier experiment_id;
    ScenarioIdentifier workload_id;
    kernel::KernelId kernel_id;

    auto operator<=>(const ScenarioKernelBinding&) const = default;
};

class ScenarioKernelBindings {
  public:
    [[nodiscard]] auto bindings() const noexcept
        -> const std::vector<ScenarioKernelBinding>& {
        return bindings_;
    }

  private:
    explicit ScenarioKernelBindings(std::vector<ScenarioKernelBinding> bindings)
        : bindings_{std::move(bindings)} {}

    std::vector<ScenarioKernelBinding> bindings_;

    friend auto project_kernel_bindings(const StructurallyValidScenario&) noexcept
        -> std::expected<struct ScenarioKernelBindingProjectionResult, core::Error>;
};

struct ScenarioKernelBindingProjectionResult {
    std::optional<ScenarioKernelBindings> bindings;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto project_kernel_bindings(const StructurallyValidScenario& scenario) noexcept
    -> std::expected<ScenarioKernelBindingProjectionResult, core::Error>;

} // namespace epycbench::scenario
