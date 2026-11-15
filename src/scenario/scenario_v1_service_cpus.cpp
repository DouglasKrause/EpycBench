#include "scenario/scenario_v1_service_cpus.hpp"
#include "core/json_ownership.hpp"

#include "scenario/resource_selector.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <utility>

namespace epycbench::scenario::detail {
namespace {

using Json = nlohmann::ordered_json;

auto error_context(std::string classification, std::string underlying = {}) -> nlohmann::json {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};
    context["classification"] = std::move(classification);
    if (!underlying.empty()) context["underlying_code"] = std::move(underlying);
    return nlohmann::json(std::move(context));
}

auto location_for(const ResourceResolvedScenario& scenario, const std::string& pointer)
    -> std::optional<SourceLocation> {
    const auto& locations = scenario.default_expanded()
                                .unit_normalized()
                                .structurally_valid()
                                .authored()
                                .locations();
    if (const auto found = locations.find(pointer); found != locations.end()) {
        return found->second.value;
    }
    return std::nullopt;
}

class DiagnosticAccumulator {
  public:
    explicit DiagnosticAccumulator(const ResourceResolvedScenario& scenario) : scenario_{scenario} {}

    void add(std::string code, std::string pointer, std::string classification,
             std::map<std::string, std::string> context = {},
             std::optional<SourceLocation> location = std::nullopt) {
        if (capped_) return;
        if (diagnostics_.size() == 63) {
            diagnostics_.push_back({core::StableErrorCode{"scenario.service_cpus.diagnostic_limit"},
                                    "", std::nullopt,
                                    {{"classification", "diagnostic_limit"}}});
            capped_ = true;
            return;
        }
        context.insert_or_assign("classification", std::move(classification));
        diagnostics_.push_back({core::StableErrorCode{std::move(code)}, pointer,
                                location ? std::move(location) : location_for(scenario_, pointer),
                                std::move(context)});
    }

    void translate(ScenarioDiagnostic diagnostic) {
        auto code = diagnostic.code.value();
        if (code == "scenario.resources.unknown_reference") {
            code = "scenario.service_cpus.unknown_worker_set";
        } else if (code == "scenario.resources.duplicate_resource") {
            code = "scenario.service_cpus.invalid_selector";
        } else if (code.starts_with("scenario.resources.")) {
            code.replace(0, std::string{"scenario.resources"}.size(), "scenario.service_cpus");
        }
        const auto classification = diagnostic.safe_context.contains("classification")
                                        ? diagnostic.safe_context.at("classification")
                                        : std::string{"selector_failure"};
        diagnostic.safe_context.erase("classification");
        add(std::move(code), std::move(diagnostic.json_pointer), classification,
            std::move(diagnostic.safe_context), std::move(diagnostic.location));
    }

    [[nodiscard]] auto capped() const noexcept -> bool { return capped_; }
    [[nodiscard]] auto take() noexcept -> std::vector<ScenarioDiagnostic> {
        return std::move(diagnostics_);
    }

