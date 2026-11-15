#include "scenario/resource_selector.hpp"
#include "core/json_ownership.hpp"

#include "epycbench/core/seed.hpp"
#include "epycbench/topology/invariants.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace epycbench::scenario::detail {
namespace {

using Json = nlohmann::ordered_json;

struct PuView {
    const topology::ProcessingUnit* pu{};
    const topology::Core* core{};
    topology::StableProcessingUnitId stable_pu;
    topology::StableCoreId stable_core;
};

struct Selector {
    SelectorUnit unit{SelectorUnit::processing_unit};
    SelectorFilters filters;
    SelectorExclusions exclusions;
    SelectorSmtMode smt{SelectorSmtMode::one_per_core};
    std::vector<std::uint64_t> smt_indices;
    std::optional<std::uint64_t> count;
    SelectorOrdering ordering{SelectorOrdering::ascending_pu};
};

struct Unit {
    topology::StableCoreId core_id;
    std::vector<const PuView*> pus;
};

auto error_context(std::string classification, std::string underlying = {}) -> nlohmann::json {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};
    context["classification"] = std::move(classification);
    if (!underlying.empty()) context["underlying_code"] = std::move(underlying);
    return nlohmann::json(std::move(context));
}

template <typename Entry>
auto equal_entries(std::span<const Entry> left, std::span<const Entry> right) -> bool {
    return std::ranges::equal(left, right);
}

auto equal_ids(const topology::StableTopologyIds& left,
               const topology::StableTopologyIds& right) -> bool {
    return left.machine_id() == right.machine_id() &&
           equal_entries(left.packages(), right.packages()) &&
           equal_entries(left.dies(), right.dies()) && equal_entries(left.groups(), right.groups()) &&
           equal_entries(left.cores(), right.cores()) &&
           equal_entries(left.processing_units(), right.processing_units()) &&
           equal_entries(left.caches(), right.caches()) &&
           equal_entries(left.numa_nodes(), right.numa_nodes()) &&
           equal_entries(left.pci_devices(), right.pci_devices());
}

auto location_for(const DefaultExpandedScenario& scenario, const std::string& pointer)
    -> std::optional<SourceLocation> {
    const auto& locations = scenario.unit_normalized().structurally_valid().authored().locations();
    if (const auto found = locations.find(pointer); found != locations.end()) return found->second.value;
    return std::nullopt;
}

auto key_location_for(const DefaultExpandedScenario& scenario, const std::string& pointer)
    -> std::optional<SourceLocation> {
    const auto& locations = scenario.unit_normalized().structurally_valid().authored().locations();
    if (const auto found = locations.find(pointer); found != locations.end())
        return found->second.mapping_key.value_or(found->second.value);
    return std::nullopt;
}

class Diagnostics {
  public:
    explicit Diagnostics(const DefaultExpandedScenario& scenario) : scenario_{scenario} {}

    void add(std::string code, std::string pointer, std::string classification,
             std::map<std::string, std::string> extra = {}, bool key_location = false) {
        if (capped_) return;
        if (values_.size() == 63) {
            values_.push_back(ScenarioDiagnostic{
                .code = core::StableErrorCode{"scenario.resources.diagnostic_limit"},
                .json_pointer = "",
                .location = std::nullopt,
                .safe_context = {{"classification", "diagnostic_limit"}}});
            capped_ = true;
            return;
        }
        extra.insert_or_assign("classification", std::move(classification));
        values_.push_back(ScenarioDiagnostic{
            .code = core::StableErrorCode{std::move(code)},
            .json_pointer = pointer,
            .location = key_location ? key_location_for(scenario_, pointer)
                                     : location_for(scenario_, pointer),
            .safe_context = std::move(extra)});
    }

    [[nodiscard]] auto values() noexcept -> std::vector<ScenarioDiagnostic>& { return values_; }
    [[nodiscard]] auto capped() const noexcept -> bool { return capped_; }

  private:
    const DefaultExpandedScenario& scenario_;
    std::vector<ScenarioDiagnostic> values_;
    bool capped_{};
};

auto uints(const Json& array) -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> result;
    result.reserve(array.size());
    for (const auto& value : array) result.push_back(value.get<std::uint32_t>());
    return result;
}

auto parse_ordering(std::string_view text) -> std::optional<SelectorOrdering> {
    if (text == "ascending-pu") return SelectorOrdering::ascending_pu;
    if (text == "descending-pu") return SelectorOrdering::descending_pu;
    if (text == "compact-cache") return SelectorOrdering::compact_cache;
    if (text == "spread-cache") return SelectorOrdering::spread_cache;
    if (text == "compact-numa") return SelectorOrdering::compact_numa;
    if (text == "spread-numa") return SelectorOrdering::spread_numa;
    if (text == "random") return SelectorOrdering::random;
    return std::nullopt;
}

