#include "epycbench/scenario/kernel_binding_projection.hpp"

#include "scenario/kernel_resolution.hpp"

#include <exception>
#include <map>
#include <set>
#include <utility>

namespace epycbench::scenario {

auto project_kernel_bindings(const StructurallyValidScenario& scenario) noexcept
    -> std::expected<ScenarioKernelBindingProjectionResult, core::Error> {
    try {
        detail::KernelDiagnostics diagnostics{scenario.authored()};
        std::map<std::string, kernel::KernelId> kernels;
        std::set<std::string> failed;
        const auto& workloads = scenario.value().at("workloads");
        for (auto iterator = workloads.begin();
             iterator != workloads.end() && !diagnostics.capped(); ++iterator) {
            auto resolved = detail::resolve_workload_kernel_id(iterator.value(), iterator.key(),
                                                               diagnostics);
            if (!resolved) return std::unexpected{std::move(resolved.error())};
            if (*resolved)
                kernels.emplace(iterator.key(), (**resolved).id);
            else
                failed.insert(iterator.key());
        }

        std::vector<ScenarioKernelBinding> bindings;
        const auto& experiments = scenario.value().at("experiments");
        for (std::size_t index = 0; index < experiments.size() && !diagnostics.capped(); ++index) {
            const auto& experiment = experiments.at(index);
            auto resolved = detail::resolve_experiment_workload_reference(
                experiment, index, workloads, kernels, failed, diagnostics);
            if (!resolved) return std::unexpected{std::move(resolved.error())};
            if (!*resolved) continue;
            auto experiment_id = ScenarioIdentifier::parse(experiment.at("id").get<std::string>());
            auto workload_id = ScenarioIdentifier::parse((**resolved).first);
            if (!experiment_id || !workload_id)
                return std::unexpected{detail::kernel_projection_internal_error(
                    "structured_identifier_invariant", "project_kernel_bindings")};
            bindings.push_back({std::move(*experiment_id), std::move(*workload_id),
                                *(**resolved).second});
        }
        auto values = diagnostics.take();
        if (!values.empty())
            return ScenarioKernelBindingProjectionResult{.bindings = std::nullopt,
                                                          .diagnostics = std::move(values)};
        return ScenarioKernelBindingProjectionResult{
            .bindings = ScenarioKernelBindings{std::move(bindings)}, .diagnostics = {}};
    } catch (const std::exception&) {
        return std::unexpected{detail::kernel_projection_internal_error(
            "contained_exception", "project_kernel_bindings")};
    } catch (...) {
        return std::unexpected{detail::kernel_projection_internal_error(
            "contained_unknown_failure", "project_kernel_bindings")};
    }
}

} // namespace epycbench::scenario
