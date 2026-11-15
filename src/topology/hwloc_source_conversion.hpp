#pragma once

#include "epycbench/topology/hwloc_source.hpp"
#include "epycbench/topology/model.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace epycbench::topology::detail {

// One converter algorithm instantiated with the actual installed or prefixed
// native types. Each API owns its native boundary; no layout casts or copies.
template<class Api> class HwlocSourceConversion final {
  public:
    explicit HwlocSourceConversion(Api& api) noexcept : api_{api} {}
    using ConstBitmap = typename Api::ConstBitmap;
    using Topology = typename Api::Topology;
    using Object = typename Api::Object;
    using ObjectType = typename Api::ObjectType;
    using CacheType = typename Api::CacheType;
    using Distance = typename Api::Distance;
    static constexpr auto constant_distances_kind_means_bandwidth = Api::constant_distances_kind_means_bandwidth;
    static constexpr auto constant_distances_kind_means_latency = Api::constant_distances_kind_means_latency;
    static constexpr auto constant_obj_cache_data = Api::constant_obj_cache_data;
    static constexpr auto constant_obj_cache_instruction = Api::constant_obj_cache_instruction;
    static constexpr auto constant_obj_cache_unified = Api::constant_obj_cache_unified;
    static constexpr auto constant_obj_core = Api::constant_obj_core;
    static constexpr auto constant_obj_die = Api::constant_obj_die;
    static constexpr auto constant_obj_group = Api::constant_obj_group;
    static constexpr auto constant_obj_l1cache = Api::constant_obj_l1cache;
    static constexpr auto constant_obj_l1icache = Api::constant_obj_l1icache;
    static constexpr auto constant_obj_l2cache = Api::constant_obj_l2cache;
    static constexpr auto constant_obj_l2icache = Api::constant_obj_l2icache;
    static constexpr auto constant_obj_l3cache = Api::constant_obj_l3cache;
    static constexpr auto constant_obj_l3icache = Api::constant_obj_l3icache;
    static constexpr auto constant_obj_l4cache = Api::constant_obj_l4cache;
    static constexpr auto constant_obj_l5cache = Api::constant_obj_l5cache;
    static constexpr auto constant_obj_memcache = Api::constant_obj_memcache;
    static constexpr auto constant_obj_numanode = Api::constant_obj_numanode;
    static constexpr auto constant_obj_package = Api::constant_obj_package;
    static constexpr auto constant_obj_pci_device = Api::constant_obj_pci_device;
    static constexpr auto constant_obj_pu = Api::constant_obj_pu;
    static constexpr auto constant_unknown_index = Api::constant_unknown_index;
    static constexpr auto constant_version = Api::constant_version;
struct DiagnosticError final : core::Error {
    explicit DiagnosticError(core::Error&& value) noexcept : core::Error(std::move(value)) {}
    DiagnosticError(DiagnosticError&& other) noexcept : core::Error(std::move(other)) {}
    auto operator=(DiagnosticError&& other) noexcept -> DiagnosticError& {
        if (this != &other) {
            core::detail::retire_json(context);
            core::Error::operator=(std::move(other));
        }
        return *this;
    }
    ~DiagnosticError() { core::detail::retire_json(context); }
};
struct EvidenceCleanup {
    TopologySourceEvidence& evidence;
    ~EvidenceCleanup() {
        for (auto& error : evidence.diagnostics) core::detail::retire_json(error.context);
    }
};
static_assert(std::is_nothrow_move_constructible_v<core::Error>);
static_assert(std::is_nothrow_move_assignable_v<core::Error>);

static void sort_diagnostics(TopologySourceEvidence& evidence) {
    std::vector<DiagnosticError> pending;
    pending.reserve(evidence.diagnostics.size());
    for (auto& error : evidence.diagnostics) pending.emplace_back(std::move(error));
    // Sorting may allocate comparator strings; pivots must own Core retirement.
    std::ranges::sort(pending, [](const auto& left, const auto& right) {
        return std::pair{left.code.value(), left.context.dump()} <
               std::pair{right.code.value(), right.context.dump()};
    });
    for (std::size_t index = 0; index < pending.size(); ++index) {
        evidence.diagnostics[index] = std::move(static_cast<core::Error&>(pending[index]));
    }
}

static auto error(std::string code, std::string message, std::string operation) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::topology,
        .code = core::StableErrorCode{std::move(code)},
        .message = std::move(message),
        .operation = std::move(operation),
        .os_error = std::nullopt,
        .context = nlohmann::json::object(),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

template <class F>
static auto error(std::string code, std::string message, std::string operation, F&& fill) -> core::Error {
    // Acquire strings first; a raw structured argument could otherwise die
    // before entering this helper when another argument allocation fails.
    auto context = core::detail::json_object<nlohmann::json>(std::forward<F>(fill));
    core::detail::JsonCleanup cleanup{context};
    auto result = error(std::move(code), std::move(message), std::move(operation));
    result.context.swap(context);
    return result;
}

static auto warning(std::string code, std::string message, std::string locator) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::topology,
        .code = core::StableErrorCode{std::move(code)},
        .message = std::move(message),
        .operation = "discover_hwloc_topology",
        .os_error = std::nullopt,
        .context = core::detail::json_object<nlohmann::json>([&](auto& object) {
            core::detail::set_json(object, "locator", std::move(locator));
        }),
        .severity = core::ErrorSeverity::warning,
        .continuation = core::ContinuationPolicy::continue_operation,
    };
}