auto parse_selector(const Json& value) -> std::expected<Selector, core::Error> {
    try {
        Selector result;
        result.unit = core::detail::json_string_equal(value.at("unit"), "core")
                          ? SelectorUnit::core
                          : SelectorUnit::processing_unit;
        if (value.contains("where")) {
            const auto& where = value.at("where");
            if (where.contains("packages")) result.filters.packages = uints(where.at("packages"));
            if (where.contains("numa_nodes")) result.filters.numa_nodes = uints(where.at("numa_nodes"));
            if (where.contains("dies")) result.filters.dies = uints(where.at("dies"));
            if (where.contains("cache_domains")) result.filters.cache_domains = uints(where.at("cache_domains"));
            if (where.contains("allowed")) result.filters.allowed = where.at("allowed").get<bool>();
            if (where.contains("online")) result.filters.online = where.at("online").get<bool>();
        }
        if (value.contains("exclude")) {
            const auto& exclude = value.at("exclude");
            if (exclude.contains("pus"))
                for (const auto& item : exclude.at("pus"))
                    result.exclusions.processing_units.emplace_back(item.get<std::uint32_t>());
            if (exclude.contains("cores")) result.exclusions.cores = uints(exclude.at("cores"));
            if (exclude.contains("worker_sets"))
                result.exclusions.worker_sets = exclude.at("worker_sets").get<std::vector<std::string>>();
        }
        const auto& smt = value.at("smt");
        if (smt.is_string()) {
            result.smt = core::detail::json_string_equal(smt, "all")
                             ? SelectorSmtMode::all
                             : SelectorSmtMode::one_per_core;
        } else {
            result.smt = SelectorSmtMode::explicit_indices;
            result.smt_indices = smt.at("indices").get<std::vector<std::uint64_t>>();
        }
        if (value.contains("count") && value.at("count").is_number_unsigned())
            result.count = value.at("count").get<std::uint64_t>();
        const auto ordering = parse_ordering(value.at("ordering").get_ref<const std::string&>());
        if (!ordering) return std::unexpected{resources_internal_error("unknown_ordering_in_validated_input")};
        result.ordering = *ordering;
        return result;
    } catch (const std::exception&) {
        return std::unexpected{resources_internal_error("selector_shape_mismatch")};
    }
}

class Resolver {
  public:
    Resolver(const DefaultExpandedScenario& scenario, const ResourceResolutionContext& context,
             SelectionOverrides overrides = {})
        : scenario_{scenario}, context_{context}, diagnostics_{scenario},
          overrides_{std::move(overrides)} {}

    auto run() -> std::expected<ResolutionPayload, core::Error> {
        auto indexed = build_indexes();
        if (!indexed) return std::unexpected{std::move(indexed.error())};
        collect_declarations();
        find_graph_errors();
        for (const auto& name : declaration_order_) {
            if (diagnostics_.capped()) break;
            resolve_named(name);
        }
        bind_experiments();
        return ResolutionPayload{.worker_sets = std::move(resolved_),
                                 .experiment_workers = std::move(experiment_workers_),
                                 .matrix_templates = std::move(matrix_templates_),
                                 .diagnostics = std::move(diagnostics_.values())};
    }

    auto resolve_one(const Json& declaration, std::string pointer, std::string target_name,
                     const std::map<std::string, ResolvedWorkerSet>& worker_sets)
        -> std::expected<SingleSelectionPayload, core::Error> {
        auto indexed = build_indexes();
        if (!indexed) return std::unexpected{std::move(indexed.error())};
        resolved_ = worker_sets;
        declarations_.emplace(target_name, &declaration);
        auto selected = resolve_selection(declaration, std::move(pointer), target_name);
        return SingleSelectionPayload{.processing_units = std::move(selected),
                                      .diagnostics = std::move(diagnostics_.values())};
    }

  private:
    auto build_indexes() -> std::expected<void, core::Error> {
        const auto& effective = context_.topology.machine().effective_cpus;
        if (effective.availability() != topology::Availability::available || !effective.value())
            return std::unexpected{resources_internal_error("effective_cpu_set_unavailable")};
        for (const auto& core : context_.topology.cores()) cores_.emplace(core.id, &core);
        for (const auto& package : context_.topology.packages()) packages_.emplace(package.id, &package);
        for (const auto& die : context_.topology.dies()) dies_.emplace(die.id, &die);

        std::vector<std::pair<topology::StableCacheId, const topology::CacheDomain*>> caches;
        for (const auto& cache : context_.topology.caches()) {
            const auto id = context_.stable_ids.cache_id(cache.id);
            if (!id) return std::unexpected{resources_internal_error("missing_cache_stable_id")};
            caches.emplace_back(id->get(), &cache);
        }
        std::ranges::sort(caches, {}, &decltype(caches)::value_type::first);
        for (std::size_t index = 0; index < caches.size(); ++index)
            cache_ordinals_.emplace(static_cast<std::uint32_t>(index), caches[index].second);

        for (const auto& pu : context_.topology.processing_units()) {
            const auto core = cores_.find(pu.core_id);
            const auto stable_pu = context_.stable_ids.processing_unit_id(pu.id);
            const auto stable_core = context_.stable_ids.core_id(pu.core_id);
            if (core == cores_.end() || !stable_pu || !stable_core)
                return std::unexpected{resources_internal_error("missing_required_ownership")};
            auto [it, inserted] = all_pus_.emplace(
                pu.linux_cpu_id,
                PuView{&pu, core->second, stable_pu->get(), stable_core->get()});
            if (!inserted) return std::unexpected{resources_internal_error("duplicate_linux_cpu")};
            if (effective.value()->contains(pu.linux_cpu_id)) effective_pus_.push_back(&it->second);
        }
        std::ranges::sort(effective_pus_, [](const PuView* a, const PuView* b) {
            if (a->pu->linux_cpu_id != b->pu->linux_cpu_id)
                return a->pu->linux_cpu_id < b->pu->linux_cpu_id;
            return a->stable_pu < b->stable_pu;
        });
        return {};
    }