  private:
    const ResourceResolvedScenario& scenario_;
    std::vector<ScenarioDiagnostic> diagnostics_;
    bool capped_{};
};

auto overlap_pointer(const Json& declaration, const std::string& base,
                     core::LinuxCpuId cpu) -> std::string {
    if (!declaration.contains("pus")) return base;
    const auto& pus = declaration.at("pus");
    for (std::size_t index = 0; index < pus.size(); ++index) {
        if (pus.at(index).get<std::uint32_t>() == cpu.value()) {
            return base + "/pus/" + std::to_string(index);
        }
    }
    return base;
}

auto normalized_service_declaration(const Json& declaration) -> Json {
    auto normalized = core::detail::clone_json(declaration);
    core::detail::JsonCleanup normalized_cleanup{normalized};
    if (!normalized.contains("select"))
        return Json(std::move(normalized));
    auto& select = normalized.at("select");
    if (!select.contains("smt"))
        core::detail::set_json(select, "smt", "one-per-core");
    if (!select.contains("count"))
        core::detail::set_json(select, "count", std::uint64_t{1});
    if (!select.contains("ordering"))
        core::detail::set_json(select, "ordering", "ascending-pu");
    return Json(std::move(normalized));
}

auto active_workers(const ResourceResolvedScenario& scenario)
    -> std::pair<std::set<core::LinuxCpuId>, std::set<topology::StableCoreId>> {
    std::set<core::LinuxCpuId> cpus;
    std::set<topology::StableCoreId> cores;
    for (const auto& reference : scenario.experiment_workers()) {
        const auto found = scenario.worker_sets().find(reference.worker_set_name);
        if (found == scenario.worker_sets().end()) continue;
        for (const auto& pu : found->second.processing_units) {
            cpus.insert(pu.linux_cpu);
            cores.insert(pu.core_id);
        }
    }
    return {std::move(cpus), std::move(cores)};
}

auto service_pool(const std::string& name, const std::string& pointer,
                  std::vector<ResolvedProcessingUnit> selected) -> ResolvedServiceCpuPool {
    core::CpuSet cpus;
    std::set<topology::StableCoreId> cores;
    std::vector<ResolvedServiceCpu> records;
    records.reserve(selected.size());
    for (auto& pu : selected) {
        cpus = cpus.set_union(core::CpuSet{pu.linux_cpu});
        cores.insert(pu.core_id);
        records.push_back({std::move(pu.stable_id), std::move(pu.core_id), pu.linux_cpu});
    }
    return {name, std::move(records), std::move(cpus),
            {std::make_move_iterator(cores.begin()), std::make_move_iterator(cores.end())}, pointer};
}

class ServiceResolver {
  public:
    ServiceResolver(const ResourceResolvedScenario& scenario, const ResourceResolutionContext& context)
        : scenario_{scenario}, context_{context}, diagnostics_{scenario} {
        auto [cpus, cores] = active_workers(scenario);
        worker_cpus_ = std::move(cpus);
        worker_cores_ = std::move(cores);
    }

    auto run() -> std::expected<ServiceCpuPayload, core::Error> {
        const auto& root = scenario_.default_expanded().value();
        if (!root.contains("resources") ||
            !root.at("resources").contains("service_cpus")) {
            return finish();
        }
        const auto& declarations = root.at("resources").at("service_cpus");
        for (auto it = declarations.begin(); it != declarations.end() && !diagnostics_.capped(); ++it) {
            resolve_pool(it.key(), it.value());
        }
        return finish();
    }

  private:
    void resolve_pool(const std::string& name, const Json& authored) {
        const auto base = "/resources/service_cpus/" + name;
        auto declaration = normalized_service_declaration(authored);
        core::detail::JsonCleanup declaration_cleanup{declaration};
        if (declaration.contains("select")) {
            const auto& select = declaration.at("select");
            if (core::detail::json_string_equal(select.at("ordering"), "random")) {
                diagnostics_.add("scenario.service_cpus.unsupported_ordering",
                                 base + "/select/ordering", "random_not_supported");
                return;
            }
            if (select.contains("exclude") && select.at("exclude").contains("worker_sets")) {
                const auto& references = select.at("exclude").at("worker_sets");
                bool missing = false;
                for (std::size_t index = 0; index < references.size(); ++index) {
                    const auto& reference = references.at(index).get_ref<const std::string&>();
                    if (!scenario_.worker_sets().contains(reference)) {
                        diagnostics_.add("scenario.service_cpus.unknown_worker_set",
                                         base + "/select/exclude/worker_sets/" +
                                             std::to_string(index),
                                         "unknown_worker_set",
                                         {{"reference_category", "worker_set"}});
                        missing = true;
                    }
                }
                if (missing) return;
            }
        }

        auto selected = resolve_v1_selection(scenario_.default_expanded(), context_, declaration,
                                             base, name, scenario_.worker_sets());
        if (!selected) {
            throw std::runtime_error{selected.error().code.value()};
        }
        if (!selected->diagnostics.empty()) {
            for (auto& diagnostic : selected->diagnostics) diagnostics_.translate(std::move(diagnostic));
            return;
        }
        if (!selected->processing_units || selected->processing_units->empty()) {
            diagnostics_.add("scenario.service_cpus.empty_selection", base, "named_pool_empty");
            return;
        }
        if (overlaps_workers(authored, base, *selected->processing_units)) return;
        if (overlaps_services(authored, base, *selected->processing_units)) return;

        auto pool = service_pool(name, base, std::move(*selected->processing_units));
        for (const auto& pu : pool.processing_units) {
            service_cpus_.insert(pu.linux_cpu);
            service_cores_.insert(pu.core_id);
        }
        pools_.emplace(name, std::move(pool));
    }