static void append_warning(TopologySourceEvidence& result, std::string code,
                    std::string message, std::string locator) {
    DiagnosticError owned{warning(std::move(code), std::move(message), std::move(locator))};
    result.diagnostics.push_back(std::move(static_cast<core::Error&>(owned)));
}

static auto provenance(std::string locator, SourceConfidence confidence = SourceConfidence::reported)
    -> std::vector<SourceProvenance> {
    return {{TopologyEvidenceSource::hwloc, std::move(locator), confidence}};
}

template <typename T>
static auto available(T value, std::string locator,
               SourceConfidence confidence = SourceConfidence::reported) -> SourceObservation<T> {
    return {SourceAvailability::available, std::move(value),
            provenance(std::move(locator), confidence)};
}

template <typename T> auto unavailable(std::string locator) -> SourceObservation<T> {
    return {SourceAvailability::unavailable, std::nullopt, provenance(std::move(locator))};
}

auto cpu_set(ConstBitmap bitmap) -> core::CpuSet {
    std::string cpus;
    if (bitmap != nullptr) {
        int cpu = -1;
        while ((cpu = api_.bitmap_next(bitmap, cpu)) >= 0) {
            if (!cpus.empty()) {
                cpus.push_back(',');
            }
            cpus += std::to_string(cpu);
        }
    }
    if (cpus.empty()) {
        return {};
    }
    return *core::CpuSet::parse(cpus);
}

auto node_set(ConstBitmap bitmap) -> core::NodeSet {
    std::string nodes;
    if (bitmap != nullptr) {
        int node = -1;
        while ((node = api_.bitmap_next(bitmap, node)) >= 0) {
            if (!nodes.empty()) {
                nodes.push_back(',');
            }
            nodes += std::to_string(node);
        }
    }
    if (nodes.empty()) {
        return {};
    }
    return *core::NodeSet::parse(nodes);
}

auto object_key(Object object) -> SourceObjectKey {
    return {cpu_set(object == nullptr ? nullptr : object->cpuset),
            object != nullptr && object->os_index != constant_unknown_index
                ? std::optional<std::uint32_t>{object->os_index}
                : std::nullopt};
}

static auto key_text(const SourceObjectKey& key) -> std::string {
    return key.member_cpus.to_range_list() + '|' +
           (key.os_id ? std::to_string(*key.os_id) : std::string{"?"});
}

auto locator(Object object) -> std::string {
    std::array<char, 32> type{};
    static_cast<void>(api_.obj_type_snprintf(type.data(), type.size(), object, 0));
    return std::string{"hwloc:"} + type.data() + ":cpus=" + cpu_set(object->cpuset).to_range_list() +
           ":os=" +
           (object->os_index == constant_unknown_index ? std::string{"?"}
                                                    : std::to_string(object->os_index));
}