    void collect_declarations() {
        const auto& root = scenario_.value();
        if (!root.contains("resources") || !root.at("resources").contains("worker_sets")) return;
        const auto& sets = root.at("resources").at("worker_sets");
        for (auto it = sets.begin(); it != sets.end(); ++it) {
            const auto name = it.key();
            declaration_order_.push_back(name);
            declarations_.emplace(name, &it.value());
            std::vector<std::string> dependencies;
            if (it.value().contains("select")) {
                const auto& select = it.value().at("select");
                if (select.contains("exclude") && select.at("exclude").contains("worker_sets"))
                    dependencies = select.at("exclude").at("worker_sets").get<std::vector<std::string>>();
            }
            graph_.emplace(name, std::move(dependencies));
        }
    }

    void find_graph_errors() {
        for (const auto& name : declaration_order_) {
            const auto& deps = graph_.at(name);
            for (std::size_t index = 0; index < deps.size(); ++index) {
                if (!graph_.contains(deps[index])) {
                    failed_.insert(name);
                    diagnostics_.add("scenario.resources.unknown_reference",
                                     "/resources/worker_sets/" + name +
                                         "/select/exclude/worker_sets/" + std::to_string(index),
                                     "unknown_worker_set", {{"reference_category", "worker_set"}});
                }
            }
        }
        enum class Mark { visiting, done };
        std::map<std::string, Mark> marks;
        const auto visit = [&](const auto& self, const std::string& name) -> void {
            marks[name] = Mark::visiting;
            const auto& deps = graph_.at(name);
            std::vector<std::pair<std::string, std::size_t>> ordered;
            for (std::size_t i = 0; i < deps.size(); ++i)
                if (graph_.contains(deps[i])) ordered.emplace_back(deps[i], i);
            std::ranges::sort(ordered);
            for (const auto& [dep, authored_index] : ordered) {
                if (!marks.contains(dep)) self(self, dep);
                else if (marks.at(dep) == Mark::visiting) {
                    failed_.insert(name);
                    diagnostics_.add("scenario.resources.cyclic_reference",
                                     "/resources/worker_sets/" + name +
                                         "/select/exclude/worker_sets/" + std::to_string(authored_index),
                                     "dependency_cycle", {{"reference_category", "worker_set"}});
                }
                if (failed_.contains(dep)) failed_.insert(name);
            }
            marks[name] = Mark::done;
        };
        for (const auto& [name, unused] : graph_) {
            static_cast<void>(unused);
            if (!marks.contains(name)) visit(visit, name);
        }
    }

    auto resolve_named(const std::string& name) -> bool {
        if (resolved_.contains(name)) return true;
        if (failed_.contains(name)) return false;
        for (const auto& dependency : graph_.at(name))
            if (!resolve_named(dependency)) { failed_.insert(name); return false; }
        const auto pointer = "/resources/worker_sets/" + name;
        const auto selected = resolve_selection(*declarations_.at(name), pointer, name);
        if (!selected) { failed_.insert(name); return false; }
        core::CpuSet cpus;
        for (const auto& pu : *selected) cpus = cpus.set_union(core::CpuSet{pu.linux_cpu});
        resolved_.emplace(name, ResolvedWorkerSet{name, std::move(*selected), std::move(cpus), pointer});
        return true;
    }

    auto resolve_selection(const Json& declaration, const std::string& pointer,
                           const std::string& name)
        -> std::optional<std::vector<ResolvedProcessingUnit>> {
        if (declaration.contains("pus")) return resolve_explicit(declaration.at("pus"), pointer + "/pus");
        auto parsed = parse_selector(declaration.at("select"));
        if (!parsed) throw std::runtime_error{parsed.error().code.value()};
        return apply_selector(*parsed, pointer + "/select", name);
    }

