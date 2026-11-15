#include "scenario/scenario_v1_kernels.hpp"
#include "core/json_ownership.hpp"

#include "epycbench/core/seed.hpp"
#include "scenario/byte_quantity_parser.hpp"
#include "scenario/kernel_resolution.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace epycbench::scenario::detail {
namespace {

using Json = nlohmann::ordered_json;

struct ParsedWorkload {
    kernel::KernelId id;
    kernel::KernelConfiguration configuration;
};

class Dispatcher {
  public:
    explicit Dispatcher(const MemoryPlacementResolvedScenario& scenario)
        : scenario_{scenario}, diagnostics_{scenario.service_cpu_resolved()
                                               .resource_resolved()
                                               .default_expanded()
                                               .unit_normalized()
                                               .structurally_valid()
                                               .authored()} {}

    auto run() -> std::expected<KernelDispatchPayload, core::Error> {
        const auto& value = effective_scenario();
        const auto& workload_values = value.at("workloads");
        for (auto iterator = workload_values.begin();
             iterator != workload_values.end() && !diagnostics_.capped(); ++iterator) {
            auto dispatched = dispatch_workload(iterator.key(), iterator.value());
            if (!dispatched) return std::unexpected{std::move(dispatched.error())};
        }
        const auto& experiments = value.at("experiments");
        for (std::size_t index = 0; index < experiments.size() && !diagnostics_.capped(); ++index) {
            auto bound = bind_experiment(experiments.at(index), index);
            if (!bound) return std::unexpected{std::move(bound.error())};
        }
        return KernelDispatchPayload{.workloads = std::move(workloads_),
                                     .bindings = std::move(bindings_),
                                     .diagnostics = diagnostics_.take()};
    }

  private:
    auto effective_scenario() const -> const Json& {
        return scenario_.service_cpu_resolved()
            .resource_resolved()
            .default_expanded()
            .value();
    }

    auto parameters(const Json& workload) const -> const Json& {
        static const Json empty = Json::object();
        const auto found = workload.find("parameters");
        return found == workload.end() ? empty : *found;
    }

    auto parse_working_set(const Json& parameters_value, const std::string& base,
                           kernel::KernelId id, bool parameters_present)
        -> std::optional<core::ByteCount> {
        const auto found = parameters_value.find("working_set");
        if (found == parameters_value.end()) {
            diagnostics_.add("scenario.kernel.missing_parameter",
                             parameters_present ? base + "/parameters" : base,
                             "missing_working_set", {{"parameter", "working_set"}});
            return std::nullopt;
        }
        const auto pointer = base + "/parameters/working_set";
        if (!found->is_string()) {
            diagnostics_.add("scenario.kernel.invalid_parameter_type", pointer,
                             "working_set_type",
                             {{"parameter", "working_set"}, {"expected_type", "string"}});
            return std::nullopt;
        }
        auto parsed = parse_byte_quantity(found->get_ref<const std::string&>());
        if (!parsed) {
            diagnostics_.add("scenario.kernel.invalid_quantity", pointer,
                             std::string{byte_quantity_classification(parsed.error().failure)},
                             {{"parameter", "working_set"}});
            return std::nullopt;
        }
        if (parsed->value() == 0U) {
            diagnostics_.add("scenario.kernel.invalid_parameter", pointer, "working_set_zero",
                             {{"parameter", "working_set"}});
            return std::nullopt;
        }
        if (parsed->value() % 64U != 0U) {
            diagnostics_.add("scenario.kernel.invalid_parameter", pointer,
                             "working_set_not_multiple_of_64",
                             {{"parameter", "working_set"}});
            return std::nullopt;
        }
        if (id == kernel::KernelId::dependent_load_latency_v1 && parsed->value() < 128U) {
            diagnostics_.add("scenario.kernel.invalid_parameter", pointer,
                             "latency_working_set_too_small",
                             {{"parameter", "working_set"}});
            return std::nullopt;
        }
        return *parsed;
    }

    void unknown_parameters(const Json& parameters_value, const std::string& base,
                            kernel::KernelId id) {
        for (auto iterator = parameters_value.begin();
             iterator != parameters_value.end() && !diagnostics_.capped(); ++iterator) {
            const auto known = iterator.key() == "working_set" ||
                               (id == kernel::KernelId::dependent_load_latency_v1 &&
                                iterator.key() == "chain_seed");
            if (!known) {
                const auto pointer = base + "/parameters/" + iterator.key();
                diagnostics_.add("scenario.kernel.unknown_parameter", pointer,
                                 "unknown_parameter", {{"parameter", iterator.key()}}, true);
            }
        }
    }