static auto ancestor(Object object, ObjectType type) -> Object {
    for (auto* parent = object == nullptr ? nullptr : object->parent; parent != nullptr;
         parent = parent->parent) {
        if (parent->type == type) {
            return parent;
        }
    }
    return nullptr;
}

static auto group_ancestors(Object object) -> std::vector<Object> {
    std::vector<Object> groups;
    for (auto* parent = object == nullptr ? nullptr : object->parent; parent != nullptr;
         parent = parent->parent) {
        if (parent->type == constant_obj_group) {
            groups.push_back(parent);
        }
    }
    return groups;
}

static void collect_groups(Object object, std::vector<Object>& groups) {
    if (object == nullptr) {
        return;
    }
    if (object->type == constant_obj_group) {
        groups.push_back(object);
    }
    for (unsigned index = 0; index < object->arity; ++index) {
        collect_groups(object->children[index], groups);
    }
}

auto info(Object object, const char* name) -> std::optional<std::string> {
    const auto* value = api_.obj_get_info_by_name(object, name);
    return value == nullptr ? std::nullopt : std::optional<std::string>{value};
}

static auto semantic_less(const SourceObjectKey& left, const SourceObjectKey& right) -> bool {
    return std::pair{left.member_cpus.to_range_list(), left.os_id} <
           std::pair{right.member_cpus.to_range_list(), right.os_id};
}

auto machine_cpu_identity(Topology topology, Object root, const char* name,
                          bool reverse_packages_for_test = false)
    -> SourceObservation<std::string> {
    const auto root_locator = std::string{"hwloc:Machine:"} + name;
    if (const auto root_value = info(root, name); root_value && !root_value->empty()) {
        return available(*root_value, root_locator);
    }

    std::vector<Object> packages;
    for (auto* object = api_.get_next_obj_by_type(topology, constant_obj_package, nullptr);
         object != nullptr; object = api_.get_next_obj_by_type(topology, constant_obj_package, object)) {
        packages.push_back(object);
    }
    if (reverse_packages_for_test) {
        std::ranges::reverse(packages);
    }
    std::ranges::sort(packages, [this](Object left, Object right) {
        return semantic_less(object_key(left), object_key(right));
    });
    if (packages.empty()) {
        return unavailable<std::string>(root_locator);
    }

    std::vector<SourceProvenance> package_provenance;
    package_provenance.reserve(packages.size());
    std::optional<std::string> unanimous;
    bool agrees = true;
    for (auto* package : packages) {
        package_provenance.push_back(
            {TopologyEvidenceSource::hwloc, locator(package) + ':' + name,
             SourceConfidence::reported});
        const auto value = info(package, name);
        if (!value || value->empty()) {
            agrees = false;
            continue;
        }
        if (!unanimous) {
            unanimous = *value;
        } else if (*unanimous != *value) {
            agrees = false;
        }
    }
    if (!agrees || !unanimous) {
        return {SourceAvailability::unavailable, std::nullopt, std::move(package_provenance)};
    }
    for (auto& item : package_provenance) {
        item.confidence = SourceConfidence::derived;
    }
    return {SourceAvailability::available, std::move(unanimous),
            std::move(package_provenance)};
}

template <typename Range, typename Projection>
static auto has_duplicate(const Range& range, Projection projection) -> bool {
    return std::ranges::adjacent_find(range, {}, projection) != range.end();
}

static auto cache_kind(CacheType kind) -> SourceCacheKind {
    switch (kind) {
    case constant_obj_cache_data:
        return SourceCacheKind::data;
    case constant_obj_cache_instruction:
        return SourceCacheKind::instruction;
    case constant_obj_cache_unified:
        return SourceCacheKind::unified;
    }
    return SourceCacheKind::unknown;
}

static auto cache_types() -> std::array<ObjectType, 9> {
    return {constant_obj_l1cache, constant_obj_l2cache, constant_obj_l3cache, constant_obj_l4cache,
            constant_obj_l5cache, constant_obj_l1icache, constant_obj_l2icache, constant_obj_l3icache,
            constant_obj_memcache};
}