    auto resolve_explicit(const Json& list, const std::string& pointer)
        -> std::optional<std::vector<ResolvedProcessingUnit>> {
        std::set<core::LinuxCpuId> seen;
        std::vector<ResolvedProcessingUnit> output;
        bool failed = false;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto cpu = core::LinuxCpuId{list.at(i).get<std::uint32_t>()};
            const auto found = all_pus_.find(cpu);
            const auto element = pointer + "/" + std::to_string(i);
            if (found == all_pus_.end()) {
                diagnostics_.add("scenario.resources.invalid_explicit_pu", element,
                                 "absent_cpu", {{"array_index", std::to_string(i)}});
                failed = true;
            } else if (!std::ranges::contains(effective_pus_, &found->second)) {
                diagnostics_.add("scenario.resources.resource_not_effective", element,
                                 "ineffective_cpu", {{"array_index", std::to_string(i)}});
                failed = true;
            } else if (!seen.insert(cpu).second) {
                diagnostics_.add("scenario.resources.duplicate_resource", element,
                                 "duplicate_cpu", {{"array_index", std::to_string(i)}});
                failed = true;
            } else {
                output.push_back(materialize(found->second));
            }
        }
        if (failed) return std::nullopt;
        return output;
    }

    auto apply_selector(const Selector& selector, const std::string& pointer,
                        const std::string& target_name)
        -> std::optional<std::vector<ResolvedProcessingUnit>> {
        if ((selector.filters.allowed && !*selector.filters.allowed) ||
            (selector.filters.online && !*selector.filters.online)) {
            const auto field = selector.filters.allowed && !*selector.filters.allowed
                                   ? "allowed"
                                   : "online";
            diagnostics_.add("scenario.resources.invalid_selector", pointer + "/where/" + field,
                             "false_effective_assertion");
            return std::nullopt;
        }
        const auto empty_filter = [&](const auto& values, std::string_view field) {
            if (values.empty()) return false;
            static_cast<void>(field);
            return false;
        };
        static_cast<void>(empty_filter);
        const Json empty_select;
        const auto& select_json = declarations_.contains(target_name)
                                      ? declarations_.at(target_name)->at("select")
                                      : empty_select;
        if (select_json.contains("where")) {
            const auto& where = select_json.at("where");
            for (const auto field : {"packages", "numa_nodes", "dies", "cache_domains"})
                if (where.contains(field) && where.at(field).empty()) {
                    diagnostics_.add("scenario.resources.invalid_selector",
                                     pointer + "/where/" + field, "empty_filter",
                                     {{"filter_category", field}});
                    return std::nullopt;
                }
        }
        if (!validate_filter_identities(selector.filters, pointer) ||
            !validate_core_exclusion_identities(selector.exclusions.cores, pointer))
            return std::nullopt;

        std::vector<const PuView*> filtered;
        for (const auto* pu : effective_pus_) {
            if (overrides_.mandatory_linux_numa_node) {
                const auto nodes = context_.topology.numa_nodes_for_pu(pu->pu->id);
                if (nodes.size() != 1U) {
                    diagnostics_.add(nodes.empty() ? "scenario.resources.unavailable_attribute"
                                                   : "scenario.resources.conflicted_attribute",
                                     pointer, "numa_membership_not_unique");
                    return std::nullopt;
                }
                const auto node = context_.topology.numa_node(nodes.front());
                if (!node || !attribute_value(node->linux_node_id, pointer)) return std::nullopt;
                if (*node->linux_node_id.value() != *overrides_.mandatory_linux_numa_node)
                    continue;
            }
            const auto match = matches_filters(*pu, selector.filters, pointer);
            if (!match) return std::nullopt;
            if (*match) filtered.push_back(pu);
        }
        if (filtered.empty()) {
            diagnostics_.add("scenario.resources.empty_selection", pointer, "filter_no_match");
            return std::nullopt;
        }
        std::map<topology::StableCoreId, std::vector<const PuView*>> groups;
        for (const auto* pu : filtered) groups[pu->stable_core].push_back(pu);
        std::vector<Unit> units;
        for (auto& [core_id, siblings] : groups) {
            std::ranges::sort(siblings, pu_less);
            std::vector<const PuView*> projected;
            if (selector.smt == SelectorSmtMode::all) projected = siblings;
            else if (selector.smt == SelectorSmtMode::one_per_core) projected.push_back(siblings.front());
            else {
                for (std::size_t authored = 0; authored < selector.smt_indices.size(); ++authored) {
                    const auto index = selector.smt_indices[authored];
                    if (index >= siblings.size()) {
                        diagnostics_.add("scenario.resources.invalid_smt_index",
                                         pointer + "/smt/indices/" + std::to_string(authored),
                                         "index_unavailable", {{"smt_mode", "explicit_indices"}});
                        return std::nullopt;
                    }
                    projected.push_back(siblings[static_cast<std::size_t>(index)]);
                }
            }
            units.push_back(Unit{core_id, std::move(projected)});
        }

        std::set<core::LinuxCpuId> excluded;
        for (const auto cpu : selector.exclusions.processing_units) {
            if (!all_pus_.contains(cpu)) {
                diagnostics_.add("scenario.resources.invalid_selector", pointer + "/exclude/pus",
                                 "unknown_exclusion_identity", {{"filter_category", "pus"}});
                return std::nullopt;
            }
            excluded.insert(cpu);
        }
        for (const auto& set_name : selector.exclusions.worker_sets) {
            const auto found = resolved_.find(set_name);
            if (found == resolved_.end()) return std::nullopt;
            for (const auto& pu : found->second.processing_units) excluded.insert(pu.linux_cpu);
        }
        for (auto& unit : units) {
            std::erase_if(unit.pus, [&](const PuView* pu) {
                if (excluded.contains(pu->pu->linux_cpu_id)) return true;
                if (!selector.exclusions.cores.empty()) {
                    if (pu->core->os_id.availability() == topology::Availability::conflicted) {
                        diagnostics_.add("scenario.resources.conflicted_attribute",
                                         pointer + "/exclude/cores", "core_os_id_conflicted");
                        return false;
                    }
                    if (!pu->core->os_id.value()) {
                        diagnostics_.add("scenario.resources.unavailable_attribute",
                                         pointer + "/exclude/cores", "core_os_id_unavailable");
                        return false;
                    }
                    return std::ranges::contains(selector.exclusions.cores, *pu->core->os_id.value());
                }
                return false;
            });
        }
        if (!diagnostics_.values().empty() && failed_.contains(target_name)) return std::nullopt;
        std::erase_if(units, [](const Unit& unit) { return unit.pus.empty(); });
        if (units.empty()) {
            diagnostics_.add("scenario.resources.empty_selection", pointer, "excluded_all");
            return std::nullopt;
        }
        if (selector.unit == SelectorUnit::processing_unit) {
            std::vector<Unit> pu_units;
            for (const auto& group : units)
                for (const auto* pu : group.pus) pu_units.push_back(Unit{group.core_id, {pu}});
            units = std::move(pu_units);
        }
        if (!order_units(units, selector.ordering, pointer, target_name)) return std::nullopt;
        if (selector.count && *selector.count > units.size()) {
            diagnostics_.add("scenario.resources.insufficient_resources", pointer + "/count",
                             "count_exceeds_available",
                             {{"requested_count", std::to_string(*selector.count)},
                              {"available_count", std::to_string(units.size())}});
            return std::nullopt;
        }
        if (selector.count)
            units.erase(units.begin() + static_cast<std::ptrdiff_t>(*selector.count), units.end());
        std::vector<ResolvedProcessingUnit> output;
        for (const auto& unit : units) for (const auto* pu : unit.pus) output.push_back(materialize(*pu));
        if (output.empty()) {
            diagnostics_.add("scenario.resources.empty_selection", pointer, "smt_empty");
            return std::nullopt;
        }
        return output;
    }

    template <typename Object, typename AttributeMember>
    auto validate_os_ids(const std::vector<std::uint32_t>& requested,
                         const std::vector<Object>& objects, AttributeMember member,
                         const std::string& pointer, std::string_view category) -> bool {
        if (requested.empty()) return true;
        std::set<std::uint32_t> available;
        for (const auto& object : objects) {
            const auto& attribute = object.*member;
            if (attribute.availability() == topology::Availability::conflicted) {
                diagnostics_.add("scenario.resources.conflicted_attribute", pointer,
                                 "identity_attribute_conflicted",
                                 {{"filter_category", std::string{category}}});
                return false;
            }
            if (!attribute.value()) {
                diagnostics_.add("scenario.resources.unavailable_attribute", pointer,
                                 "identity_attribute_unavailable",
                                 {{"filter_category", std::string{category}}});
                return false;
            }
            available.insert(*attribute.value());
        }
        for (const auto value : requested)
            if (!available.contains(value)) {
                diagnostics_.add("scenario.resources.invalid_selector", pointer,
                                 "unknown_filter_identity",
                                 {{"filter_category", std::string{category}}});
                return false;
            }
        return true;
    }

    auto validate_filter_identities(const SelectorFilters& filters,
                                    const std::string& pointer) -> bool {
        if (!validate_os_ids(filters.packages, context_.topology.packages(),
                             &topology::Package::os_id, pointer + "/where/packages", "packages") ||
            !validate_os_ids(filters.dies, context_.topology.dies(), &topology::Die::os_id,
                             pointer + "/where/dies", "dies"))
            return false;
        if (!filters.numa_nodes.empty()) {
            std::set<std::uint32_t> available;
            for (const auto& node : context_.topology.numa_nodes()) {
                if (node.linux_node_id.availability() == topology::Availability::conflicted) {
                    diagnostics_.add("scenario.resources.conflicted_attribute",
                                     pointer + "/where/numa_nodes",
                                     "identity_attribute_conflicted",
                                     {{"filter_category", "numa_nodes"}});
                    return false;
                }
                if (!node.linux_node_id.value()) {
                    diagnostics_.add("scenario.resources.unavailable_attribute",
                                     pointer + "/where/numa_nodes",
                                     "identity_attribute_unavailable",
                                     {{"filter_category", "numa_nodes"}});
                    return false;
                }
                available.insert(*node.linux_node_id.value());
            }
            for (const auto value : filters.numa_nodes)
                if (!available.contains(value)) {
                    diagnostics_.add("scenario.resources.invalid_selector",
                                     pointer + "/where/numa_nodes", "unknown_filter_identity",
                                     {{"filter_category", "numa_nodes"}});
                    return false;
                }
        }
        for (const auto ordinal : filters.cache_domains)
            if (!cache_ordinals_.contains(ordinal)) {
                diagnostics_.add("scenario.resources.invalid_selector",
                                 pointer + "/where/cache_domains", "unknown_cache_ordinal",
                                 {{"filter_category", "cache_domains"}});
                return false;
            }
        return true;
    }

    auto validate_core_exclusion_identities(const std::vector<std::uint32_t>& requested,
                                            const std::string& pointer) -> bool {
        return validate_os_ids(requested, context_.topology.cores(), &topology::Core::os_id,
                               pointer + "/exclude/cores", "cores");
    }

    auto matches_filters(const PuView& pu, const SelectorFilters& filters,
                         const std::string& pointer) -> std::optional<bool> {
        const auto contains = [](const auto& values, std::uint32_t value) {
            return values.empty() || std::ranges::contains(values, value);
        };
        if (!filters.packages.empty()) {
            const auto package = packages_.at(pu.core->package_id);
            if (!attribute_value(package->os_id, pointer + "/where/packages")) return std::nullopt;
            if (!contains(filters.packages, *package->os_id.value())) return false;
        }
        if (!filters.dies.empty()) {
            if (!pu.core->die_id) {
                diagnostics_.add("scenario.resources.unavailable_attribute", pointer + "/where/dies",
                                 "die_membership_unavailable"); return std::nullopt;
            }
            const auto die = dies_.at(*pu.core->die_id);
            if (!attribute_value(die->os_id, pointer + "/where/dies")) return std::nullopt;
            if (!contains(filters.dies, *die->os_id.value())) return false;
        }
        if (!filters.numa_nodes.empty()) {
            const auto nodes = context_.topology.numa_nodes_for_pu(pu.pu->id);
            if (nodes.size() != 1) {
                diagnostics_.add(nodes.empty() ? "scenario.resources.unavailable_attribute"
                                               : "scenario.resources.conflicted_attribute",
                                 pointer + "/where/numa_nodes", "numa_membership_not_unique");
                return std::nullopt;
            }
            const auto node = context_.topology.numa_node(nodes.front());
            if (!node || !attribute_value(node->linux_node_id, pointer + "/where/numa_nodes"))
                return std::nullopt;
            if (!contains(filters.numa_nodes, *node->linux_node_id.value())) return false;
        }
        if (!filters.cache_domains.empty()) {
            bool matches = false;
            const auto memberships = context_.topology.caches_for_pu(pu.pu->id);
            for (const auto ordinal : filters.cache_domains) {
                const auto cache = cache_ordinals_.find(ordinal);
                if (cache == cache_ordinals_.end()) {
                    diagnostics_.add("scenario.resources.invalid_selector",
                                     pointer + "/where/cache_domains", "unknown_cache_ordinal");
                    return std::nullopt;
                }
                if (std::ranges::contains(memberships, cache->second->id)) matches = true;
            }
            if (!matches) return false;
        }
        return true;
    }

    template <typename T>
    auto attribute_value(const topology::Attribute<T>& attribute, const std::string& pointer) -> bool {
        if (attribute.availability() == topology::Availability::conflicted) {
            diagnostics_.add("scenario.resources.conflicted_attribute", pointer,
                             "attribute_conflicted"); return false;
        }
        if (!attribute.value()) {
            diagnostics_.add("scenario.resources.unavailable_attribute", pointer,
                             "attribute_unavailable"); return false;
        }
        return true;
    }

    static auto pu_less(const PuView* left, const PuView* right) -> bool {
        if (left->pu->linux_cpu_id != right->pu->linux_cpu_id)
            return left->pu->linux_cpu_id < right->pu->linux_cpu_id;
        return left->stable_pu < right->stable_pu;
    }

    static auto representative(const Unit& unit) -> const PuView* {
        return *std::ranges::min_element(unit.pus, pu_less);
    }

    static auto ascending(const Unit& left, const Unit& right) -> bool {
        const auto* a = representative(left); const auto* b = representative(right);
        if (a->pu->linux_cpu_id != b->pu->linux_cpu_id) return a->pu->linux_cpu_id < b->pu->linux_cpu_id;
        if (a->stable_pu != b->stable_pu) return a->stable_pu < b->stable_pu;
        return left.core_id < right.core_id;
    }

    auto order_units(std::vector<Unit>& units, SelectorOrdering ordering,
                     const std::string& pointer, const std::string& target) -> bool {
        std::ranges::sort(units, ascending);
        if (ordering == SelectorOrdering::ascending_pu) return true;
        if (ordering == SelectorOrdering::descending_pu) {
            std::ranges::sort(units, [](const Unit& left, const Unit& right) {
                const auto* a = representative(left); const auto* b = representative(right);
                if (a->pu->linux_cpu_id != b->pu->linux_cpu_id)
                    return a->pu->linux_cpu_id > b->pu->linux_cpu_id;
                if (a->stable_pu != b->stable_pu) return a->stable_pu < b->stable_pu;
                return left.core_id < right.core_id;
            }); return true;
        }
        if (ordering == SelectorOrdering::random) return shuffle(units, target, pointer);
        if (ordering == SelectorOrdering::compact_numa || ordering == SelectorOrdering::spread_numa)
            return bucket_numa(units, ordering == SelectorOrdering::spread_numa, pointer);
        return bucket_cache(units, ordering == SelectorOrdering::spread_cache, pointer);
    }

    auto unit_numa(const Unit& unit, const std::string& pointer) -> std::optional<std::uint32_t> {
        std::optional<std::uint32_t> result;
        for (const auto* pu : unit.pus) {
            const auto nodes = context_.topology.numa_nodes_for_pu(pu->pu->id);
            if (nodes.size() != 1) {
                diagnostics_.add(nodes.empty() ? "scenario.resources.unavailable_attribute"
                                               : "scenario.resources.conflicted_attribute",
                                 pointer + "/ordering", "numa_membership_not_unique"); return std::nullopt;
            }
            const auto node = context_.topology.numa_node(nodes.front());
            if (!node || !attribute_value(node->linux_node_id, pointer + "/ordering")) return std::nullopt;
            const auto id = *node->linux_node_id.value();
            if (result && *result != id) {
                diagnostics_.add("scenario.resources.conflicted_attribute", pointer + "/ordering",
                                 "core_crosses_numa_domains"); return std::nullopt;
            }
            result = id;
        }
        return result;
    }

    auto bucket_numa(std::vector<Unit>& units, bool spread, const std::string& pointer) -> bool {
        std::map<std::uint32_t, std::vector<Unit>> buckets;
        for (auto& unit : units) {
            const auto node = unit_numa(unit, pointer); if (!node) return false;
            buckets[*node].push_back(std::move(unit));
        }
        units.clear();
        for (auto& [id, bucket] : buckets) { static_cast<void>(id); std::ranges::sort(bucket, ascending); }
        flatten(buckets, units, spread); return true;
    }

    auto cache_for(const Unit& unit, std::uint32_t level) const
        -> std::optional<topology::StableCacheId> {
        std::optional<topology::StableCacheId> result;
        for (const auto* pu : unit.pus) {
            std::vector<topology::StableCacheId> ids;
            for (const auto cache_id : context_.topology.caches_for_pu(pu->pu->id)) {
                const auto cache = context_.topology.cache(cache_id);
                if (cache && cache->kind == topology::CacheKind::unified && cache->level == level) {
                    const auto stable = context_.stable_ids.cache_id(cache_id);
                    if (stable) ids.push_back(stable->get());
                }
            }
            if (ids.size() != 1) return std::nullopt;
            if (result && *result != ids.front()) return std::nullopt;
            result = ids.front();
        }
        return result;
    }

    auto bucket_cache(std::vector<Unit>& units, bool spread, const std::string& pointer) -> bool {
        std::set<std::uint32_t, std::greater<>> levels;
        for (const auto& cache : context_.topology.caches())
            if (cache.kind == topology::CacheKind::unified) levels.insert(cache.level);
        for (const auto level : levels) {
            std::map<topology::StableCacheId, std::vector<Unit>> buckets;
            bool valid = true;
            for (const auto& unit : units) {
                const auto cache = cache_for(unit, level);
                if (!cache) { valid = false; break; }
                buckets[*cache].push_back(unit);
            }
            if (!valid) continue;
            units.clear();
            for (auto& [id, bucket] : buckets) { static_cast<void>(id); std::ranges::sort(bucket, ascending); }
            flatten(buckets, units, spread); return true;
        }
        diagnostics_.add("scenario.resources.unavailable_attribute", pointer + "/ordering",
                         "common_unified_cache_unavailable"); return false;
    }

    template <typename Key>
    static void flatten(std::map<Key, std::vector<Unit>>& buckets, std::vector<Unit>& output,
                        bool spread) {
        if (!spread) {
            for (auto& [key, bucket] : buckets) {
                static_cast<void>(key);
                std::ranges::move(bucket, std::back_inserter(output));
            }
            return;
        }
        std::size_t index = 0;
        bool more = true;
        while (more) {
            more = false;
            for (auto& [key, bucket] : buckets) {
                static_cast<void>(key);
                if (index < bucket.size()) { output.push_back(std::move(bucket[index])); more = true; }
            }
            ++index;
        }
    }

    auto shuffle(std::vector<Unit>& units, const std::string& target,
                 const std::string& pointer) -> bool {
        const auto root = scenario_.value().at("random").at("seed").get<std::uint64_t>();
        auto components = std::vector<core::SeedComponent>{
            {"profile", std::string{resource_resolution_profile}},
            {"scope", overrides_.mandatory_linux_numa_node ? pointer
                                                           : "/resources/worker_sets/" + target},
            {"effective-topology", context_.fingerprints.effective.to_string()}};
        if (overrides_.matrix_worker_node_seed_component)
            components.push_back(
                {"matrix-worker-node", *overrides_.matrix_worker_node_seed_component});
        auto seed = core::derive_seed(root, core::SeedDerivationKey{
            .domain = core::SeedDomain::worker_selection,
            .components = std::move(components)});
        if (!seed) throw std::runtime_error{seed.error().code.value()};
        std::uint64_t state = seed->value();
        auto draw = [&state]() {
            state += UINT64_C(0x9E3779B97F4A7C15);
            auto z = state;
            z = (z ^ (z >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
            z = (z ^ (z >> 27U)) * UINT64_C(0x94D049BB133111EB);
            return z ^ (z >> 31U);
        };
        for (std::size_t i = units.size(); i > 1; --i) {
            const auto bound = static_cast<std::uint64_t>(i);
            const auto threshold = (UINT64_C(0) - bound) % bound;
            std::uint64_t value{};
            do { value = draw(); } while (value < threshold);
            std::swap(units[i - 1], units[static_cast<std::size_t>(value % bound)]);
        }
        return true;
    }

    auto materialize(const PuView& pu) const -> ResolvedProcessingUnit {
        return {pu.stable_pu, pu.pu->linux_cpu_id, pu.stable_core};
    }

    void bind_experiments() {
        const auto& experiments = scenario_.value().at("experiments");
        for (std::size_t index = 0; index < experiments.size() && !diagnostics_.capped(); ++index) {
            const auto& experiment = experiments.at(index);
            const auto base = "/experiments/" + std::to_string(index);
            if (core::detail::json_string_equal(experiment.at("type"), "comparison")) {
                const auto name = experiment.at("workers").get<std::string>();
                if (resolved_.contains(name))
                    experiment_workers_.push_back({index, name, base + "/workers"});
                else if (!declarations_.contains(name))
                    diagnostics_.add("scenario.resources.unknown_reference", base + "/workers",
                                     "unknown_worker_set", {{"reference_category", "worker_set"}});
            } else {
                const auto pointer = base + "/workers_per_node/select";
                auto selector = parse_selector(experiment.at("workers_per_node").at("select"));
                if (!selector) throw std::runtime_error{selector.error().code.value()};
                bool references_valid = true;
                const auto& selector_json = experiment.at("workers_per_node").at("select");
                if (selector_json.contains("where")) {
                    const auto& where = selector_json.at("where");
                    for (const auto field : {"packages", "numa_nodes", "dies", "cache_domains"})
                        if (where.contains(field) && where.at(field).empty()) {
                            diagnostics_.add("scenario.resources.invalid_selector",
                                             pointer + "/where/" + field, "empty_filter",
                                             {{"filter_category", field}});
                            references_valid = false;
                        }
                }
                if ((selector->filters.allowed && !*selector->filters.allowed) ||
                    (selector->filters.online && !*selector->filters.online)) {
                    const auto field = selector->filters.allowed && !*selector->filters.allowed
                                           ? "allowed"
                                           : "online";
                    diagnostics_.add("scenario.resources.invalid_selector",
                                     pointer + "/where/" + field,
                                     "false_effective_assertion");
                    references_valid = false;
                }
                for (std::size_t ref = 0; ref < selector->exclusions.worker_sets.size(); ++ref) {
                    if (!declarations_.contains(selector->exclusions.worker_sets[ref])) {
                        diagnostics_.add("scenario.resources.unknown_reference",
                                         pointer + "/exclude/worker_sets/" + std::to_string(ref),
                                         "unknown_worker_set", {{"reference_category", "worker_set"}});
                        references_valid = false;
                    }
                }
                if (references_valid)
                    matrix_templates_.push_back({index, selector->unit, selector->filters,
                                                 selector->exclusions, selector->smt,
                                                 selector->smt_indices, selector->count,
                                                 selector->ordering, pointer,
                                                 std::string{resource_resolution_profile}});
            }
        }
    }

    const DefaultExpandedScenario& scenario_;
    const ResourceResolutionContext& context_;
    Diagnostics diagnostics_;
    std::map<core::LinuxCpuId, PuView> all_pus_;
    std::vector<const PuView*> effective_pus_;
    std::map<core::CoreId, const topology::Core*> cores_;
    std::map<core::PackageId, const topology::Package*> packages_;
    std::map<core::DieId, const topology::Die*> dies_;
    std::map<std::uint32_t, const topology::CacheDomain*> cache_ordinals_;
    std::vector<std::string> declaration_order_;
    std::map<std::string, const Json*> declarations_;
    std::map<std::string, std::vector<std::string>> graph_;
    std::set<std::string> failed_;
    std::map<std::string, ResolvedWorkerSet> resolved_;
    std::vector<ExperimentWorkerReference> experiment_workers_;
    std::vector<MatrixWorkerSelectorTemplate> matrix_templates_;
    SelectionOverrides overrides_;
};

} // namespace