    auto dispatch_workload(const std::string& name, const Json& workload)
        -> std::expected<void, core::Error> {
        const auto base = "/workloads/" + name;
        if (!workload.is_object() || !workload.contains("kernel") ||
            !workload.at("kernel").is_string()) {
            return std::unexpected{kernel_dispatch_internal_error("normalized_workload_shape")};
        }
        auto identity = resolve_workload_kernel_id(workload, name, diagnostics_);
        if (!identity) return std::unexpected{std::move(identity.error())};
        if (!*identity) {
            failed_.insert(name);
            return {};
        }
        const auto& token = (**identity).token;
        const auto id = (**identity).id;
        const auto& params = parameters(workload);
        if (!params.is_object())
            return std::unexpected{kernel_dispatch_internal_error("normalized_parameter_shape")};

        const auto before = diagnostic_count();
        auto working_set = parse_working_set(params, base, id, workload.contains("parameters"));
        std::optional<kernel::PointerCycleSeedIntent> seed_intent;
        if (id == kernel::KernelId::dependent_load_latency_v1) {
            const auto seed = params.find("chain_seed");
            if (seed == params.end()) {
                seed_intent = kernel::PointerCycleSeedIntent::derive_per_experiment();
            } else if (!seed->is_number_unsigned()) {
                diagnostics_.add("scenario.kernel.invalid_parameter_type",
                                 base + "/parameters/chain_seed", "chain_seed_type",
                                 {{"parameter", "chain_seed"},
                                  {"expected_type", "unsigned_integer"}});
            } else {
                seed_intent = kernel::PointerCycleSeedIntent::explicit_value(
                    seed->get<std::uint64_t>());
            }
        }
        unknown_parameters(params, base, id);
        if (!working_set || diagnostic_count() != before) {
            failed_.insert(name);
            return {};
        }

        std::optional<ParsedWorkload> parsed;
        if (id == kernel::KernelId::sequential_read_bandwidth_v1) {
            auto config = kernel::SequentialReadBandwidthConfig::create(*working_set);
            if (!config)
                return std::unexpected{kernel_dispatch_internal_error(
                    "kernel_contract_failure", config.error().code.value())};
            parsed = ParsedWorkload{id, std::move(*config)};
        } else {
            auto config = kernel::DependentLoadLatencyConfig::create(*working_set, *seed_intent);
            if (!config)
                return std::unexpected{kernel_dispatch_internal_error(
                    "kernel_contract_failure", config.error().code.value())};
            parsed = ParsedWorkload{id, std::move(*config)};
        }
        workloads_.emplace(name, DispatchedWorkload{name, parsed->id, token,
                                                     std::move(parsed->configuration), base});
        return {};
    }

    auto diagnostic_count() const noexcept -> std::size_t { return diagnostics_.size(); }

    auto working_set(const DispatchedWorkload& workload) const noexcept -> core::ByteCount {
        if (const auto* bandwidth =
                std::get_if<kernel::SequentialReadBandwidthConfig>(&workload.configuration))
            return bandwidth->working_set_per_worker();
        return std::get<kernel::DependentLoadLatencyConfig>(workload.configuration).working_set();
    }

    auto worker_count(std::size_t experiment_index) const
        -> std::expected<std::size_t, core::Error> {
        const auto& resources = scenario_.service_cpu_resolved().resource_resolved();
        const auto reference = std::ranges::find_if(
            resources.experiment_workers(), [experiment_index](const auto& item) {
                return item.experiment_index == experiment_index;
            });
        if (reference == resources.experiment_workers().end())
            return std::unexpected{kernel_dispatch_internal_error("worker_binding_missing")};
        const auto found = resources.worker_sets().find(reference->worker_set_name);
        if (found == resources.worker_sets().end())
            return std::unexpected{kernel_dispatch_internal_error("worker_set_missing")};
        return found->second.processing_units.size();
    }

    auto resolved_seed(const Json& experiment, const DispatchedWorkload& workload)
        -> std::expected<std::pair<std::optional<std::uint64_t>,
                                  std::optional<PointerCycleSeedOrigin>>, core::Error> {
        if (workload.kernel_id != kernel::KernelId::dependent_load_latency_v1)
            return std::pair{std::optional<std::uint64_t>{},
                             std::optional<PointerCycleSeedOrigin>{}};
        const auto& config = std::get<kernel::DependentLoadLatencyConfig>(workload.configuration);
        if (config.pointer_cycle_seed().mode() ==
            kernel::PointerCycleSeedMode::explicit_value) {
            return std::pair{config.pointer_cycle_seed().value(),
                             std::optional{PointerCycleSeedOrigin::explicit_value}};
        }
        const auto& root = effective_scenario().at("random").at("seed");
        if (!root.is_number_unsigned() || !experiment.at("id").is_string())
            return std::unexpected{kernel_dispatch_internal_error("seed_input_invariant")};
        const core::SeedDerivationKey key{
            core::SeedDomain::pointer_cycle,
            {{"profile", std::string{kernel::pointer_cycle_seed_profile}},
             {"kernel", std::string{kernel::kernel_id_token(workload.kernel_id)}},
             {"workload", workload.workload_id},
             {"experiment", experiment.at("id").get<std::string>()}}};
        auto seed = core::derive_seed(root.get<std::uint64_t>(), key);
        if (!seed)
            return std::unexpected{kernel_dispatch_internal_error("seed_derivation",
                                                                  seed.error().code.value())};
        return std::pair{std::optional{seed->value()},
                         std::optional{PointerCycleSeedOrigin::derived_value}};
    }