    auto overlaps_workers(const Json& declaration, const std::string& base,
                          const std::vector<ResolvedProcessingUnit>& selected) -> bool {
        bool conflict = false;
        for (const auto& pu : selected) {
            const auto pointer = overlap_pointer(declaration, base, pu.linux_cpu);
            if (worker_cpus_.contains(pu.linux_cpu)) {
                diagnostics_.add("scenario.service_cpus.worker_overlap", pointer, "same_pu");
                conflict = true;
            } else if (worker_cores_.contains(pu.core_id)) {
                diagnostics_.add("scenario.service_cpus.core_overlap", pointer,
                                 "worker_same_core");
                conflict = true;
            }
        }
        return conflict;
    }

    auto overlaps_services(const Json& declaration, const std::string& base,
                           const std::vector<ResolvedProcessingUnit>& selected) -> bool {
        bool conflict = false;
        for (const auto& pu : selected) {
            const auto pointer = overlap_pointer(declaration, base, pu.linux_cpu);
            if (service_cpus_.contains(pu.linux_cpu)) {
                diagnostics_.add("scenario.service_cpus.worker_overlap", pointer, "same_pu");
                conflict = true;
            } else if (service_cores_.contains(pu.core_id)) {
                diagnostics_.add("scenario.service_cpus.core_overlap", pointer,
                                 "service_pool_same_core");
                conflict = true;
            }
        }
        return conflict;
    }

    auto finish() -> ServiceCpuPayload {
        core::CpuSet cpus;
        for (const auto cpu : service_cpus_) cpus = cpus.set_union(core::CpuSet{cpu});
        return {.pools = std::move(pools_),
                .reserved_cpus = std::move(cpus),
                .reserved_cores = {service_cores_.begin(), service_cores_.end()},
                .diagnostics = diagnostics_.take()};
    }

    const ResourceResolvedScenario& scenario_;
    const ResourceResolutionContext& context_;
    DiagnosticAccumulator diagnostics_;
    std::set<core::LinuxCpuId> worker_cpus_;
    std::set<topology::StableCoreId> worker_cores_;
    std::set<core::LinuxCpuId> service_cpus_;
    std::set<topology::StableCoreId> service_cores_;
    std::map<std::string, ResolvedServiceCpuPool> pools_;
};

} // namespace

auto service_cpus_internal_error(std::string classification, std::string underlying) -> core::Error {
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.service_cpus.internal_failure"},
            .message = "Service CPU resolution failed internally",
            .operation = "resolve_service_cpus",
            .os_error = std::nullopt,
            .context = error_context(std::move(classification), std::move(underlying)),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto resolve_v1_service_cpus(const ResourceResolvedScenario& scenario,
                             const ResourceResolutionContext& context)
    -> std::expected<ServiceCpuPayload, core::Error> {
    try {
        return ServiceResolver{scenario, context}.run();
    } catch (const std::exception&) {
        return std::unexpected{service_cpus_internal_error("selector_exception")};
    } catch (...) {
        return std::unexpected{service_cpus_internal_error("selector_unknown_failure")};
    }
}

} // namespace epycbench::scenario::detail