auto resources_internal_error(std::string classification, std::string underlying) -> core::Error {
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.resources.internal_failure"},
            .message = "Resource resolution failed internally",
            .operation = "resolve_scenario_resources",
            .os_error = std::nullopt,
            .context = error_context(std::move(classification), std::move(underlying)),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto verify_resource_topology(const ResourceResolutionContext& context)
    -> std::expected<void, core::Error> {
    const auto validation = topology::validate_topology(context.topology);
    if (!validation.valid()) {
        const auto underlying = validation.diagnostics.empty()
                                    ? std::string{}
                                    : validation.diagnostics.front().code.value();
        return std::unexpected{resources_internal_error("invalid_topology", underlying)};
    }
    auto ids = topology::derive_stable_topology_ids(context.topology);
    core::detail::ExpectedErrorCleanup ids_error_cleanup{ids};
    if (!ids) return std::unexpected{resources_internal_error("stable_id_derivation", ids.error().code.value())};
    if (!equal_ids(*ids, context.stable_ids))
        return std::unexpected{resources_internal_error("stable_ids_mismatch")};
    auto fingerprints = topology::derive_topology_fingerprints(context.topology, *ids);
    core::detail::ExpectedErrorCleanup fingerprints_error_cleanup{fingerprints};
    if (!fingerprints)
        return std::unexpected{resources_internal_error("fingerprint_derivation", fingerprints.error().code.value())};
    if (fingerprints->physical != context.fingerprints.physical)
        return std::unexpected{resources_internal_error("physical_fingerprint_mismatch")};
    if (fingerprints->effective != context.fingerprints.effective)
        return std::unexpected{resources_internal_error("effective_fingerprint_mismatch")};
    return {};
}

auto resolve_v1_resources(const DefaultExpandedScenario& scenario,
                          const ResourceResolutionContext& context)
    -> std::expected<ResolutionPayload, core::Error> {
    try { return Resolver{scenario, context}.run(); }
    catch (const std::exception&) { return std::unexpected{resources_internal_error("selector_exception")}; }
    catch (...) { return std::unexpected{resources_internal_error("selector_unknown_failure")}; }
}

auto resolve_v1_selection(const DefaultExpandedScenario& scenario,
                          const ResourceResolutionContext& context,
                          const nlohmann::ordered_json& declaration, std::string pointer,
                          std::string target_name,
                          const std::map<std::string, ResolvedWorkerSet>& worker_sets,
                          SelectionOverrides overrides)
    -> std::expected<SingleSelectionPayload, core::Error> {
    try {
        return Resolver{scenario, context, std::move(overrides)}.resolve_one(
            declaration, std::move(pointer), std::move(target_name), worker_sets);
    } catch (const std::exception&) {
        return std::unexpected{resources_internal_error("contained_exception")};
    } catch (...) {
        return std::unexpected{resources_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario::detail