    void check_page_size(const DispatchedWorkload& workload, core::ByteCount page_size,
                         std::size_t experiment_index, std::optional<std::size_t> variant_index) {
        if (working_set(workload) >= page_size) return;
        std::map<std::string, std::string> context{
            {"experiment_index", std::to_string(experiment_index)}};
        if (variant_index)
            context.emplace("variant_index", std::to_string(*variant_index));
        diagnostics_.add("scenario.kernel.unsupported_configuration",
                         workload.source_pointer + "/parameters/working_set",
                         "working_set_smaller_than_page", std::move(context));
    }

    auto bind_experiment(const Json& experiment, std::size_t experiment_index)
        -> std::expected<void, core::Error> {
        if (!experiment.at("workload").is_string())
            return std::unexpected{kernel_dispatch_internal_error("workload_reference_shape")};
        const auto pointer = "/experiments/" + std::to_string(experiment_index) + "/workload";
        auto resolved = resolve_experiment_workload_reference(
            experiment, experiment_index, effective_scenario().at("workloads"), workloads_,
            failed_, diagnostics_);
        if (!resolved) return std::unexpected{std::move(resolved.error())};
        if (!*resolved) return {};
        const auto& name = (**resolved).first;
        const auto* workload = (**resolved).second;

        auto seed = resolved_seed(experiment, *workload);
        if (!seed) return std::unexpected{std::move(seed.error())};
        std::vector<DeferredKernelConstraint> deferred;
        if (core::detail::json_string_equal(experiment.at("type"), "comparison")) {
            for (const auto& memory : scenario_.comparison_memory()) {
                if (memory.experiment_index == experiment_index)
                    check_page_size(*workload, std::visit([](const auto& placement) {
                                        return placement.page_size;
                                    }, memory.placement),
                                    experiment_index, memory.variant_index);
            }
            if (workload->kernel_id ==
                kernel::KernelId::dependent_load_latency_v1) {
                auto count = worker_count(experiment_index);
                if (!count) return std::unexpected{std::move(count.error())};
                if (*count != 1U)
                    diagnostics_.add("scenario.kernel.unsupported_configuration", pointer,
                                     "latency_requires_one_worker",
                                     {{"experiment_index", std::to_string(experiment_index)}});
            }
        } else {
            const auto memory = std::ranges::find_if(
                scenario_.matrix_memory_templates(), [experiment_index](const auto& item) {
                    return item.experiment_index == experiment_index;
                });
            if (memory == scenario_.matrix_memory_templates().end()) {
                deferred.push_back(
                    DeferredKernelConstraint::working_set_at_least_resolved_page_size);
            } else {
                check_page_size(*workload, memory->page_size, experiment_index,
                                std::nullopt);
            }
            if (workload->kernel_id ==
                kernel::KernelId::dependent_load_latency_v1)
                deferred.push_back(DeferredKernelConstraint::exactly_one_worker);
        }
        bindings_.push_back({experiment_index, name, seed->first, seed->second,
                             std::move(deferred), pointer});
        return {};
    }

    const MemoryPlacementResolvedScenario& scenario_;
    KernelDiagnostics diagnostics_;
    std::map<std::string, DispatchedWorkload> workloads_;
    std::set<std::string> failed_;
    std::vector<ExperimentWorkloadBinding> bindings_;
};

} // namespace

auto kernel_dispatch_internal_error(std::string classification, std::string underlying)
    -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup cleanup{context};
    context["classification"] = std::move(classification);
    if (!underlying.empty()) context["underlying_code"] = std::move(underlying);
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.kernel.internal_failure"},
            .message = "Kernel parameter dispatch failed internally",
            .operation = "dispatch_kernel_parameters",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto dispatch_v1_kernels(const MemoryPlacementResolvedScenario& scenario)
    -> std::expected<KernelDispatchPayload, core::Error> {
    try {
        return Dispatcher{scenario}.run();
    } catch (const std::exception&) {
        return std::unexpected{kernel_dispatch_internal_error("normalized_scenario_invariant")};
    } catch (...) {
        return std::unexpected{kernel_dispatch_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario::detail
