#include "epycbench/topology/invariants.hpp"
#include "topology/page_size_projection.hpp"
#include "core/json_ownership.hpp"
#include "topology/source_diagnostic_access.hpp"

#include <type_traits>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace epycbench::topology {
namespace {
using Json = nlohmann::json;

enum class Phase : std::uint8_t {
    references = 1,
    machine_sets,
    core_structure,
    package_structure,
    caches,
    numa,
    distances,
    pci,
    attributes,
};

struct DiagnosticError : core::Error {
    DiagnosticError(core::Error&& value) noexcept : core::Error(std::move(value)) {}
    DiagnosticError(DiagnosticError&& value) noexcept : core::Error(std::move(value)) {}
    auto operator=(DiagnosticError&& value) noexcept -> DiagnosticError& {
        if (this != &value) {
            core::detail::retire_json(context);
            core::Error::operator=(std::move(value));
        }
        return *this;
    }
    ~DiagnosticError() { core::detail::retire_json(context); }
};
struct PendingDiagnostic {
    Phase phase;
    std::string object_type;
    std::string object_id;
    std::string field_path;
    std::string code;
    std::string reference;
    DiagnosticError error;
    std::size_t ordinal{};
};
static_assert(std::is_nothrow_move_constructible_v<PendingDiagnostic>);
static_assert(std::is_nothrow_move_assignable_v<PendingDiagnostic>);

// This producer's completed candidate remains owned across a failing append.
// The sink is a main-only borrow, never an ambient/TLS error destination.
struct PendingDiagnostics final {
    std::vector<PendingDiagnostic> values;
    std::optional<core::Error>* sink{};
    detail::SourceValidationEligibility eligibility{};
    std::size_t next{};
    explicit PendingDiagnostics(std::optional<core::Error>* raw,
        detail::SourceValidationEligibility mode) noexcept : sink{raw}, eligibility{mode} {}
    auto begin() noexcept { return values.begin(); }
    auto end() noexcept { return values.end(); }
    auto begin() const noexcept { return values.begin(); }
    auto end() const noexcept { return values.end(); }
    auto size() const noexcept { return values.size(); }
    auto back() noexcept -> PendingDiagnostic& { return values.back(); }
    auto eligible(const PendingDiagnostic& value) const noexcept -> bool {
        return eligibility != detail::SourceValidationEligibility::stable_identity ||
            !(value.code == "topology.attribute.conflict_without_difference" &&
              (value.object_type == "package" || value.object_type == "die") &&
              value.field_path == "os_id");
    }
    void retain(PendingDiagnostic* candidate = nullptr) noexcept {
        if (!sink || *sink) return;
        auto* first = candidate && eligible(*candidate) ? candidate : nullptr;
        for (auto& value : values)
            if (eligible(value) && (!first || value.ordinal < first->ordinal)) first = &value;
        if (first) sink->emplace(std::move(static_cast<core::Error&>(first->error)));
    }
    void push_back(PendingDiagnostic&& candidate) {
        candidate.ordinal = next++;
        try { values.push_back(std::move(candidate)); }
        catch (...) { retain(&candidate); throw; }
    }
};

auto make_diagnostic(Phase phase, std::string_view code, std::string_view message,
                     std::string object_type, std::string object_id, std::string field_path,
                     std::string reference = {},
                     core::detail::OwnedJson<Json> extra = Json::object()) -> PendingDiagnostic {
    auto context = core::detail::json_object<Json>([&](auto& object_1) {
        core::detail::set_json(object_1, "object_type", object_type);
        core::detail::set_json(object_1, "object_id", object_id);
        core::detail::set_json(object_1, "field_path", field_path);
    });
    core::detail::JsonCleanup context_cleanup{context};
    if (!reference.empty()) {
        context["referenced_id"] = reference;
    }
    for (auto it = extra.value.begin(); it != extra.value.end(); ++it)
        core::detail::set_json(context, it.key(), it.value());
    return PendingDiagnostic{
        .phase = phase,
        .object_type = std::move(object_type),
        .object_id = std::move(object_id),
        .field_path = std::move(field_path),
        .code = std::string{code},
        .reference = std::move(reference),
        .error =
            core::Error{
                .category = core::ErrorCategory::topology,
                .code = core::StableErrorCode{std::string{code}},
                .message = std::string{message},
                .operation = "validate_topology",
                .os_error = std::nullopt,
                .context = std::move(context),
                .severity = core::ErrorSeverity::error,
                .continuation = core::ContinuationPolicy::continue_operation,
            },
    };
}

template <typename Id>
auto numeric_reference(std::string_view prefix, Id id) -> std::string {
    if constexpr (std::integral<Id>) {
        return std::string{prefix} + '/' + std::to_string(id);
    } else {
        return std::string{prefix} + '/' + std::to_string(id.value());
    }
}

auto pci_id(const PciAddress& address) -> std::string { return "pci/" + address.to_string(); }

template <typename Id> auto formatted_ids(const std::vector<Id>& ids) -> Json {
    auto result = Json::array();
    core::detail::JsonCleanup cleanup{result};
    for (const auto id : ids) {
        result.push_back(format_topology_id(id));
    }
    return Json(std::move(result));
}

template <typename T>
void check_conflict(const Attribute<T>& attribute, std::string object_type, std::string object_id,
                    std::string field_path, PendingDiagnostics& diagnostics) {
    if (attribute.availability() != Availability::conflicted) {
        return;
    }
    const auto& conflicts = attribute.conflicts();
    const auto has_difference = std::ranges::any_of(conflicts | std::views::drop(1),
                                                    [&](const auto& candidate) {
                                                        return candidate.value != conflicts.front().value;
                                                    });
    if (!has_difference) {
        diagnostics.push_back(make_diagnostic(
            Phase::attributes, "topology.attribute.conflict_without_difference",
            "Conflicted attribute does not contain distinct values", std::move(object_type),
            std::move(object_id), std::move(field_path)));
    }
}

template <typename Id>
auto contains_id(const std::vector<Id>& ids, Id id) -> bool {
    return std::ranges::binary_search(ids, id);
}

auto pu_package(const TopologySnapshot& topology, core::ProcessingUnitId pu_id)
    -> std::optional<core::PackageId> {
    const auto pu = topology.processing_unit(pu_id);
    if (!pu) {
        return std::nullopt;
    }
    const auto core = topology.core(pu->core_id);
    return core ? std::optional{core->package_id} : std::nullopt;
}

void validate_references(const TopologySnapshot& topology,
                         PendingDiagnostics& diagnostics) {
    for (const auto& pu : topology.processing_units()) {
        if (!topology.core(pu.core_id)) {
            diagnostics.push_back(make_diagnostic(
                Phase::references, "topology.reference.missing_core",
                "Processing unit references a missing core", "processing_unit",
                format_topology_id(pu.id), "core_id", format_topology_id(pu.core_id)));
        }
    }
    for (const auto& core : topology.cores()) {
        if (!topology.package(core.package_id)) {
            diagnostics.push_back(make_diagnostic(
                Phase::references, "topology.reference.missing_package",
                "Core references a missing package", "core", format_topology_id(core.id),
                "package_id", format_topology_id(core.package_id)));
        }
        if (core.die_id) {
            const auto die = topology.die(*core.die_id);
            if (!die) {
                diagnostics.push_back(make_diagnostic(
                    Phase::references, "topology.reference.missing_die",
                    "Core references a missing die", "core", format_topology_id(core.id),
                    "die_id", format_topology_id(*core.die_id)));
            } else if (die->package_id != core.package_id) {
                diagnostics.push_back(make_diagnostic(
                    Phase::references, "topology.reference.core_die_package_mismatch",
                    "Core and die reference different packages", "core",
                    format_topology_id(core.id), "die_id", format_topology_id(*core.die_id),
                    core::detail::json_object<Json>([&](auto& object_2) {
                        core::detail::set_json(object_2, "core_package",
                                               format_topology_id(core.package_id));
                        core::detail::set_json(object_2, "die_package",
                                               format_topology_id(die->package_id));
                    })));
            }
        }
    }
    for (const auto& die : topology.dies()) {
        if (!topology.package(die.package_id)) {
            diagnostics.push_back(make_diagnostic(
                Phase::references, "topology.reference.missing_package",
                "Die references a missing package", "die", format_topology_id(die.id),
                "package_id", format_topology_id(die.package_id)));
        }
        if (die.group_id && !topology.architecture_group(*die.group_id)) {
            diagnostics.push_back(make_diagnostic(
                Phase::references, "topology.reference.missing_group",
                "Die references a missing architecture group", "die", format_topology_id(die.id),
                "group_id", format_topology_id(*die.group_id)));
        }
    }
    for (const auto& group : topology.architecture_groups()) {
        if (!topology.package(group.package_id)) {
            diagnostics.push_back(make_diagnostic(
                Phase::references, "topology.reference.missing_package",
                "Architecture group references a missing package", "architecture_group",
                format_topology_id(group.id), "package_id", format_topology_id(group.package_id)));
        }
        if (group.parent_group_id && !topology.architecture_group(*group.parent_group_id)) {
            diagnostics.push_back(make_diagnostic(
                Phase::references, "topology.reference.missing_group_parent",
                "Architecture group references a missing parent", "architecture_group",
                format_topology_id(group.id), "parent_group_id",
                format_topology_id(*group.parent_group_id)));
        }
        for (const auto pu : group.member_pus) {
            if (!topology.processing_unit(pu)) {
                diagnostics.push_back(make_diagnostic(
                    Phase::references, "topology.group.invalid_member",
                    "Architecture group references a missing processing unit",
                    "architecture_group", format_topology_id(group.id), "member_pus",
                    format_topology_id(pu)));
            }
        }
    }
    for (const auto& cache : topology.caches()) {
        for (const auto pu : cache.member_pus) {
            if (!topology.processing_unit(pu)) {
                diagnostics.push_back(make_diagnostic(
                    Phase::references, "topology.cache.invalid_member",
                    "Cache references a missing processing unit", "cache",
                    format_topology_id(cache.id), "member_pus", format_topology_id(pu)));
            }
        }
    }
    for (const auto& node : topology.numa_nodes()) {
        for (const auto pu : node.member_pus) {
            if (!topology.processing_unit(pu)) {
                diagnostics.push_back(make_diagnostic(
                    Phase::references, "topology.numa.invalid_member",
                    "NUMA node references a missing processing unit", "numa_node",
                    format_topology_id(node.id), "member_pus", format_topology_id(pu)));
            }
        }
    }
    for (const auto& entry : topology.numa_distances().directed_distances) {
        if (!topology.numa_node(entry.from)) {
            diagnostics.push_back(make_diagnostic(
                Phase::references, "topology.distance.invalid_node_reference",
                "Distance source references a missing NUMA node", "numa_distance",
                format_topology_id(entry.from) + "->" + format_topology_id(entry.to), "from",
                format_topology_id(entry.from)));
        }
        if (!topology.numa_node(entry.to)) {
            diagnostics.push_back(make_diagnostic(
                Phase::references, "topology.distance.invalid_node_reference",
                "Distance target references a missing NUMA node", "numa_distance",
                format_topology_id(entry.from) + "->" + format_topology_id(entry.to), "to",
                format_topology_id(entry.to)));
        }
    }
    for (const auto& device : topology.pci_devices()) {
        if (device.numa_locality.availability() == Availability::available &&
            !topology.numa_node(*device.numa_locality.value())) {
            diagnostics.push_back(make_diagnostic(
                Phase::references, "topology.pci.invalid_numa_locality",
                "PCI device references a missing NUMA node", "pci_device", pci_id(device.address),
                "numa_locality", format_topology_id(*device.numa_locality.value())));
        }
        if (device.local_pus.availability() == Availability::available) {
            for (const auto pu : *device.local_pus.value()) {
                if (!topology.processing_unit(pu)) {
                    diagnostics.push_back(make_diagnostic(
                        Phase::references, "topology.pci.invalid_local_pu",
                        "PCI device references a missing local processing unit", "pci_device",
                        pci_id(device.address), "local_pus", format_topology_id(pu)));
                }
            }
        }
    }
}

void validate_machine_sets(const TopologySnapshot& topology,
                           PendingDiagnostics& diagnostics) {
    const auto& machine = topology.machine();
    const auto machine_id = format_topology_id(machine.id);
    const auto report_set = [&](std::string_view code, std::string_view message, std::string field,
                                std::string relevant) {
        diagnostics.push_back(make_diagnostic(
            Phase::machine_sets, code, message, "machine", machine_id, std::move(field), {},
            core::detail::json_object<Json>([&](auto& object_3) {
                core::detail::set_json(object_3, "relevant_set", std::move(relevant));
            })));
    };
    if (!machine.online_cpus.is_subset_of(machine.present_cpus)) {
        report_set("topology.set.online_cpus_outside_present",
                   "Online CPU set is not a subset of present CPUs", "online_cpus",
                   machine.online_cpus.difference(machine.present_cpus).to_range_list());
    }

    std::map<core::LinuxCpuId, std::uint32_t> represented;
    for (const auto& pu : topology.processing_units()) {
        ++represented[pu.linux_cpu_id];
        if (!machine.present_cpus.contains(pu.linux_cpu_id)) {
            diagnostics.push_back(make_diagnostic(
                Phase::machine_sets, "topology.cpu.represented_outside_present",
                "Processing unit Linux CPU is outside the present set", "processing_unit",
                format_topology_id(pu.id), "linux_cpu_id",
                numeric_reference("cpu", pu.linux_cpu_id)));
        }
    }
    for (const auto cpu : machine.online_cpus) {
        if (represented[cpu] != 1U) {
            diagnostics.push_back(make_diagnostic(
                Phase::machine_sets, "topology.cpu.online_unrepresented",
                "Online Linux CPU does not have exactly one processing unit", "machine", machine_id,
                "online_cpus", numeric_reference("cpu", cpu),
                core::detail::json_object<Json>([&](auto& object_4) {
                    core::detail::set_json(object_4, "representation_count", represented[cpu]);
                })));
        }
    }

    const auto check_cpu_subset = [&](const Attribute<core::CpuSet>& attribute,
                                      std::string_view outside_online_code, std::string field) {
        if (attribute.availability() == Availability::available &&
            !attribute.value()->is_subset_of(machine.online_cpus)) {
            report_set(outside_online_code, "CPU resource set is outside online CPUs", field,
                       attribute.value()->difference(machine.online_cpus).to_range_list());
        }
    };
    check_cpu_subset(machine.allowed_cpus, "topology.set.allowed_cpus_outside_online",
                     "allowed_cpus");
    check_cpu_subset(machine.effective_cpus, "topology.set.effective_cpus_outside_online",
                     "effective_cpus");
    if (machine.allowed_cpus.availability() == Availability::available &&
        machine.effective_cpus.availability() == Availability::available &&
        !machine.effective_cpus.value()->is_subset_of(*machine.allowed_cpus.value())) {
        report_set("topology.set.effective_cpus_outside_allowed",
                   "Effective CPU set is outside allowed CPUs", "effective_cpus",
                   machine.effective_cpus.value()
                       ->difference(*machine.allowed_cpus.value())
                       .to_range_list());
    }

    if (!machine.online_nodes.is_subset_of(machine.present_nodes)) {
        report_set("topology.set.online_nodes_outside_present",
                   "Online node set is not a subset of present nodes", "online_nodes",
                   machine.online_nodes.difference(machine.present_nodes).to_range_list());
    }
    std::map<std::uint32_t, core::NumaNodeId> linux_nodes;
    for (const auto& node : topology.numa_nodes()) {
        if (node.linux_node_id.availability() != Availability::available) {
            continue;
        }
        const auto linux_id = *node.linux_node_id.value();
        if (!machine.present_nodes.contains(core::NumaNodeId{linux_id})) {
            diagnostics.push_back(make_diagnostic(
                Phase::machine_sets, "topology.numa.linux_node_outside_present",
                "NUMA Linux node ID is outside the machine present-node set", "numa_node",
                format_topology_id(node.id), "linux_node_id", numeric_reference("node", linux_id)));
        }
        const auto [existing, inserted] = linux_nodes.try_emplace(linux_id, node.id);
        if (!inserted) {
            diagnostics.push_back(make_diagnostic(
                Phase::machine_sets, "topology.numa.duplicate_linux_node_id",
                "Multiple NUMA nodes claim the same Linux node ID", "numa_node",
                format_topology_id(node.id), "linux_node_id", numeric_reference("node", linux_id),
                core::detail::json_object<Json>([&](auto& object_5) {
                    core::detail::set_json(object_5, "first_object_id",
                                           format_topology_id(existing->second));
                })));
        }
    }

    const auto check_node_subset = [&](const Attribute<core::NodeSet>& attribute,
                                       std::string_view outside_online_code, std::string field) {
        if (attribute.availability() == Availability::available &&
            !attribute.value()->is_subset_of(machine.online_nodes)) {
            report_set(outside_online_code, "Node resource set is outside online nodes", field,
                       attribute.value()->difference(machine.online_nodes).to_range_list());
        }
    };
    check_node_subset(machine.allowed_nodes, "topology.set.allowed_nodes_outside_online",
                      "allowed_nodes");
    check_node_subset(machine.effective_nodes, "topology.set.effective_nodes_outside_online",
                      "effective_nodes");
    if (machine.allowed_nodes.availability() == Availability::available &&
        machine.effective_nodes.availability() == Availability::available &&
        !machine.effective_nodes.value()->is_subset_of(*machine.allowed_nodes.value())) {
        report_set("topology.set.effective_nodes_outside_allowed",
                   "Effective node set is outside allowed nodes", "effective_nodes",
                   machine.effective_nodes.value()
                       ->difference(*machine.allowed_nodes.value())
                       .to_range_list());
    }
}

void validate_cores(const TopologySnapshot& topology,
                    PendingDiagnostics& diagnostics) {
    for (const auto& core : topology.cores()) {
        if (topology.core_members(core.id).empty()) {
            diagnostics.push_back(make_diagnostic(
                Phase::core_structure, "topology.core.empty_membership",
                "Core has no processing-unit members", "core", format_topology_id(core.id),
                "processing_units"));
        }
    }
}

void validate_packages_and_groups(const TopologySnapshot& topology,
                                  PendingDiagnostics& diagnostics) {
    for (const auto& die : topology.dies()) {
        if (!die.group_id) {
            continue;
        }
        const auto group = topology.architecture_group(*die.group_id);
        if (group && group->package_id != die.package_id) {
            diagnostics.push_back(make_diagnostic(
                Phase::package_structure, "topology.group.die_package_mismatch",
                "Die and architecture group belong to different packages", "die",
                format_topology_id(die.id), "group_id", format_topology_id(*die.group_id)));
        }
    }
    for (const auto& group : topology.architecture_groups()) {
        for (const auto pu : group.member_pus) {
            const auto package = pu_package(topology, pu);
            if (package && *package != group.package_id) {
                diagnostics.push_back(make_diagnostic(
                    Phase::package_structure, "topology.group.member_package_mismatch",
                    "Architecture-group member belongs to another package", "architecture_group",
                    format_topology_id(group.id), "member_pus", format_topology_id(pu),
                    core::detail::json_object<Json>([&](auto& object_6) {
                        core::detail::set_json(object_6, "group_package",
                                               format_topology_id(group.package_id));
                        core::detail::set_json(object_6, "member_package",
                                               format_topology_id(*package));
                    })));
            }
        }
        if (!group.parent_group_id) {
            continue;
        }
        const auto parent = topology.architecture_group(*group.parent_group_id);
        if (!parent) {
            continue;
        }
        if (parent->package_id != group.package_id) {
            diagnostics.push_back(make_diagnostic(
                Phase::package_structure, "topology.group.parent_package_mismatch",
                "Child and parent architecture groups belong to different packages",
                "architecture_group", format_topology_id(group.id), "parent_group_id",
                format_topology_id(parent->id)));
        }
        if (!group.member_pus.is_subset_of(parent->member_pus)) {
            diagnostics.push_back(make_diagnostic(
                Phase::package_structure, "topology.group.child_members_outside_parent",
                "Child architecture-group members are outside the parent group",
                "architecture_group", format_topology_id(group.id), "member_pus",
                format_topology_id(parent->id),
                core::detail::json_object<Json>([&](auto& object_7) {
                    core::detail::set_json(
                        object_7, "relevant_set",
                        group.member_pus.difference(parent->member_pus).to_range_list());
                })));
        }
    }

    enum class Visit { unseen, active, complete };
    std::map<core::ArchitectureGroupId, Visit> visits;
    const auto visit = [&](const auto& self, core::ArchitectureGroupId id) -> void {
        if (visits[id] == Visit::complete) {
            return;
        }
        if (visits[id] == Visit::active) {
            diagnostics.push_back(make_diagnostic(
                Phase::package_structure, "topology.group.containment_cycle",
                "Architecture-group parent relationship contains a cycle", "architecture_group",
                format_topology_id(id), "parent_group_id", format_topology_id(id)));
            return;
        }
        visits[id] = Visit::active;
        const auto group = topology.architecture_group(id);
        if (group && group->parent_group_id && topology.architecture_group(*group->parent_group_id)) {
            self(self, *group->parent_group_id);
        }
        visits[id] = Visit::complete;
    };
    for (const auto& group : topology.architecture_groups()) {
        visit(visit, group.id);
    }
}

void validate_caches(const TopologySnapshot& topology,
                     PendingDiagnostics& diagnostics) {
    for (const auto& cache : topology.caches()) {
        const auto id = format_topology_id(cache.id);
        if (cache.member_pus.empty()) {
            diagnostics.push_back(make_diagnostic(
                Phase::caches, "topology.cache.empty_membership", "Cache membership is empty",
                "cache", id, "member_pus"));
        }
        if (cache.level == 0U) {
            diagnostics.push_back(make_diagnostic(Phase::caches, "topology.cache.invalid_level",
                                                  "Cache level must be greater than zero", "cache",
                                                  id, "level"));
        }
        if (cache.size.value() == 0U) {
            diagnostics.push_back(make_diagnostic(Phase::caches, "topology.cache.invalid_size",
                                                  "Cache size must be greater than zero", "cache",
                                                  id, "size"));
        }
        if (cache.line_size.value() == 0U) {
            diagnostics.push_back(make_diagnostic(
                Phase::caches, "topology.cache.invalid_line_size",
                "Cache line size must be greater than zero", "cache", id, "line_size"));
        }
        if (cache.associativity.availability() == Availability::available &&
            *cache.associativity.value() == 0U) {
            diagnostics.push_back(make_diagnostic(
                Phase::caches, "topology.cache.invalid_associativity",
                "Available cache associativity must be greater than zero", "cache", id,
                "associativity"));
        }
    }
}

void validate_numa(const TopologySnapshot& topology,
                   PendingDiagnostics& diagnostics) {
    std::map<core::ProcessingUnitId, std::vector<core::NumaNodeId>> memberships;
    for (const auto& node : topology.numa_nodes()) {
        const auto id = format_topology_id(node.id);
        for (const auto pu : node.member_pus) {
            if (topology.processing_unit(pu)) {
                memberships[pu].push_back(node.id);
            }
        }
        if (node.installed_memory.availability() == Availability::available &&
            node.available_memory.availability() == Availability::available &&
            node.available_memory.value()->value() > node.installed_memory.value()->value()) {
            diagnostics.push_back(make_diagnostic(
                Phase::numa, "topology.numa.available_memory_exceeds_installed",
                "Available memory exceeds installed memory", "numa_node", id, "available_memory"));
        }
        for (std::size_t index = 0; index < node.page_sizes.size(); ++index) {
            if (node.page_sizes[index].value() == 0U) {
                diagnostics.push_back(make_diagnostic(
                    Phase::numa, "topology.numa.invalid_page_size",
                    "NUMA page size must be greater than zero", "numa_node", id,
                    "page_sizes/" + std::to_string(index)));
            }
        }
    }
    const auto machine_pages = detail::derive_machine_page_sizes(topology.numa_nodes());
    if (machine_pages && topology.machine().page_sizes != *machine_pages) {
        diagnostics.push_back(make_diagnostic(
            Phase::numa, "topology.numa.machine_page_sizes_mismatch",
            "Machine page sizes must equal the canonical union of NUMA-node page sizes",
            "machine", "machine", "page_sizes"));
    }
    for (std::size_t index = 0; index < topology.machine().page_sizes.size(); ++index) {
        if (topology.machine().page_sizes[index].value() == 0U) {
            diagnostics.push_back(make_diagnostic(
                Phase::numa, "topology.numa.invalid_machine_page_size",
                "Machine page size must be greater than zero", "machine", "machine",
                "page_sizes/" + std::to_string(index)));
        }
    }
    for (const auto& [pu, nodes] : memberships) {
        if (nodes.size() > 1U) {
            diagnostics.push_back(
                make_diagnostic(Phase::numa, "topology.numa.overlapping_membership",
                                "Processing unit belongs to multiple NUMA nodes", "processing_unit",
                                format_topology_id(pu), "numa_membership", {},
                                core::detail::json_object<Json>([&](auto& object_8) {
                                    core::detail::set_json(object_8, "node_ids", Json::array());
                                })));
            auto& node_ids = diagnostics.back().error.context["node_ids"];
            for (const auto node : nodes) {
                node_ids.push_back(format_topology_id(node));
            }
        }
    }
    const auto has_online_nodes = !topology.machine().online_nodes.empty();
    if (has_online_nodes) {
        for (const auto& pu : topology.processing_units()) {
            if (topology.machine().online_cpus.contains(pu.linux_cpu_id) && memberships[pu.id].empty()) {
                diagnostics.push_back(make_diagnostic(
                    Phase::numa, "topology.numa.online_cpu_without_membership",
                    "Online processing unit has no NUMA membership", "processing_unit",
                    format_topology_id(pu.id), "numa_membership"));
            }
        }
    }
}

void validate_distances(const TopologySnapshot& topology,
                        PendingDiagnostics& diagnostics) {
    const auto& matrix = topology.numa_distances();
    std::vector<core::NumaNodeId> represented;
    represented.reserve(topology.numa_nodes().size());
    for (const auto& node : topology.numa_nodes()) {
        represented.push_back(node.id);
    }
    if (matrix.nodes != represented) {
        diagnostics.push_back(make_diagnostic(
            Phase::distances, "topology.distance.node_set_mismatch",
            "Distance matrix node set differs from represented NUMA nodes", "numa_distance_matrix",
            "distance_matrix", "nodes", {}, core::detail::json_object<Json>([&](auto& object_9) {
                core::detail::set_json(object_9, "matrix_nodes", formatted_ids(matrix.nodes));
                core::detail::set_json(object_9, "represented_nodes", formatted_ids(represented));
            })));
    }
    for (const auto node : matrix.nodes) {
        if (!topology.numa_node(node)) {
            diagnostics.push_back(make_diagnostic(
                Phase::distances, "topology.distance.matrix_unknown_node",
                "Distance matrix declares an unknown NUMA node", "numa_distance_matrix",
                "distance_matrix", "nodes", format_topology_id(node)));
        }
    }
    for (const auto& entry : matrix.directed_distances) {
        const auto entry_id = format_topology_id(entry.from) + "->" + format_topology_id(entry.to);
        if (!contains_id(matrix.nodes, entry.from) || !contains_id(matrix.nodes, entry.to)) {
            diagnostics.push_back(make_diagnostic(
                Phase::distances, "topology.distance.entry_outside_matrix",
                "Distance entry references a node outside the declared matrix", "numa_distance",
                entry_id, "directed_distances"));
        }
        if (entry.value == 0U) {
            diagnostics.push_back(make_diagnostic(
                Phase::distances, "topology.distance.nonpositive_value",
                "NUMA distance must be greater than zero", "numa_distance", entry_id, "value"));
        }
    }
    for (const auto from : matrix.nodes) {
        for (const auto to : matrix.nodes) {
            if (!matrix.distance(from, to)) {
                diagnostics.push_back(make_diagnostic(
                    Phase::distances, "topology.distance.incomplete_matrix",
                    "Directed NUMA distance pair is missing", "numa_distance_matrix",
                    "distance_matrix", "directed_distances",
                    format_topology_id(from) + "->" + format_topology_id(to)));
            }
        }
    }
}

void validate_attributes(const TopologySnapshot& topology,
                         PendingDiagnostics& diagnostics) {
    const auto& machine = topology.machine();
    const auto mid = format_topology_id(machine.id);
    check_conflict(machine.cpu_vendor, "machine", mid, "cpu_vendor", diagnostics);
    check_conflict(machine.cpu_model, "machine", mid, "cpu_model", diagnostics);
    check_conflict(machine.total_memory, "machine", mid, "total_memory", diagnostics);
    check_conflict(machine.allowed_cpus, "machine", mid, "allowed_cpus", diagnostics);
    check_conflict(machine.effective_cpus, "machine", mid, "effective_cpus", diagnostics);
    check_conflict(machine.allowed_nodes, "machine", mid, "allowed_nodes", diagnostics);
    check_conflict(machine.effective_nodes, "machine", mid, "effective_nodes", diagnostics);
    for (const auto& package : topology.packages()) {
        check_conflict(package.os_id, "package", format_topology_id(package.id), "os_id", diagnostics);
    }
    for (const auto& die : topology.dies()) {
        check_conflict(die.os_id, "die", format_topology_id(die.id), "os_id", diagnostics);
    }
    for (const auto& core : topology.cores()) {
        check_conflict(core.os_id, "core", format_topology_id(core.id), "os_id", diagnostics);
    }
    for (const auto& pu : topology.processing_units()) {
        check_conflict(pu.isolated, "processing_unit", format_topology_id(pu.id), "isolated",
                       diagnostics);
    }
    for (const auto& cache : topology.caches()) {
        check_conflict(cache.associativity, "cache", format_topology_id(cache.id), "associativity",
                       diagnostics);
    }
    for (const auto& node : topology.numa_nodes()) {
        const auto id = format_topology_id(node.id);
        check_conflict(node.linux_node_id, "numa_node", id, "linux_node_id", diagnostics);
        check_conflict(node.installed_memory, "numa_node", id, "installed_memory", diagnostics);
        check_conflict(node.available_memory, "numa_node", id, "available_memory", diagnostics);
    }
    for (const auto& device : topology.pci_devices()) {
        const auto id = pci_id(device.address);
        check_conflict(device.numa_locality, "pci_device", id, "numa_locality", diagnostics);
        check_conflict(device.local_pus, "pci_device", id, "local_pus", diagnostics);
    }
}

} // namespace