class DistanceMatrixBatch final {
  public:
    DistanceMatrixBatch(Api& api, Topology topology, std::span<Distance*> matrices,
                        const typename Api::DistanceHooks* hooks) noexcept
        : api_{api}, topology_{topology}, matrices_{matrices}, hooks_{hooks} {}
    DistanceMatrixBatch(const DistanceMatrixBatch&) = delete;
    auto operator=(const DistanceMatrixBatch&) -> DistanceMatrixBatch& = delete;
    ~DistanceMatrixBatch() noexcept {
        if (armed_) {
            for (std::size_t index = 0; index < matrices_.size(); ++index) release(index);
        }
    }
    void arm() noexcept { armed_ = true; }
    void release(std::size_t index) noexcept {
        if (auto* matrix = std::exchange(matrices_[index], nullptr)) {
            if (hooks_ != nullptr && hooks_->release != nullptr)
                hooks_->release(hooks_->context, topology_, matrix);
            else
                api_.distances_release(topology_, matrix);
        }
    }
  private:
    Api& api_;
    Topology topology_;
    std::span<Distance*> matrices_;
    const typename Api::DistanceHooks* hooks_;
    bool armed_{};
};

auto convert_distances(Topology topology, TopologySourceEvidence& result,
                       const typename Api::DistanceHooks* hooks = nullptr)
    -> std::expected<void, core::Error> {
    const auto get = [&](unsigned* count, Distance** matrices) {
        return hooks != nullptr && hooks->get != nullptr
                   ? hooks->get(hooks->context, topology, count, matrices, 0UL, 0UL)
                   : api_.distances_get(topology, count, matrices, 0UL, 0UL);
    };
    unsigned count = 0;
    if (get(&count, nullptr) != 0) {
        append_warning(result, "topology.hwloc.unsupported_distance_matrix",
                                             "hwloc distance matrices are unavailable",
                                             "hwloc:distances");
        return {};
    }
    std::vector<Distance*> matrices(count, nullptr);
    DistanceMatrixBatch owners{api_, topology, matrices, hooks};
    if (count != 0) {
        if (get(&count, matrices.data()) != 0) {
            // Failed get retains library ownership; its partial slots may dangle.
            append_warning(result, "topology.hwloc.unsupported_distance_matrix",
                                                 "hwloc distance matrices could not be read",
                                                 "hwloc:distances");
            return {};
        }
        owners.arm(); // Successful transfer: no fallible registration window.
    }
    if (count > matrices.size()) {
        return std::unexpected{error("topology.hwloc.malformed_relationship",
                                     "hwloc distance matrix count exceeds acquired capacity",
                                     "discover_hwloc_topology")};
    }
    for (unsigned matrix_index = 0; matrix_index < count; ++matrix_index) {
        auto* matrix = matrices[matrix_index];
        if (matrix == nullptr) {
            return std::unexpected{error("topology.hwloc.malformed_relationship",
                                         "hwloc returned an absent distance matrix",
                                         "discover_hwloc_topology")};
        }
        if (hooks != nullptr && hooks->before_convert != nullptr)
            hooks->before_convert(hooks->context, matrix_index);
        const auto name_pointer = api_.distances_get_name(topology, matrix);
        const std::string name = name_pointer == nullptr ? "" : name_pointer;
        const auto meaning_bits = matrix->kind &
                                  (constant_distances_kind_means_latency |
                                   constant_distances_kind_means_bandwidth);
        bool nodes_only = matrix->nbobjs > 0;
        std::set<std::uint32_t> node_ids;
        for (unsigned index = 0; index < matrix->nbobjs; ++index) {
            const auto* object = matrix->objs[index];
            nodes_only = nodes_only && object != nullptr && object->type == constant_obj_numanode &&
                         object->os_index != constant_unknown_index;
            if (nodes_only) {
                node_ids.insert(object->os_index);
            }
        }
        if (!nodes_only || node_ids.size() != matrix->nbobjs ||
            meaning_bits != constant_distances_kind_means_latency) {
            append_warning(result,
                "topology.hwloc.unsupported_distance_matrix",
                "Distance matrix is not an unambiguous NUMA latency matrix",
                "hwloc:distance:" + (name.empty() ? std::string{"unnamed"} : name));
            owners.release(matrix_index);
            continue;
        }
        NumaDistanceSourceEvidence evidence{
            .name = name,
            .meaning = DistanceMeaning::latency,
            .units = "hwloc-relative-latency",
            .linux_nodes = {node_ids.begin(), node_ids.end()},
            .directed_values = {},
            .provenance = provenance("hwloc:distance:" + name),
        };
        for (unsigned from = 0; from < matrix->nbobjs; ++from) {
            for (unsigned to = 0; to < matrix->nbobjs; ++to) {
                evidence.directed_values.push_back(
                    {matrix->objs[from]->os_index, matrix->objs[to]->os_index,
                     matrix->values[static_cast<std::size_t>(from) * matrix->nbobjs + to]});
            }
        }
        std::ranges::sort(evidence.directed_values);
        result.numa_distances.push_back(std::move(evidence));
        owners.release(matrix_index);
    }
    std::ranges::sort(result.numa_distances, {}, &NumaDistanceSourceEvidence::name);
    return {};
}