auto detail::validate_topology_with_failure_sink(const TopologySnapshot& topology,
    std::optional<core::Error>* raw_failure, SourceValidationEligibility eligibility)
    -> ValidationReport {
    PendingDiagnostics pending{raw_failure, eligibility};
    try {
    validate_references(topology, pending);
    validate_machine_sets(topology, pending);
    validate_cores(topology, pending);
    validate_packages_and_groups(topology, pending);
    validate_caches(topology, pending);
    validate_numa(topology, pending);
    validate_distances(topology, pending);
    validate_attributes(topology, pending);

    std::ranges::sort(pending, {}, [](const PendingDiagnostic& diagnostic) {
        return std::tie(diagnostic.phase, diagnostic.object_type, diagnostic.object_id,
                        diagnostic.field_path, diagnostic.code, diagnostic.reference);
    });
    ValidationReport report;
    report.diagnostics.reserve(pending.size());
    for (auto& diagnostic : pending) {
        report.diagnostics.push_back(std::move(diagnostic.error));
    }
    return report;
    } catch (...) {
        // Candidate appends retain before their temporary ends. Later phase,
        // report-reserve and sorting failures still have this owning stock.
        pending.retain();
        throw;
    }
}

auto validate_topology(const TopologySnapshot& topology) -> ValidationReport {
    return detail::validate_topology_with_failure_sink(topology, nullptr);
}

} // namespace epycbench::topology