auto convert(Topology topology, const HwlocDiscoveryOptions& options,
             bool reverse_identity_packages_for_test = false)
    -> std::expected<TopologySourceEvidence, core::Error> {
    TopologySourceEvidence result;
    EvidenceCleanup cleanup{result};
    result.source = TopologyEvidenceSource::hwloc;
    result.source_version = constant_version;
    auto* root = api_.get_root_obj(topology);
    if (root == nullptr) {
        return std::unexpected{error("topology.hwloc.malformed_relationship",
                                     "hwloc did not provide a machine root",
                                     "discover_hwloc_topology")};
    }

    result.machine.architecture = info(root, "Architecture")
                                      ? available(*info(root, "Architecture"),
                                                  "hwloc:Machine:Architecture")
                                      : unavailable<std::string>("hwloc:Machine:Architecture");
    result.machine.cpu_vendor = machine_cpu_identity(topology, root, "CPUVendor",
                                                     reverse_identity_packages_for_test);
    result.machine.cpu_model = machine_cpu_identity(topology, root, "CPUModel",
                                                    reverse_identity_packages_for_test);
    result.machine.total_memory =
        available(core::ByteCount::from_base_units(root->total_memory), "hwloc:Machine:memory");
    result.machine.cpu_sets.topology =
        available(cpu_set(api_.topology_get_topology_cpuset(topology)), "hwloc:topology_cpuset");
    result.machine.cpu_sets.complete =
        available(cpu_set(api_.topology_get_complete_cpuset(topology)), "hwloc:complete_cpuset");
    result.machine.cpu_sets.source_reported_allowed =
        available(cpu_set(api_.topology_get_allowed_cpuset(topology)), "hwloc:allowed_cpuset");
    result.machine.node_sets.topology =
        available(node_set(api_.topology_get_topology_nodeset(topology)), "hwloc:topology_nodeset");
    result.machine.node_sets.complete =
        available(node_set(api_.topology_get_complete_nodeset(topology)), "hwloc:complete_nodeset");
    result.machine.node_sets.source_reported_allowed =
        available(node_set(api_.topology_get_allowed_nodeset(topology)), "hwloc:allowed_nodeset");

    for (auto* object = api_.get_next_obj_by_type(topology, constant_obj_package, nullptr);
         object != nullptr; object = api_.get_next_obj_by_type(topology, constant_obj_package, object)) {
        result.packages.push_back({object_key(object), provenance(locator(object))});
    }
    for (auto* object = api_.get_next_obj_by_type(topology, constant_obj_die, nullptr);
         object != nullptr; object = api_.get_next_obj_by_type(topology, constant_obj_die, object)) {
        auto* package = ancestor(object, constant_obj_package);
        if (package == nullptr) {
            return std::unexpected{error("topology.hwloc.malformed_relationship",
                                         "A die has no package ancestor",
                                         "discover_hwloc_topology", [&](auto& context) {
                                             core::detail::set_json(context, "object", locator(object));
                                         })};
        }
        result.dies.push_back({object_key(object), object_key(package), provenance(locator(object))});
    }
    std::vector<Object> hwloc_groups;
    collect_groups(root, hwloc_groups);
    for (auto* object : hwloc_groups) {
        auto* parent = ancestor(object, constant_obj_group);
        result.architecture_groups.push_back({
            .key = object_key(object),
            .kind = object->attr == nullptr ? "generic" : "hwloc-group-" +
                                                               std::to_string(object->attr->group.kind),
            .subtype = object->subtype == nullptr
                           ? unavailable<std::string>(locator(object) + ":subtype")
                           : available(std::string{object->subtype}, locator(object) + ":subtype"),
            .parent = parent == nullptr ? std::nullopt
                                       : std::optional<SourceObjectKey>{object_key(parent)},
            .provenance = provenance(locator(object)),
        });
    }
    for (auto* object = api_.get_next_obj_by_type(topology, constant_obj_core, nullptr);
         object != nullptr; object = api_.get_next_obj_by_type(topology, constant_obj_core, object)) {
        auto* package = ancestor(object, constant_obj_package);
        auto* die = ancestor(object, constant_obj_die);
        result.cores.push_back({
            object_key(object),
            package == nullptr ? std::nullopt
                               : std::optional<SourceObjectKey>{object_key(package)},
            die == nullptr ? std::nullopt : std::optional<SourceObjectKey>{object_key(die)},
            provenance(locator(object)),
        });
    }
    for (auto* object = api_.get_next_obj_by_type(topology, constant_obj_pu, nullptr);
         object != nullptr; object = api_.get_next_obj_by_type(topology, constant_obj_pu, object)) {
        if (object->os_index == constant_unknown_index) {
            return std::unexpected{error("topology.hwloc.missing_cpu_os_index",
                                         "A processing unit has no Linux OS CPU number",
                                         "discover_hwloc_topology")};
        }
        if (std::ranges::any_of(result.processing_units, [&](const auto& existing) {
                return existing.linux_cpu_id == core::LinuxCpuId{object->os_index};
            })) {
            return std::unexpected{error("topology.hwloc.duplicate_semantic_object",
                                         "Multiple processing units have the same Linux CPU number",
                                         "discover_hwloc_topology")};
        }
        auto* core = ancestor(object, constant_obj_core);
        if (core == nullptr || !object_key(core).member_cpus.contains(core::LinuxCpuId{object->os_index})) {
            return std::unexpected{error("topology.hwloc.malformed_relationship",
                                         "A processing unit has no usable core relationship",
                                         "discover_hwloc_topology",
                                         [&](auto& context) {
                                             core::detail::set_json(context, "linux_cpu", object->os_index);
                                         })};
        }
        auto* package = ancestor(object, constant_obj_package);
        auto* die = ancestor(object, constant_obj_die);
        std::vector<SourceObjectKey> groups;
        for (auto* group : group_ancestors(object)) {
            groups.push_back(object_key(group));
        }
        std::ranges::sort(groups, semantic_less);
        result.processing_units.push_back({
            core::LinuxCpuId{object->os_index}, unavailable<bool>(locator(object) + ":online"),
            object_key(core),
            package == nullptr ? std::nullopt
                               : std::optional<SourceObjectKey>{object_key(package)},
            die == nullptr ? std::nullopt : std::optional<SourceObjectKey>{object_key(die)},
            std::move(groups), provenance(locator(object))});
    }
    if (result.processing_units.empty()) {
        return std::unexpected{error("topology.hwloc.no_processing_units",
                                     "hwloc returned no usable processing units",
                                     "discover_hwloc_topology")};
    }
    std::ranges::sort(result.processing_units, {}, &ProcessingUnitSourceEvidence::linux_cpu_id);
    if (has_duplicate(result.processing_units, &ProcessingUnitSourceEvidence::linux_cpu_id)) {
        return std::unexpected{error("topology.hwloc.duplicate_semantic_object",
                                     "Multiple processing units have the same Linux CPU number",
                                     "discover_hwloc_topology")};
    }

    for (const auto type : cache_types()) {
        for (auto* object = api_.get_next_obj_by_type(topology, type, nullptr); object != nullptr;
             object = api_.get_next_obj_by_type(topology, type, object)) {
            if (object->attr == nullptr || object->cpuset == nullptr ||
                api_.bitmap_iszero(object->cpuset)) {
                return std::unexpected{error("topology.hwloc.malformed_relationship",
                                             "A cache has no usable CPU membership",
                                             "discover_hwloc_topology")};
            }
            const auto associativity = object->attr->cache.associativity > 0
                                           ? available(static_cast<std::uint32_t>(
                                                           object->attr->cache.associativity),
                                                       locator(object) + ":associativity")
                                           : unavailable<std::uint32_t>(locator(object) +
                                                                         ":associativity");
            result.caches.push_back({
                object->attr->cache.depth, cache_kind(object->attr->cache.type),
                core::ByteCount::from_base_units(object->attr->cache.size),
                core::ByteCount::from_base_units(object->attr->cache.linesize), associativity,
                unavailable<std::uint32_t>(locator(object) + ":source_cache_id"),
                unavailable<std::uint64_t>(locator(object) + ":number_of_sets"),
                unavailable<std::uint32_t>(locator(object) + ":physical_line_partition"),
                unavailable<std::string>(locator(object) + ":allocation_policy"),
                unavailable<std::string>(locator(object) + ":write_policy"),
                cpu_set(object->cpuset), provenance(locator(object))});
        }
    }

    for (auto* object = api_.get_next_obj_by_type(topology, constant_obj_numanode, nullptr);
         object != nullptr; object = api_.get_next_obj_by_type(topology, constant_obj_numanode, object)) {
        const auto linux_id = object->os_index == constant_unknown_index
                                  ? unavailable<std::uint32_t>(locator(object) + ":os_index")
                                  : available(static_cast<std::uint32_t>(object->os_index),
                                              locator(object) + ":os_index");
        std::vector<core::ByteCount> page_sizes;
        if (object->attr != nullptr) {
            for (unsigned index = 0; index < object->attr->numanode.page_types_len; ++index) {
                page_sizes.push_back(core::ByteCount::from_base_units(
                    object->attr->numanode.page_types[index].size));
            }
        }
        result.numa_nodes.push_back({
            linux_id, cpu_set(object->cpuset),
            object->attr == nullptr
                ? unavailable<core::ByteCount>(locator(object) + ":local_memory")
                : available(core::ByteCount::from_base_units(object->attr->numanode.local_memory),
                            locator(object) + ":local_memory"),
            std::move(page_sizes), provenance(locator(object))});
    }

    if (options.include_pci) {
        for (auto* object = api_.get_next_obj_by_type(topology, constant_obj_pci_device, nullptr);
             object != nullptr;
             object = api_.get_next_obj_by_type(topology, constant_obj_pci_device, object)) {
            if (object->attr == nullptr) {
                return std::unexpected{error("topology.hwloc.invalid_pci_address",
                                             "A PCI function has no numeric identity",
                                             "discover_hwloc_topology")};
            }
            const auto& pci = object->attr->pcidev;
            if (pci.domain > std::numeric_limits<std::uint16_t>::max() || pci.dev > 0x1fU ||
                pci.func > 7U) {
                return std::unexpected{error("topology.hwloc.invalid_pci_address",
                                             "A PCI function has an invalid BDF",
                                             "discover_hwloc_topology")};
            }
            const auto address = PciAddress::create(static_cast<std::uint16_t>(pci.domain), pci.bus,
                                                    pci.dev, pci.func);
            if (!address) {
                return std::unexpected{error("topology.hwloc.invalid_pci_address",
                                             "A PCI function has an invalid BDF",
                                             "discover_hwloc_topology")};
            }
            auto* locality = object->parent;
            while (locality != nullptr && locality->cpuset == nullptr) {
                locality = locality->parent;
            }
            const auto local_cpus = locality == nullptr
                                        ? unavailable<core::CpuSet>("hwloc:PCI:" +
                                                                     address->to_string() +
                                                                     ":local_cpus")
                                        : available(cpu_set(locality->cpuset),
                                                    "hwloc:PCI:" + address->to_string() +
                                                        ":local_cpus",
                                                    SourceConfidence::derived);
            auto numa = unavailable<std::uint32_t>("hwloc:PCI:" + address->to_string() +
                                                   ":numa_node");
            if (locality != nullptr && locality->nodeset != nullptr &&
                api_.bitmap_weight(locality->nodeset) == 1) {
                numa = available(static_cast<std::uint32_t>(api_.bitmap_first(locality->nodeset)),
                                 "hwloc:PCI:" + address->to_string() + ":numa_node",
                                 SourceConfidence::derived);
            }
            result.pci_functions.push_back(
                {address->to_string(), pci.vendor_id, pci.device_id,
                 static_cast<std::uint32_t>(pci.class_id) << 8U,
                 available(static_cast<std::uint16_t>(pci.subvendor_id),
                           "hwloc:PCI:" + address->to_string() + ":subsystem_vendor"),
                 available(static_cast<std::uint16_t>(pci.subdevice_id),
                           "hwloc:PCI:" + address->to_string() + ":subsystem_device"),
                 local_cpus, numa,
                 provenance("hwloc:PCI:" + address->to_string())});
        }
    }
    if (options.include_distances) {
        auto converted = convert_distances(topology, result);
        if (!converted) {
            return std::unexpected{std::move(converted.error())};
        }
    }

    const auto sort_objects = [](auto& objects, auto projection) {
        std::ranges::sort(objects, [&](const auto& left, const auto& right) {
            return semantic_less(std::invoke(projection, left), std::invoke(projection, right));
        });
    };
    sort_objects(result.packages, &PackageSourceEvidence::key);
    sort_objects(result.dies, &DieSourceEvidence::key);
    std::ranges::sort(result.architecture_groups, [](const auto& left, const auto& right) {
        const auto left_parent = left.parent ? key_text(*left.parent) : std::string{};
        const auto right_parent = right.parent ? key_text(*right.parent) : std::string{};
        return std::tuple{key_text(left.key), left.kind, left.subtype.value, left_parent} <
               std::tuple{key_text(right.key), right.kind, right.subtype.value, right_parent};
    });
    sort_objects(result.cores, &CoreSourceEvidence::key);
    std::ranges::sort(result.caches, [](const auto& left, const auto& right) {
        return std::tuple{left.level, left.kind, left.shared_cpus.to_range_list(), left.size.value(),
                          left.line_size.value()} <
               std::tuple{right.level, right.kind, right.shared_cpus.to_range_list(),
                          right.size.value(), right.line_size.value()};
    });
    std::ranges::sort(result.numa_nodes, [](const auto& left, const auto& right) {
        return std::pair{left.linux_node_id.value, left.member_cpus.to_range_list()} <
               std::pair{right.linux_node_id.value, right.member_cpus.to_range_list()};
    });
    std::ranges::sort(result.pci_functions, {}, &PciFunctionSourceEvidence::bdf);
    const auto duplicate_group = std::ranges::adjacent_find(
        result.architecture_groups, [](const auto& left, const auto& right) {
            return left.key == right.key && left.kind == right.kind &&
                   left.subtype.value == right.subtype.value && left.parent == right.parent;
        });
    const auto duplicate_cache = std::ranges::adjacent_find(
        result.caches, [](const auto& left, const auto& right) {
            return left.level == right.level && left.kind == right.kind &&
                   left.shared_cpus == right.shared_cpus;
        });
    const auto duplicate_node = std::ranges::adjacent_find(
        result.numa_nodes, [](const auto& left, const auto& right) {
            return left.linux_node_id.value && right.linux_node_id.value &&
                   left.linux_node_id.value == right.linux_node_id.value;
        });
    if (has_duplicate(result.packages, [](const auto& value) { return key_text(value.key); }) ||
        has_duplicate(result.dies, [](const auto& value) { return key_text(value.key); }) ||
        has_duplicate(result.cores, [](const auto& value) { return key_text(value.key); }) ||
        has_duplicate(result.pci_functions, &PciFunctionSourceEvidence::bdf) ||
        duplicate_group != result.architecture_groups.end() ||
        duplicate_cache != result.caches.end() || duplicate_node != result.numa_nodes.end()) {
        return std::unexpected{error("topology.hwloc.duplicate_semantic_object",
                                     "hwloc returned duplicate semantic topology evidence",
                                     "discover_hwloc_topology")};
    }
    sort_diagnostics(result);
    return std::expected<TopologySourceEvidence, core::Error>{std::move(result)};
}

  private:
    Api& api_;
};

} // namespace epycbench::topology::detail
