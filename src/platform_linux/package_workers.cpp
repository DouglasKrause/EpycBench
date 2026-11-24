#include "platform_linux/package_workers.hpp"

#include "epycbench/platform_linux/memory_mapping.hpp"
#include "epycbench/platform_linux/numa_policy.hpp"
#include "epycbench/platform_linux/page_location.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"
#include "epycbench/platform_linux/thread_affinity.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"
#include "core/binary_digest.hpp"
#include "platform_linux/host_work_admission.hpp"
#include "platform_linux/package_worker_monitoring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <latch>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux::package_execution {
namespace {
using core::LinuxCpuId;
using core::LinuxNumaNodeId;

auto failure(std::string_view code, std::string_view message,
             nlohmann::json context = nlohmann::json::object()) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::string{code}},
            .message = std::string{message},
            .operation = "Package.run_workers", .os_error = std::nullopt, .context = std::move(context)};
}

auto pages_for(std::uint64_t bytes, std::uint64_t page_bytes) noexcept -> std::uint64_t {
    return 1U + (bytes - 1U) / page_bytes;
}

auto mapping_bytes(std::uint64_t bytes, std::uint64_t alignment, std::uint64_t page_bytes) noexcept
    -> std::uint64_t {
    const auto mapped = pages_for(bytes, page_bytes) * page_bytes;
    return mapped + (alignment > page_bytes ? alignment - page_bytes : 0U);
}

auto replica_policy(InputPolicy policy) noexcept -> bool {
    return policy == InputPolicy::replica_per_worker || policy == InputPolicy::replica_per_node;
}

auto distribution_qualified(const RegionPlacement& region, bool after) noexcept -> bool {
    const auto resolved = after ? region.resident_after : region.resident_before;
    const auto unresolved = after ? region.unresolved_after : region.unresolved_before;
    if (region.targets.empty() || region.pages == 0 || resolved != region.pages || unresolved != 0) return false;
    if (region.distribution.empty())
        return region.targets.size() == 1U && region.node == region.targets.front() &&
            region.policy == InputPolicy::bind;
    if (region.policy == InputPolicy::interleave && (region.targets.size() < 2U || region.pages < region.targets.size()))
        return false;
    std::uint64_t remaining = region.pages;
    const auto minimum = region.pages / region.targets.size();
    const auto maximum = minimum + (region.pages % region.targets.size() != 0 ? 1U : 0U);
    for (std::size_t i = 0; i < region.distribution.size(); ++i) {
        const auto& row = region.distribution[i];
        if (std::ranges::find(std::span{region.distribution}.first(i), row.node, &NodeResidency::node) !=
            std::span{region.distribution}.first(i).end()) return false;
        const auto count = after ? row.after : row.before;
        if (!std::ranges::contains(region.targets, row.node)) return false;
        if (count > remaining) return false;
        remaining -= count;
    }
    if (region.policy == InputPolicy::interleave) {
        for (const auto target : region.targets) {
            const auto row = std::ranges::find(region.distribution, target, &NodeResidency::node);
            if (row == region.distribution.end()) return false;
            const auto count = after ? row->after : row->before;
            if (count < minimum || count > maximum) return false;
        }
    }
    return remaining == 0;
}

struct NativeResource {
    struct WorkingRegion {
        std::string name;
        std::shared_ptr<AnonymousMapping> owner;
        std::span<std::byte> bytes;
        std::uint32_t kind;
        LinuxNumaNodeId node;
        InputPolicy policy;
        std::vector<LinuxNumaNodeId> targets;
    };

    std::uint64_t page_bytes;
    LinuxNumaNodeId node;
    NativeNumaMask nodes;
    struct SharedNode {
        LinuxNumaNodeId node;
        NativeNumaMask nodes;
    };
    std::optional<SharedNode> shared_node;
    InputPlacement input_placement;
    std::optional<NativeNumaMask> input_mask;
    bool override_shared_input;
    std::vector<WorkingRegion> working;

    static auto allocate(void* context, const AllocationRequest& request)
        -> std::expected<Allocation, core::Error> {
        auto& self = *static_cast<NativeResource*>(context);
        const auto length = mapping_bytes(request.bytes, request.alignment, self.page_bytes);
        auto native = AnonymousMapping::create(static_cast<std::size_t>(length));
        if (!native) return std::unexpected{std::move(native.error())};
        if (auto advice = native->disable_transparent_huge_pages(); !advice)
            return std::unexpected{std::move(advice.error())};
        const auto use_shared = self.shared_node &&
            (request.kind == EPYCBENCH_PACKAGE_SHARED || request.kind == EPYCBENCH_PACKAGE_SHARED_INPUT);
        const auto selected_input = !request.reference_output && (request.kind == EPYCBENCH_PACKAGE_INPUT ||
            (request.kind == EPYCBENCH_PACKAGE_SHARED_INPUT && self.override_shared_input));
        const auto& mask = selected_input && self.input_mask ? *self.input_mask :
            (use_shared ? self.shared_node->nodes : self.nodes);
        const auto policy = selected_input ? self.input_placement.policy : InputPolicy::bind;
        auto bound = policy == InputPolicy::interleave ? interleave_memory(*native, mask) : bind_memory(*native, mask);
        if (!bound)
            return std::unexpected{std::move(bound.error())};
        auto owner = std::make_shared<AnonymousMapping>(std::move(*native));
        const auto address = reinterpret_cast<std::uintptr_t>(owner->data());
        const auto offset = (request.alignment - address % request.alignment) % request.alignment;
        const auto bytes = owner->writable_bytes().subspan(offset, request.bytes);
        if (!request.reference_output) {
            const auto target = use_shared ? self.shared_node->node : self.node;
            auto targets = selected_input && !self.input_placement.targets.empty() ? self.input_placement.targets :
                std::vector{target};
            self.working.push_back({std::string{request.name}, owner, bytes, request.kind,
                targets.front(), policy, std::move(targets)});
        }
        return Allocation{std::move(owner), bytes};
    }

    auto protect_inputs(std::string_view writable_handoff = {}) const -> std::expected<void, core::Error> {
        for (const auto& region : working) {
            if (region.kind != EPYCBENCH_PACKAGE_SHARED_INPUT && region.kind != EPYCBENCH_PACKAGE_INPUT) continue;
            if (region.name == writable_handoff) continue;
            auto protected_input = region.owner->protect(MemoryProtection::read_only);
            if (!protected_input) return protected_input;
        }
        return {};
    }

    void retire_inputs(Placement& placement, std::optional<core::Error>& failure) {
        for (auto& region : working) {
            if (region.kind != EPYCBENCH_PACKAGE_INPUT && region.kind != EPYCBENCH_PACKAGE_SHARED_INPUT) continue;
            auto released = region.owner->release();
            const auto location = std::ranges::find(placement.regions, region.name, &RegionPlacement::name);
            if (location != placement.regions.end()) location->released = released.has_value();
            if (!released && !failure) failure = std::move(released.error());
        }
    }

    auto observe(Placement& placement, bool after) const -> std::expected<void, core::Error> {
        const core::CpuSet expected{placement.requested_cpu};
        auto affinity = get_current_thread_affinity();
        if (!affinity) return std::unexpected{std::move(affinity.error())};
        auto cpu = current_cpu();
        if (!cpu) return std::unexpected{std::move(cpu.error())};
        (after ? placement.cpu_after : placement.cpu_before) = *cpu;
        if (*affinity != expected || *cpu != placement.requested_cpu)
            return std::unexpected{failure("runtime.package.cpu_placement_mismatch",
                                           "Worker affinity or observed CPU differs from its request")};

        bool placed = true;
        constexpr std::size_t batch_size = 4096;
        std::vector<void*> pages;
        pages.reserve(batch_size);
        for (std::size_t i = 0; i < working.size(); ++i) {
            const auto& region = working[i];
            const auto count = pages_for(region.bytes.size(), page_bytes);
            auto& entry = after ? placement.regions[i] : placement.regions.emplace_back(RegionPlacement{
                .name = region.name, .pages = count, .node = region.node, .policy = region.policy,
                .targets = region.targets});
            auto& resident = after ? entry.resident_after : entry.resident_before;
            auto& unresolved = after ? entry.unresolved_after : entry.unresolved_before;
            for (std::uint64_t cursor = 0; cursor < count;) {
                const auto batch = std::min<std::uint64_t>(batch_size, count - cursor);
                pages.clear();
                for (std::uint64_t i_page = 0; i_page < batch; ++i_page)
                    pages.push_back(region.bytes.data() + (cursor + i_page) * page_bytes);
                const auto locations = query_page_locations(pages);
                if (!locations) return std::unexpected{locations.error()};
                if (locations->size() != pages.size())
                    return std::unexpected{failure("runtime.package.page_query_inconsistent",
                                                   "NUMA query did not report every requested page")};
                for (const auto& location : *locations) {
                    if (location.kind() != PageQueryStatusKind::resident || !location.node()) {
                        ++unresolved;
                        continue;
                    }
                    const auto found = std::ranges::find(entry.distribution, *location.node(), &NodeResidency::node);
                    auto& row = found == entry.distribution.end() ?
                        entry.distribution.emplace_back(NodeResidency{.node = *location.node()}) : *found;
                    ++(after ? row.after : row.before);
                    if (std::ranges::contains(entry.targets, row.node)) ++resident;
                }
                cursor += batch;
            }
            std::ranges::sort(entry.distribution, {}, &NodeResidency::node);
            placed = distribution_qualified(entry, after) && placed;
        }
        if (!placed)
            return std::unexpected{failure("runtime.package.numa_placement_mismatch",
                                           "Some working pages did not reside on their requested NUMA node")};
        return {};
    }
};

struct RankPlan {
    std::size_t role_index;
    Request request;
    EpycBenchPackageLayoutV1 layout;
    std::optional<EpycBenchPackageLayoutV1> shared_layout;
    std::optional<LinuxNumaNodeId> shared_node;
    std::uint32_t replica_owner{};
    std::vector<std::string_view> exports{};
};

struct MemoryDemand {
    std::uint64_t peak{};
    std::map<LinuxNumaNodeId, std::uint64_t> nodes;
};

struct RegionPolicy {
    InputPolicy policy;
    std::vector<LinuxNumaNodeId> targets;
};

auto region_policy(const Role& role, std::optional<LinuxNumaNodeId> shared_node,
                   const EpycBenchPackageRegionRequirementV1& region,
                   LinuxNumaNodeId local_node) -> RegionPolicy {
    const auto shared = region.kind == EPYCBENCH_PACKAGE_SHARED || region.kind == EPYCBENCH_PACKAGE_SHARED_INPUT;
    const auto node = shared ? *shared_node : local_node;
    const auto& placement = role.input_placement;
    const auto selected = region.kind == EPYCBENCH_PACKAGE_INPUT ||
        (region.kind == EPYCBENCH_PACKAGE_SHARED_INPUT && !role.module.shared_cpu_local() && !replica_policy(placement.policy));
    return {selected ? placement.policy : InputPolicy::bind,
        selected && !placement.targets.empty() ? placement.targets : std::vector{node}};
}

auto memory_node(const Role& role, std::uint32_t rank) noexcept -> LinuxNumaNodeId {
    return role.request.locality.empty() ? role.node : LinuxNumaNodeId{role.request.locality[rank].memory_node};
}

auto replica_group(const Role& role) noexcept -> std::string_view {
    return role.input_placement.replica_group.empty() ? role.id : role.input_placement.replica_group;
}

auto cross_role_replicas(std::span<const Role> roles) -> bool {
    std::set<std::string_view> groups;
    for (const auto& role : roles) {
        if (replica_policy(role.input_placement.policy) && !groups.insert(replica_group(role)).second)
            return true;
    }
    return false;
}

auto find_region(const EpycBenchPackageLayoutV1& layout, std::string_view name, std::uint32_t kind)
    -> const EpycBenchPackageRegionRequirementV1* {
    const auto regions = std::span{layout.regions}.first(layout.region_count);
    const auto found = std::ranges::find_if(regions, [&](const auto& region) {
        return region.name == name && region.kind == kind;
    });
    return found == regions.end() ? nullptr : &*found;
}

auto budget_error(std::uint64_t budget) -> core::Error {
    return failure("runtime.package.group_budget_exceeded", "Package group allocation budget exceeded",
                   {{"maximum_region_bytes", budget}});
}

auto plan_ranks(std::span<const Role> roles, Composition composition, std::uint64_t budget,
                std::uint64_t page_bytes, MemoryDemand* demand = nullptr)
    -> std::expected<std::vector<RankPlan>, core::Error> {
    if (roles.empty() || (composition == Composition::mixed && roles.size() < 2U) ||
        (composition == Composition::pipeline && roles.size() < 2U))
        return std::unexpected{failure("runtime.package.composition_invalid", "Invalid package role composition")};
    std::vector<RankPlan> result;
    std::set<std::string_view> seen_roles;
    std::set<LinuxCpuId> seen_cpus;
    std::map<std::string_view, InputPolicy> replica_policies;
    std::map<std::string_view, std::set<LinuxNumaNodeId>> group_nodes;
    const bool co_live = composition == Composition::mixed ||
        (composition == Composition::sequential && cross_role_replicas(roles));
    if (co_live && composition == Composition::sequential &&
        std::ranges::any_of(roles, [](const Role& role) { return role.module.collective(); }))
        return std::unexpected{failure("runtime.package.replica_composition_invalid",
            "Sequential cross-role replica proofs require independent rank callbacks")};
    std::uint64_t available = budget;
    std::uint64_t retained = 0;
    std::map<LinuxNumaNodeId, std::uint64_t> active_by_node, retained_by_node;
    const auto reserve = [&](std::uint64_t bytes, std::uint64_t alignment, unsigned copies) {
        if (bytes == 0) return true;
        const auto extent = page_bytes == 0 ? std::max(bytes, alignment) : mapping_bytes(bytes, alignment, page_bytes);
        if (extent > available / copies) return false;
        available -= extent * copies;
        return true;
    };
    const auto charge_nodes = [&](std::uint64_t bytes, const RegionPolicy& policy) {
        if (!demand || bytes == 0) return;
        const auto extent = policy.policy == InputPolicy::interleave ?
            pages_for(bytes, page_bytes * policy.targets.size()) * page_bytes : bytes;
        for (const auto target : policy.targets) {
            auto& total = active_by_node[target];
            total += std::min(extent, UINT64_MAX - total);
        }
    };
    for (std::size_t role_index = 0; role_index < roles.size(); ++role_index) {
        const auto& role = roles[role_index];
        const auto staged_predecessor = role_index > 0 && composition == Composition::pipeline;
        const auto& input = role.input_placement;
        const auto targets = std::set(input.targets.begin(), input.targets.end());
        if (role.id.empty() || !seen_roles.insert(role.id).second || role.cpus.empty() || role.cpus.size() > 1024 ||
            (!role.request.locality.empty() && role.request.locality.size() != role.cpus.size()) ||
            (!role.request.inspection.empty() && role.request.inspection.size() != role.cpus.size()) ||
            (staged_predecessor && (role.links.empty() || roles[role_index - 1U].cpus.size() != role.cpus.size())) ||
            (!staged_predecessor && !role.links.empty()))
            return std::unexpected{failure("runtime.package.role_invalid", "Invalid package role or stage links",
                                           {{"role", role.id}})};
        if (input.targets.size() != targets.size() || input.targets.size() > 1024 ||
            (input.policy == InputPolicy::interleave && input.targets.size() < 2U) ||
            (replica_policy(input.policy) && !input.targets.empty()) ||
            (!replica_policy(input.policy) && !input.replica_group.empty()))
            return std::unexpected{failure("runtime.package.input_placement_invalid", "Invalid input targets or replica group",
                                           {{"role", role.id}})};
        if (replica_policy(input.policy)) {
            const auto [group, fresh] = replica_policies.try_emplace(replica_group(role), input.policy);
            if ((!fresh && (composition == Composition::pipeline || group->second != input.policy)) ||
                (input.policy == InputPolicy::replica_per_node && !role.links.empty()))
                return std::unexpected{failure("runtime.package.replica_scope_invalid",
                    "Replica group must have matching ownership and no dependent stage sharing", {{"role", role.id}})};
        }
        if (composition != Composition::mixed) seen_cpus.clear();
        if (!co_live) {
            available = budget - retained;
            active_by_node = std::move(retained_by_node);
            retained_by_node.clear();
        }
        std::uint64_t retain_for_next_stage = 0;
        std::map<LinuxNumaNodeId, std::uint32_t> replica_owners;
        const auto successor_links = composition == Composition::pipeline && role_index + 1U < roles.size()
            ? std::span<const StageLink>{roles[role_index + 1U].links} : std::span<const StageLink>{};
        bool has_qualifying_inputs = false;
        for (std::size_t index = 0; index < role.cpus.size(); ++index) {
            if (!role.request.locality.empty() && role.request.locality[index].cpu != role.cpus[index].value())
                return std::unexpected{failure("runtime.package.locality_mismatch",
                    "Rank CPU differs from its topology locality", {{"role", role.id}, {"rank", index}})};
            if (!seen_cpus.insert(role.cpus[index]).second || seen_cpus.size() > 1024)
                return std::unexpected{failure("runtime.package.cpu_conflict",
                                               "Concurrent workers require up to 1024 distinct CPUs",
                                               {{"role", role.id}, {"cpu", role.cpus[index].value()}})};
            auto request = role.request;
            request.rank = static_cast<std::uint32_t>(index);
            request.rank_count = static_cast<std::uint32_t>(role.cpus.size());
            request.supplied_workers = page_bytes == 0;
            const auto placement_node = memory_node(role, request.rank);
            const auto replica_owner = input.policy == InputPolicy::replica_per_node
                ? replica_owners.try_emplace(placement_node, request.rank).first->second : request.rank;
            if (input.policy == InputPolicy::replica_per_node && replica_owner == request.rank &&
                !group_nodes[replica_group(role)].insert(placement_node).second)
                return std::unexpected{failure("runtime.package.replica_owner_conflict",
                    "One per-node replica group cannot have distinct-role owners on the same node", {{"role", role.id}})};
            if (replica_owner != request.rank && !role.module.per_worker_input())
                return std::unexpected{failure("runtime.package.replica_input_not_shareable",
                    "Per-node borrowing requires a task with replica-compatible per-worker input", {{"role", role.id}})};
            const auto preparation_node = request.locality.empty() || request.locality[index].linux_numa_node == UINT32_MAX
                ? placement_node : LinuxNumaNodeId{request.locality[index].linux_numa_node};
            auto geometry = layout(role.module, request, budget);
            if (!geometry) return std::unexpected{std::move(geometry.error())};
            if (replica_policy(input.policy)) {
                const auto requirements = std::span{geometry->regions}.first(geometry->region_count);
                const auto input_count = std::ranges::count(requirements, EPYCBENCH_PACKAGE_INPUT,
                    &EpycBenchPackageRegionRequirementV1::kind);
                if (input_count == 0)
                    return std::unexpected{failure("runtime.package.replica_inputs_missing",
                        "Replica policy requires at least one rank-local immutable input", {{"role", role.id}})};
                if (replica_owner != request.rank) {
                    const auto& original = result[result.size() - index + replica_owner].layout;
                    const auto owned = std::span{original.regions}.first(original.region_count);
                    const auto count = std::ranges::count(owned, EPYCBENCH_PACKAGE_INPUT,
                        &EpycBenchPackageRegionRequirementV1::kind);
                    if (count != input_count)
                        return std::unexpected{failure("runtime.package.replica_layout_mismatch",
                            "Replica owner and borrower input regions differ", {{"role", role.id}})};
                    for (const auto& region : requirements) {
                        if (region.kind != EPYCBENCH_PACKAGE_INPUT) continue;
                        const auto* match = find_region(original, region.name, EPYCBENCH_PACKAGE_INPUT);
                        if (!match || match->bytes != region.bytes || match->alignment < region.alignment)
                            return std::unexpected{failure("runtime.package.replica_layout_mismatch",
                                "Replica inputs must have matching region dimensions", {{"role", role.id}, {"region", region.name}})};
                    }
                }
            }
            if (!reserve(geometry->preparation_bytes, alignof(std::max_align_t), 1))
                return std::unexpected{budget_error(budget)};
            if (demand && geometry->preparation_bytes != 0)
                charge_nodes(mapping_bytes(geometry->preparation_bytes, alignof(std::max_align_t), page_bytes),
                             {InputPolicy::bind, {preparation_node}});
            std::optional<EpycBenchPackageLayoutV1> shared;
            std::optional<LinuxNumaNodeId> shared_node;
            if (index == 0 && role.module.collective()) {
                shared_node = memory_node(role, 0);
                if (role.module.shared_cpu_local()) {
                    if (request.locality.empty() || request.locality[0].linux_numa_node == UINT32_MAX)
                        return std::unexpected{failure("runtime.package.collective_node_unavailable",
                            "Collective task requires rank-zero CPU-local shared placement", {{"role", role.id}})};
                    shared_node = LinuxNumaNodeId{request.locality[0].linux_numa_node};
                }
                auto shared_regions = collective_layout(role.module, request, budget);
                if (!shared_regions) return std::unexpected{std::move(shared_regions.error())};
                if (!reserve(shared_regions->preparation_bytes, alignof(std::max_align_t), 1))
                    return std::unexpected{budget_error(budget)};
                if (demand && shared_regions->preparation_bytes != 0)
                    charge_nodes(mapping_bytes(shared_regions->preparation_bytes, alignof(std::max_align_t), page_bytes),
                                 {InputPolicy::bind, {preparation_node}});
                for (const auto& region : std::span{shared_regions->regions}.first(shared_regions->region_count)) {
                    const auto local = std::span{geometry->regions}.first(geometry->region_count);
                    if (std::ranges::any_of(local, [&](const auto& item) {
                            return std::string_view{item.name} == region.name;
                        }))
                        return std::unexpected{failure("runtime.package.collective_region_conflict",
                            "Collective and owner-local regions must have distinct names", {{"region", region.name}})};
                }
                shared = *shared_regions;
            }
            const auto inspect_pages = !request.inspection.empty() ? request.inspection[index].base_page_bytes : 0;
            const auto placement_page = page_bytes == 0 ? inspect_pages : page_bytes;
            const auto charge_region = [&](const EpycBenchPackageRegionRequirementV1& region, unsigned copies)
                -> std::expected<void, core::Error> {
                const auto policy = region_policy(role, shared_node, region, placement_node);
                if (region.kind == EPYCBENCH_PACKAGE_INPUT ||
                    (region.kind == EPYCBENCH_PACKAGE_SHARED_INPUT && !role.module.shared_cpu_local()))
                    has_qualifying_inputs = true;
                if (policy.policy == InputPolicy::interleave && placement_page != 0 &&
                    (region.bytes % placement_page != 0 || region.bytes / placement_page < policy.targets.size()))
                    return std::unexpected{failure("runtime.package.interleave_extent_invalid",
                        "Interleaving requires an integral base-page extent with at least one page per target",
                        {{"role", role.id}, {"region", region.name}})};
                if (!reserve(region.bytes, region.alignment, copies)) return std::unexpected{budget_error(budget)};
                if (demand) {
                    const auto extent = mapping_bytes(region.bytes, region.alignment, page_bytes);
                    charge_nodes(extent, policy);
                    if (copies == 2)
                        charge_nodes(extent, {InputPolicy::bind,
                            {region.kind == EPYCBENCH_PACKAGE_SHARED || region.kind == EPYCBENCH_PACKAGE_SHARED_INPUT
                                ? *shared_node : placement_node}});
                }
                return {};
            };
            if (shared) {
                for (const auto& region : std::span{shared->regions}.first(shared->region_count)) {
                    auto charged = charge_region(region, 2);
                    if (!charged) return std::unexpected{std::move(charged.error())};
                }
            }
            for (const auto& region : std::span{geometry->regions}.first(geometry->region_count)) {
                if (region.kind == EPYCBENCH_PACKAGE_INPUT && replica_owner != request.rank) continue;
                const auto copies = region.kind == EPYCBENCH_PACKAGE_OUTPUT ? 2U : 1U;
                auto charged = charge_region(region, copies);
                if (!charged) return std::unexpected{std::move(charged.error())};
                if (region.kind == EPYCBENCH_PACKAGE_OUTPUT &&
                    std::ranges::contains(successor_links, region.name, &StageLink::output)) {
                    const auto extent = page_bytes == 0 ? std::max(region.bytes, region.alignment) :
                        mapping_bytes(region.bytes, region.alignment, page_bytes);
                    retain_for_next_stage += extent;
                    if (demand) retained_by_node[placement_node] += extent;
                }
            }
            if (staged_predecessor) {
                const auto& previous = result[result.size() - role.cpus.size()];
                std::set<std::string_view> input_names;
                for (const auto& link : role.links) {
                    const auto* source = find_region(previous.layout, link.output, EPYCBENCH_PACKAGE_OUTPUT);
                    const auto* target = find_region(*geometry, link.input, EPYCBENCH_PACKAGE_INPUT);
                    if (!input_names.insert(link.input).second || !source || !target || source->bytes != target->bytes)
                        return std::unexpected{failure("runtime.package.stage_link_invalid",
                            "Pipeline regions must identify one matching-size predecessor output and a distinct input",
                            {{"role", role.id}, {"rank", index}, {"input", link.input}, {"output", link.output}})};
                }
            }
            std::vector<std::string_view> exports;
            for (const auto& link : successor_links) {
                if (!std::ranges::contains(exports, link.output)) exports.push_back(link.output);
            }
            result.push_back({role_index, std::move(request), *geometry, std::move(shared), shared_node, replica_owner,
                std::move(exports)});
        }
        if (!has_qualifying_inputs && (input.policy != InputPolicy::bind || !input.targets.empty()))
            return std::unexpected{failure("runtime.package.input_regions_missing",
                "An explicit immutable input policy requires an applicable input region", {{"role", role.id}})};
        if (demand) {
            demand->peak = std::max(demand->peak, budget - available);
            for (const auto& [node, bytes] : active_by_node)
                demand->nodes[node] = std::max(demand->nodes[node], bytes);
        }
        retained = retain_for_next_stage;
    }
    return result;
}

auto native_memory_limit() -> std::expected<std::uint64_t, core::Error> {
    const auto operations = host_work::production_limit_operations();
    host_work::LimitPair address_space{};
    const auto read = operations.get(operations.context, host_work::NativeLimit::address_space, &address_space);
    if (read.value != 0 || read.error_number != 0)
        return std::unexpected{failure("runtime.package.address_space_limit_unavailable",
                                       "Cannot determine the Runner address-space limit", {{"errno", read.error_number}})};
    return address_space.soft;
}

auto check_headroom(const MemoryDemand& demand) -> std::expected<void, core::Error> {
    if (demand.peak == 0) return {};
    std::vector<host_work::NodeMemoryDemand> nodes;
    nodes.reserve(demand.nodes.size());
    for (const auto& [node, bytes] : demand.nodes)
        nodes.push_back({node, core::ByteCount::from_base_units(bytes)});
    const auto result = host_work::read_headroom(core::ByteCount::from_base_units(demand.peak), nodes,
        host_work::production_read_operations());
    if (!result)
        return std::unexpected{failure("runtime.package.memory_headroom_failed",
            "Native package allocations exceed available memory or memory admission failed",
            {{"required_bytes", demand.peak}, {"selected_nodes", nodes.size()},
             {"reason", static_cast<unsigned>(result.error().primary)}})};
    return {};
}

auto validate_supplied(Placement& placement, const RankPlan& plan, const Role& role,
                       Clock clock) -> std::expected<void, core::Error> {
    const auto cpu = role.cpus[plan.request.rank];
    const auto node = memory_node(role, plan.request.rank);
    const auto expected_regions = plan.layout.region_count +
        (plan.shared_layout ? plan.shared_layout->region_count : 0U);
    if (!clock.now || placement.requested_cpu != cpu || placement.requested_node != node ||
        placement.cpu_before != cpu || placement.cpu_after != cpu || !std::has_single_bit(placement.page_bytes) ||
        placement.regions.size() != expected_regions)
        return std::unexpected{failure("runtime.package.supplied_placement_invalid",
                                       "Supplied worker clock or placement evidence is incomplete")};
    const auto verify = [&](const EpycBenchPackageLayoutV1& requirements)
        -> std::expected<void, core::Error> {
        for (const auto& region : std::span{requirements.regions}.first(requirements.region_count)) {
            auto found = std::ranges::find(placement.regions, region.name, &RegionPlacement::name);
            const auto count = pages_for(region.bytes, placement.page_bytes);
            const auto policy = region_policy(role, plan.shared_node, region, node);
            if (found != placement.regions.end() && found->targets.empty() && policy.targets.size() == 1U &&
                policy.policy == InputPolicy::bind)
                found->targets = policy.targets;
            if (found != placement.regions.end())
                std::ranges::sort(found->distribution, {}, &NodeResidency::node);
            if (found == placement.regions.end() || found->pages != count || found->policy != policy.policy ||
                found->node != policy.targets.front() || found->targets != policy.targets ||
                !distribution_qualified(*found, false) || !distribution_qualified(*found, true) ||
                (policy.policy == InputPolicy::interleave && region.bytes % placement.page_bytes != 0))
                return std::unexpected{failure("runtime.package.supplied_placement_invalid",
                    "Supplied evidence does not cover every working page on the requested NUMA node",
                    {{"region", region.name}})};
            const auto expected_owner = region.kind == EPYCBENCH_PACKAGE_INPUT && replica_policy(policy.policy)
                ? std::optional{plan.replica_owner} : std::nullopt;
            if (found->owner_rank != expected_owner)
                return std::unexpected{failure("runtime.package.supplied_replica_owner_invalid",
                    "Supplied input placement has a different physical owner", {{"region", region.name}})};
        }
        return {};
    };
    if (auto local = verify(plan.layout); !local) return local;
    if (plan.shared_layout) return verify(*plan.shared_layout);
    return {};
}

struct FixtureClock {
    std::uint64_t begin, end;
    bool started{};

    static auto now(void* context) -> std::expected<std::uint64_t, core::Error> {
        auto& clock = *static_cast<FixtureClock*>(context);
        return std::exchange(clock.started, true) ? clock.end : clock.begin;
    }
};

struct FixturePhaseClock {
    std::array<std::uint64_t, 4> stamps;
    std::size_t next{};

    static auto now(void* context) -> std::expected<std::uint64_t, core::Error> {
        auto& clock = *static_cast<FixturePhaseClock*>(context);
        if (clock.next == clock.stamps.size())
            return std::unexpected{failure("runtime.package.supplied_finalization_clock_invalid",
                                           "Finalizer clock has no remaining timestamp")};
        return clock.stamps[clock.next++];
    }
};

struct FixtureClocks {
    FixtureClock rank;
    std::optional<FixturePhaseClock> finalization;
};

auto fixture_services(void* context, const Role& role, std::uint32_t rank)
    -> std::expected<WorkerServices, core::Error> {
    const auto& fixtures = *static_cast<const std::vector<SuppliedRank>*>(context);
    const auto match = [&](const SuppliedRank& fixture) { return fixture.role == role.id && fixture.rank == rank; };
    const auto found = std::ranges::find_if(fixtures, match);
    if (found == fixtures.end() || std::ranges::count_if(fixtures, match) != 1)
        return std::unexpected{failure("runtime.package.supplied_rank_invalid",
                                       "Expected one supplied clock and placement per rank",
                                       {{"role", role.id}, {"rank", rank}})};
    auto clock = std::make_shared<FixtureClocks>(FixtureClocks{{found->begin_ns, found->end_ns}, {}});
    if (const auto& stamps = found->finalization)
        clock->finalization.emplace(FixturePhaseClock{{stamps->release_ns, stamps->scan_end_ns, stamps->begin_ns,
            stamps->end_ns}});
    const auto collective_clock = clock->finalization ? Clock{&*clock->finalization, FixturePhaseClock::now} : Clock{};
    const auto* final_monitor = found->finalization && !found->finalization->monitoring.is_null()
        ? &found->finalization->monitoring : nullptr;
    return WorkerServices{{}, {&clock->rank, FixtureClock::now}, found->placement, std::move(clock), collective_clock,
        found->warmup, found->monitoring.is_null() ? nullptr : &found->monitoring, final_monitor};
}

auto bind_inputs(const Role& role, const RankTrial* predecessor, RankTrial& result)
    -> std::expected<std::vector<InputBinding>, core::Error> {
    std::vector<InputBinding> bindings;
    if (!predecessor) return bindings;
    for (const auto& link : role.links) {
        if (!predecessor->valid())
            return std::unexpected{failure("runtime.package.stage_predecessor_failed",
                                           "Pipeline predecessor did not produce an eligible output")};
        const auto& trial = *predecessor->trial;
        const auto output = std::ranges::find(trial.outputs, link.output, &ExportedOutput::name);
        const auto evidence = std::ranges::find(trial.regions, link.output, &RegionEvidence::name);
        if (output == trial.outputs.end() || evidence == trial.regions.end() || !evidence->after)
            return std::unexpected{failure("runtime.package.stage_output_missing",
                                           "Pipeline predecessor output or digest is missing")};
        bindings.push_back({link.input, output->bytes});
        result.transfers.push_back({predecessor->role, link.output, link.input, output->bytes.size(), *evidence->after});
    }
    return bindings;
}

struct RankResources {
    std::optional<NativeResource> native;
    WorkerServices services;
};

struct AllocationLedger {
    std::mutex mutex;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;
};

struct TrackedProvider {
    ResourceProvider original;
    AllocationLedger& ledger;

    static auto allocate(void* context, const AllocationRequest& request)
        -> std::expected<Allocation, core::Error> {
        const auto& self = *static_cast<TrackedProvider*>(context);
        auto result = self.original.allocate(self.original.context, request);
        if (!result) return result;
        const auto begin = reinterpret_cast<std::uintptr_t>(result->bytes.data());
        if (begin == 0 || request.bytes > std::numeric_limits<std::uintptr_t>::max() - begin)
            return std::unexpected{failure("runtime.package.worker_allocation_invalid",
                                           "Worker region provider returned an invalid address range")};
        const auto end = begin + request.bytes;
        const std::scoped_lock lock{self.ledger.mutex};
        if (std::ranges::any_of(self.ledger.ranges, [begin, end](const auto& range) {
                return begin < range.second && range.first < end;
            }))
            return std::unexpected{failure("runtime.package.worker_allocation_overlaps",
                                           "Worker region overlaps another rank or team allocation")};
        self.ledger.ranges.emplace_back(begin, end);
        return result;
    }
};

auto prepare_rank(const Role& role, const RankPlan& plan, const RankTrial* predecessor, RankTrial& result,
                  const WorkerProviders& providers, RankResources& resources,
                  std::optional<PreparedCollective>& collective, AllocationLedger& ledger,
                  std::span<const BorrowedInput> borrowed)
    -> std::expected<PreparedTrial, core::Error> {
    if (providers.supplied) {
        auto services = providers.supplied(providers.context, role, plan.request.rank);
        if (!services) return std::unexpected{std::move(services.error())};
        resources.services = std::move(*services);
        result.placement = std::move(resources.services.placement);
        result.placement.verified = false;
        if (auto proved = validate_supplied(result.placement, plan, role, resources.services.clock); !proved)
            return std::unexpected{std::move(proved.error())};
        if (plan.shared_layout && role.module.collective_finalizer() && !resources.services.collective_clock.now)
            return std::unexpected{failure("runtime.package.supplied_finalization_missing",
                                           "Finalizing collective requires supplied phase timestamps")};
    } else {
        if (auto pinned = set_current_thread_affinity(core::CpuSet{result.placement.requested_cpu}); !pinned)
            return std::unexpected{std::move(pinned.error())};
        auto nodes = NativeNumaMask::from_node_set(core::LinuxNumaNodeSet{result.placement.requested_node});
        if (!nodes) return std::unexpected{std::move(nodes.error())};
        std::optional<NativeResource::SharedNode> shared;
        if (plan.shared_node && *plan.shared_node != result.placement.requested_node) {
            auto shared_nodes = NativeNumaMask::from_node_set(core::LinuxNumaNodeSet{*plan.shared_node});
            if (!shared_nodes) return std::unexpected{std::move(shared_nodes.error())};
            shared.emplace(NativeResource::SharedNode{*plan.shared_node, std::move(*shared_nodes)});
        }
        std::optional<NativeNumaMask> input_mask;
        if (!role.input_placement.targets.empty()) {
            core::LinuxNumaNodeSet targets;
            for (const auto node : role.input_placement.targets)
                targets = targets.set_union(core::LinuxNumaNodeSet{node});
            auto native_mask = NativeNumaMask::from_node_set(targets);
            if (!native_mask) return std::unexpected{std::move(native_mask.error())};
            input_mask.emplace(std::move(*native_mask));
        }
        resources.native.emplace(NativeResource{result.placement.page_bytes, result.placement.requested_node,
            std::move(*nodes), std::move(shared), role.input_placement, std::move(input_mask),
            !role.module.shared_cpu_local() && !replica_policy(role.input_placement.policy), {}});
        resources.services.resources = {&*resources.native, NativeResource::allocate};
    }
    auto bindings = bind_inputs(role, predecessor, result);
    if (!bindings) return std::unexpected{std::move(bindings.error())};
    TrackedProvider tracked{resources.services.resources, ledger};
    const auto allocation = tracked.original.allocate ? ResourceProvider{&tracked, TrackedProvider::allocate} : ResourceProvider{};
    auto trial = prepare(role.module, plan.request, allocation, providers.maximum_region_bytes,
                         {*bindings, plan.exports, &plan.layout, borrowed});
    if (!trial) return std::unexpected{std::move(trial.error())};
    if (plan.shared_layout) {
        auto shared = prepare_collective(role.module, plan.request, *plan.shared_layout,
            allocation, providers.maximum_region_bytes);
        if (!shared) return std::unexpected{std::move(shared.error())};
        collective.emplace(std::move(*shared));
    }
    if (resources.native) {
        auto protected_inputs = resources.native->protect_inputs();
        if (!protected_inputs) return std::unexpected{std::move(protected_inputs.error())};
        auto observed = resources.native->observe(result.placement, false);
        if (!observed) return std::unexpected{std::move(observed.error())};
        for (auto& region : result.placement.regions) {
            if (replica_policy(region.policy)) region.owner_rank = plan.replica_owner;
        }
    }
    return trial;
}

auto prove_replicas(std::span<const Role> roles, std::span<const RankPlan> team,
                    std::span<const std::optional<PreparedTrial>> prepared,
                    std::span<Group::ReplicaEvidence> groups) -> bool {
    bool valid = true;
    for (auto& group : groups) {
        std::optional<std::span<const BorrowedInput>> first;
        for (std::size_t i = 0; i < team.size(); ++i) {
            const auto& plan = team[i];
            const auto& role = roles[plan.role_index];
            if (!replica_policy(role.input_placement.policy) || replica_group(role) != group.group ||
                plan.replica_owner != plan.request.rank) continue;
            const auto inputs = prepared[i]->immutable_inputs();
            if (!first) {
                first = inputs;
                continue;
            }
            if (first->size() != inputs.size()) {
                group.error = failure("runtime.package.replica_input_count_mismatch", "Replica input region counts differ");
                break;
            }
            for (const auto& input : inputs) {
                const auto other = std::ranges::find(*first, input.name, &BorrowedInput::name);
                if (other == first->end() || !std::ranges::equal(other->bytes, input.bytes)) {
                    group.error = failure("runtime.package.replica_content_mismatch",
                        "Replica owners do not contain the same complete logical input", {{"region", input.name}});
                    break;
                }
            }
            if (group.error) break;
        }
        if (!first) continue;
        if (group.error) {
            valid = false;
            continue;
        }
        for (const auto& input : *first)
            group.regions.push_back({std::string{input.name}, input.bytes.size(), core::sha256(input.bytes)});
    }
    return valid;
}

void verify_borrowed_placement(RankTrial& rank, const RankTrial& owner, bool supplied) {
    for (const auto& source : owner.placement.regions) {
        if (!replica_policy(source.policy)) continue;
        if (!supplied) {
            rank.placement.regions.push_back(source);
            continue;
        }
        const auto row = std::ranges::find(rank.placement.regions, source.name, &RegionPlacement::name);
        if (row == rank.placement.regions.end() || row->owner_rank != source.owner_rank ||
            row->distribution != source.distribution || row->pages != source.pages || row->targets != source.targets) {
            rank.error = failure("runtime.package.replica_borrowed_placement_mismatch",
                "Supplied borrower placement differs from its physical replica owner", {{"region", source.name}});
        }
    }
    rank.placement.verified = rank.placement.verified && owner.placement.verified && !rank.error;
}

void prove_role_inputs(const Role& role, std::span<const RankPlan> plans,
                       std::span<const std::optional<PreparedTrial>> prepared,
                       const std::optional<PreparedCollective>& collective, RoleInputProof& proof) {
    struct InputHash {
        core::BinaryDigest digest;
        std::uint64_t bytes{};
        bool valid{true};
    };
    std::map<std::string_view, InputHash> inputs;
    const auto append = [&](std::string_view name, std::span<const std::byte> bytes) {
        auto& hash = inputs.try_emplace(name).first->second;
        if (!hash.valid) return;
        if (bytes.size() > core::Sha256Stream::maximum_input_bytes - hash.bytes || !hash.digest.append(bytes)) {
            hash.valid = false;
            proof.error = failure("runtime.package.input_identity_failed", "Cannot hash complete role input", {{"region", name}});
            return;
        }
        hash.bytes += bytes.size();
    };
    std::vector<std::uint32_t> order;
    order.reserve(plans.size());
    for (const auto& plan : plans) order.push_back(plan.request.rank);
    if (!role.module.per_worker_input())
        std::ranges::sort(order, {}, [&](auto rank) { return std::pair{plans[rank].layout.partition_begin, rank}; });
    for (const auto rank : order) {
        if (!prepared[rank]) {
            if (!proof.error) proof.error = failure("runtime.package.input_identity_incomplete", "A role input rank was not prepared");
            continue;
        }
        for (const auto& input : prepared[rank]->immutable_inputs()) append(input.name, input.bytes);
    }
    if (collective) {
        for (const auto& input : collective->call().shared_inputs) {
            const std::string_view name{input.name};
            if (std::ranges::contains(role.module.control_inputs(), name)) continue;
            if (inputs.contains(name)) {
                proof.error = failure("runtime.package.input_identity_conflict",
                    "Rank-local and shared inputs have the same logical region name", {{"region", name}});
                continue;
            }
            append(name, std::as_bytes(std::span{input.data, static_cast<std::size_t>(input.bytes)}));
        }
    } else if (role.module.collective() && !proof.error) {
        proof.error = failure("runtime.package.input_identity_incomplete", "Shared role inputs were not prepared");
    }
    for (auto& [name, hash] : inputs) {
        if (!hash.valid) continue;
        auto digest = hash.digest.finish();
        if (digest) proof.regions.push_back({std::string{name}, hash.bytes, *digest});
        else proof.error = failure("runtime.package.input_identity_failed", "Cannot finalize role input hash", {{"region", name}});
    }
}

auto phase_now(Clock clock) -> std::expected<std::uint64_t, core::Error> {
    if (clock.now) return clock.now(clock.context);
    return raw_monotonic_now().transform([](auto stamp) { return stamp.nanoseconds(); });
}

struct FinalizationTeam {
    explicit FinalizationTeam(std::uint32_t ranks) : scans{ranks}, windows(ranks), receipts(ranks) {}

    std::latch scans;
    std::vector<RankWindow> windows;
    std::vector<EpycBenchPackageRankReceiptV1> receipts;
};

struct SequentialRole {
    SequentialRole(std::uint32_t ranks, SequentialRole* preceding) : complete{ranks}, previous{preceding} {}
    std::latch complete;
    SequentialRole* previous;
};

namespace control = measurement::package_control;

struct MonotonicControlClock {
    Clock source;
    std::uint64_t earliest;
    bool started{};

    static auto now(void* context) -> std::expected<std::uint64_t, core::Error> {
        auto& clock = *static_cast<MonotonicControlClock*>(context);
        auto stamp = phase_now(clock.source);
        if (stamp && !std::exchange(clock.started, true) && *stamp < clock.earliest)
            return std::unexpected{failure("runtime.package.warmup_clock_order",
                "Package clock starts before the preceding warmup phase completed")};
        return stamp;
    }
};

auto missing_warmup_clock(void*) -> std::expected<std::uint64_t, core::Error> {
    return std::unexpected{failure("runtime.package.supplied_warmup_clock_missing",
                                   "The required warmup timestamp pair is missing")};
}

struct WarmupTeam {
    WarmupTeam(std::uint32_t ranks, const control::WarmupSettings& policy, WarmupRole& record)
        : phase{ranks}, settings{policy}, evidence{record}, pending(policy.maximum_trials) {
        for (auto& rows : pending) rows.resize(ranks);
        evidence.attempts.reserve(policy.maximum_trials);
        durations.reserve(policy.maximum_trials);
    }

    std::barrier<> phase;
    const control::WarmupSettings& settings;
    WarmupRole& evidence;
    std::vector<std::vector<WarmupRank>> pending;
    std::vector<std::uint64_t> durations;
    std::uint64_t last_end{};
    std::atomic<bool> fixture_mismatch{false};

    static void callbacks_complete(void* context, const RankWindow&) noexcept {
        static_cast<WarmupTeam*>(context)->phase.arrive_and_wait();
    }

    void add_attempt(std::vector<WarmupRank>& ranks) {
        std::uint64_t earliest = std::numeric_limits<std::uint64_t>::max(), latest{};
        bool valid = true;
        for (const auto& rank : ranks) {
            if (rank.error || !rank.trial || !rank.trial->valid()) {
                valid = false;
                continue;
            }
            earliest = std::min(earliest, *rank.trial->begin_ns);
            latest = std::max(latest, *rank.trial->end_ns);
        }
        if (valid) last_end = latest;
        durations.push_back(valid ? latest - earliest : 0U);
        evidence.attempts.push_back(std::move(ranks));
        const auto decision = control::warmup_decision(settings, durations);
        evidence.status = decision.value_or(control::WarmupStatus::trial_failed);
    }

    auto run(PreparedTrial& prepared, RankTrial& result, const RankResources& resources, bool supplied) -> bool {
        for (std::size_t i = 0; i < pending.size(); ++i) {
            auto& row = pending[i][result.rank];
            row.rank = result.rank;
            Clock clock;
            FixtureClock fixture{};
            if (supplied) {
                if (i < resources.services.warmup_intervals.size()) {
                    const auto pair = resources.services.warmup_intervals[i];
                    fixture = {pair.begin_ns, pair.end_ns};
                    clock = {&fixture, FixtureClock::now};
                } else {
                    clock.now = missing_warmup_clock;
                }
            }
            MonotonicControlClock ordered{clock, last_end};
            prepared.reset_warmup();
            phase.arrive_and_wait();
            try {
                auto trial = prepared.measure_warmup({&ordered, MonotonicControlClock::now},
                    {this, callbacks_complete});
                if (trial) row.trial = std::move(*trial);
                else row.error = std::move(trial.error());
            } catch (const std::exception&) {
                row.error = failure("runtime.package.warmup_execution_failed", "Worker warmup failed");
            }
            phase.arrive_and_wait();
            if (result.rank == 0) add_attempt(pending[i]);
            phase.arrive_and_wait();
            if (evidence.status != control::WarmupStatus::continue_trials) break;
        }

        if (supplied && resources.services.warmup_intervals.size() > evidence.attempts.size())
            fixture_mismatch.store(true, std::memory_order_relaxed);
        phase.arrive_and_wait();
        if (evidence.status == control::WarmupStatus::trial_failed || fixture_mismatch.load(std::memory_order_relaxed)) {
            result.error = fixture_mismatch.load(std::memory_order_relaxed)
                ? failure("runtime.package.supplied_warmup_clock_extra", "Supplied warmup has unused timestamp pairs")
                : failure("runtime.package.warmup_verification_failed", "At least one warmup rank did not verify");
            return false;
        }
        prepared.reset_warmup();
        phase.arrive_and_wait();
        return true;
    }
};

struct ScanRetirement {
    std::latch& timed_work_finished;
    FinalizationTeam* team;
    std::uint32_t rank;
    const PreparedCollective* collective;
    FinalizationEvidence* evidence;
    Clock clock;
    SequentialRole* sequential;
    PackageMonitor* monitor;
    LinuxCpuId cpu;
    const nlohmann::json* supplied_monitoring;

    void finalize() const {
        auto scan_end = phase_now(clock);
        if (!scan_end) {
            evidence->error = std::move(scan_end.error());
            return;
        }
        evidence->scan_end_ns = *scan_end;
        for (std::size_t i = 0; i < team->windows.size(); ++i) {
            const auto& window = team->windows[i];
            team->receipts[i] = window.receipt;
            if (!window.begin_ns || !window.end_ns || *window.begin_ns < *evidence->release_ns ||
                *window.end_ns > *scan_end || *window.end_ns <= *window.begin_ns ||
                window.receipt.executed != 1 || window.receipt.execute_status != 0) {
                evidence->error = failure("runtime.package.collective_scan_invalid",
                                          "Collective finalization requires every completed scan window");
                return;
            }
        }
        PackageMonitorClock monitored{clock, monitor, cpu, supplied_monitoring, evidence->monitoring};
        collective->finalize(team->receipts, monitored.clock(), *evidence);
    }

    static void wait(void* context, const RankWindow& window) noexcept {
        auto& self = *static_cast<ScanRetirement*>(context);
        if (self.team) {
            self.team->windows[self.rank] = window;
            self.team->scans.arrive_and_wait();
            if (self.rank == 0) {
                try {
                    self.finalize();
                } catch (const std::exception&) {
                    self.evidence->error = failure("runtime.package.collective_finalization_failed",
                                                   "Collective finalization failed");
                }
            }
        }
        if (self.sequential) self.sequential->complete.count_down();
        self.timed_work_finished.arrive_and_wait();
    }
};

void verify_collective(const PreparedCollective& collective, std::span<const RankTrial> team,
                       CollectiveEvidence& evidence) {
    std::vector<EpycBenchPackageRankReceiptV1> receipts;
    receipts.reserve(team.size());
    std::uint64_t first = std::numeric_limits<std::uint64_t>::max(), last = 0;
    std::uint64_t overlap_begin = 0, overlap_end = std::numeric_limits<std::uint64_t>::max();
    bool complete_timing = true;
    for (const auto& rank : team) {
        if (!rank.trial) {
            receipts.push_back({rank.rank, 0, -1, {}});
            complete_timing = false;
            continue;
        }
        receipts.push_back({rank.rank, 1, rank.trial->execute_status, rank.trial->actual});
        if (!rank.trial->begin_ns || !rank.trial->end_ns) {
            complete_timing = false;
            continue;
        }
        first = std::min(first, *rank.trial->begin_ns);
        last = std::max(last, *rank.trial->end_ns);
        overlap_begin = std::max(overlap_begin, *rank.trial->begin_ns);
        overlap_end = std::min(overlap_end, *rank.trial->end_ns);
    }
    if (complete_timing) {
        evidence.begin_ns = evidence.finalization ? evidence.finalization->release_ns : std::optional{first};
        evidence.end_ns = evidence.finalization ? evidence.finalization->end_ns : std::optional{last};
        evidence.overlap_begin_ns = overlap_begin;
        evidence.overlap_end_ns = overlap_end;
    }
    collective.verify(receipts, evidence);
}

void execute_team(std::span<const Role> roles, std::span<const RankPlan> team,
                  std::span<RankTrial> results, std::span<const RankTrial> predecessor,
                  const WorkerProviders& providers, std::span<CollectiveEvidence> collective_evidence,
                  std::span<Group::ReplicaEvidence> replica_evidence, std::span<WarmupRole> warmup_evidence,
                  std::span<RoleInputProof> input_proofs,
                  bool serialize_roles = false) {
    const auto count = static_cast<std::ptrdiff_t>(team.size());
    std::latch launch{1}, ready{count}, reference_ready{count}, release{count}, finish{count}, retired{count},
        verified{count}, cleaned{count};
    bool launched = false;
    std::atomic<bool> problems{false};
    AllocationLedger ledger;
    for (const auto& previous : predecessor) {
        if (!previous.trial) continue;
        for (const auto& output : previous.trial->outputs) {
            const auto begin = reinterpret_cast<std::uintptr_t>(output.bytes.data());
            ledger.ranges.emplace_back(begin, begin + output.bytes.size());
        }
    }
    std::vector<std::optional<PreparedTrial>> prepared_ranks(team.size());
    std::vector<std::optional<PreparedCollective>> prepared_collectives(team.size());
    std::vector<std::unique_ptr<FinalizationTeam>> finalizers(team.size());
    std::vector<std::unique_ptr<std::latch>> replica_ready(team.size());
    std::vector<std::unique_ptr<SequentialRole>> sequence(team.size());
    std::vector<std::unique_ptr<WarmupTeam>> warmup_teams(team.size());
    SequentialRole* previous = nullptr;
    for (std::size_t i = 0; i < team.size(); ++i) {
        const auto& plan = team[i];
        if (plan.replica_owner != plan.request.rank) {
            const auto owner = i - plan.request.rank + plan.replica_owner;
            if (!replica_ready[owner]) replica_ready[owner] = std::make_unique<std::latch>(1);
        }
        if (serialize_roles && plan.request.rank == 0) {
            sequence[i] = std::make_unique<SequentialRole>(plan.request.rank_count, previous);
            previous = sequence[i].get();
        }
        if (providers.warmup && plan.request.rank == 0) {
            auto& record = *std::ranges::find(warmup_evidence, roles[plan.role_index].id, &WarmupRole::role);
            warmup_teams[i] = std::make_unique<WarmupTeam>(plan.request.rank_count, *providers.warmup, record);
        }
        if (team[i].shared_layout && roles[team[i].role_index].module.collective_finalizer())
            finalizers[i] = std::make_unique<FinalizationTeam>(team[i].request.rank_count);
    }
    std::vector<std::jthread> threads;
    threads.reserve(team.size());
    try {
        for (std::size_t i = 0; i < team.size(); ++i) {
            threads.emplace_back([&, i] {
                launch.wait();
                if (!launched) return;
                const auto& plan = team[i];
                auto& result = results[i];
                const auto leader = i - plan.request.rank;
                const auto owner = leader + plan.replica_owner;
                auto& prepared = prepared_ranks[i];
                auto& collective = prepared_collectives[leader];
                auto* finalizer = finalizers[leader].get();
                auto* scheduled_role = sequence[leader].get();
                auto* warmup = warmup_teams[leader].get();
                auto* evidence = plan.shared_layout ? &*std::ranges::find(collective_evidence, result.role,
                    &CollectiveEvidence::role) : nullptr;
                RankResources resources;
                try {
                    if (owner != i) replica_ready[owner]->wait();
                    if (owner == i || prepared_ranks[owner]) {
                        const auto borrowed = owner == i ? std::span<const BorrowedInput>{} :
                            prepared_ranks[owner]->immutable_inputs();
                        auto trial = prepare_rank(roles[plan.role_index], plan,
                            predecessor.empty() ? nullptr : &predecessor[i], result, providers, resources, collective, ledger,
                            borrowed);
                        if (trial) prepared.emplace(std::move(*trial));
                        else result.error = std::move(trial.error());
                    } else {
                        result.error = failure("runtime.package.replica_owner_failed", "Physical input replica owner failed preparation");
                    }
                } catch (const std::exception&) {
                    result.error = failure("runtime.package.worker_preparation_failed", "Worker preparation failed");
                }
                if (replica_ready[i]) replica_ready[i]->count_down();
                if (!prepared) problems.store(true, std::memory_order_relaxed);
                ready.arrive_and_wait();
                if (plan.request.rank == 0) {
                    auto& proof = *std::ranges::find(input_proofs, result.role, &RoleInputProof::role);
                    try {
                        prove_role_inputs(roles[plan.role_index], team.subspan(leader, plan.request.rank_count),
                            std::span{prepared_ranks}.subspan(leader, plan.request.rank_count), collective, proof);
                    } catch (const std::exception&) {
                        proof.error = failure("runtime.package.input_identity_failed", "Cannot bind prepared role inputs");
                    }
                    if (proof.error) problems.store(true, std::memory_order_relaxed);
                }
                try {
                    if (i == 0 && !replica_evidence.empty() && !problems.load(std::memory_order_relaxed) &&
                        !prove_replicas(roles, team, prepared_ranks, replica_evidence))
                        problems.store(true, std::memory_order_relaxed);
                    if (evidence && !problems.load(std::memory_order_relaxed)) {
                        std::vector<EpycBenchPackageCollectiveInputV1> inputs;
                        const auto ranks = std::span{prepared_ranks}.subspan(leader, plan.request.rank_count);
                        inputs.reserve(ranks.size());
                        for (const auto& ranked_trial : ranks) inputs.push_back(ranked_trial->collective_input());
                        if (auto reference = collective->reference(inputs); !reference) {
                            evidence->error = std::move(reference.error());
                            problems.store(true, std::memory_order_relaxed);
                        }
                    }
                } catch (const std::exception&) {
                    if (evidence)
                        evidence->error = failure("runtime.package.collective_reference_failed", "Collective reference failed");
                    else result.error = failure("runtime.package.replica_reference_failed", "Replica content proof failed");
                    problems.store(true, std::memory_order_relaxed);
                }
                reference_ready.arrive_and_wait();
                if (evidence && finalizer && !problems.load(std::memory_order_relaxed)) {
                    try {
                        auto stamp = phase_now(resources.services.collective_clock);
                        if (stamp) evidence->finalization->release_ns = *stamp;
                        else evidence->finalization->error = std::move(stamp.error());
                    } catch (const std::exception&) {
                        evidence->finalization->error = failure("runtime.package.collective_release_failed",
                                                               "Collective release clock failed");
                    }
                    if (evidence->finalization->error) problems.store(true, std::memory_order_relaxed);
                }
                release.arrive_and_wait();
                if (problems.load(std::memory_order_relaxed)) {
                    if (evidence && !evidence->error)
                        evidence->error = result.error ? result.error : std::optional{failure(
                            "runtime.package.collective_peer_failed", "A collective peer failed before timing")};
                    if (!result.error)
                        result.error = failure("runtime.package.peer_preparation_failed",
                                               "A concurrent peer failed preparation before timing");
                    if (resources.native) resources.native->retire_inputs(result.placement, result.cleanup_error);
                    return;
                }

                try {
                    if (scheduled_role && scheduled_role->previous) scheduled_role->previous->complete.wait();
                    ScanRetirement timing{finish, finalizer, plan.request.rank, collective ? &*collective : nullptr,
                        evidence && finalizer ? &*evidence->finalization : nullptr, resources.services.collective_clock,
                        scheduled_role, providers.monitor, result.placement.requested_cpu,
                        resources.services.finalization_monitoring};
                    if (!warmup || warmup->run(*prepared, result, resources, providers.supplied != nullptr)) {
                        auto clock = resources.services.clock;
                        MonotonicControlClock ordered{clock, warmup ? warmup->last_end : 0U};
                        if (warmup) clock = {&ordered, MonotonicControlClock::now};
                        PackageMonitorClock monitored{clock, providers.monitor, result.placement.requested_cpu,
                                                      resources.services.monitoring, result.monitoring};
                        auto trial = measure(std::move(*prepared), monitored.clock(), {&timing, ScanRetirement::wait},
                            collective ? collective->call() : CollectiveCall{});
                        if (trial) result.trial = std::move(*trial);
                        else result.error = std::move(trial.error());
                    } else {
                        timing.wait(&timing, {});
                    }
                    if (result.trial) {
                        for (const auto& transfer : result.transfers) {
                            const auto region = std::ranges::find(result.trial->regions, transfer.input, &RegionEvidence::name);
                            if (region == result.trial->regions.end() || region->before != transfer.sha256)
                                result.error = failure("runtime.package.stage_transfer_mismatch",
                                                       "Prepared input differs from the verified stage output");
                        }
                    }
                    if (resources.native) {
                        auto observed = resources.native->observe(result.placement, true);
                        result.placement.verified = observed.has_value();
                        if (!observed && !result.error) result.error = std::move(observed.error());
                    } else {
                        result.placement.verified = true;
                    }
                    if (!result.valid() && result.trial) result.trial->outputs.clear();
                } catch (const std::exception&) {
                    result.error = failure("runtime.package.worker_execution_failed", "Worker execution failed");
                }
                retired.arrive_and_wait();
                if (evidence) {
                    try {
                        verify_collective(*collective, results.subspan(leader, plan.request.rank_count), *evidence);
                    } catch (const std::exception&) {
                        evidence->error = failure("runtime.package.collective_verification_failed",
                                                  "Collective verification failed");
                    }
                }
                verified.arrive_and_wait();
                if (resources.native) resources.native->retire_inputs(result.placement, result.cleanup_error);
                cleaned.arrive_and_wait();
                if (owner != i) verify_borrowed_placement(result, results[owner], providers.supplied != nullptr);
            });
        }
        launched = true;
    } catch (...) {
        launch.count_down();
        for (auto& thread : threads) thread.join();
        for (auto& result : results) {
            result.error = failure("runtime.package.worker_launch_failed", "Cannot create the package worker team");
            if (result.rank == 0) {
                auto& proof = *std::ranges::find(input_proofs, result.role, &RoleInputProof::role);
                proof.error = result.error;
            }
        }
        return;
    }
    launch.count_down();
    for (auto& thread : threads) thread.join();
}
} // namespace

auto RankTrial::valid() const noexcept -> bool {
    return !error && !cleanup_error && trial && trial->valid() && placement.verified;
}

auto Group::valid() const noexcept -> bool {
    if (ranks.empty() || !std::ranges::all_of(ranks, &RankTrial::valid) ||
        !std::ranges::all_of(collectives, &CollectiveEvidence::valid)) return false;
    if (!std::ranges::all_of(replicas, [](const auto& replica) {
            return !replica.error && !replica.owners.empty() && !replica.regions.empty();
        })) return false;
    if (!std::ranges::all_of(warmup, [](const auto& role) {
            return role.status == control::WarmupStatus::stabilized ||
                role.status == control::WarmupStatus::maximum_trials_reached_without_stability;
        })) return false;
    if (!std::ranges::all_of(input_proofs, [](const auto& proof) { return !proof.error; })) return false;
    if (composition == Composition::mixed)
        return overlap_begin_ns && overlap_end_ns && *overlap_begin_ns < *overlap_end_ns;
    if (composition == Composition::pipeline) {
        std::uint64_t predecessor_end = 0;
        for (std::size_t offset = 0; offset < ranks.size();) {
            const auto count = ranks[offset].rank_count;
            if (count == 0 || count > ranks.size() - offset) return false;
            const auto stage = std::span{ranks}.subspan(offset, count);
            std::uint64_t stage_end = 0;
            for (const auto& rank : stage) {
                if (*rank.trial->begin_ns < predecessor_end) return false;
                stage_end = std::max(stage_end, *rank.trial->end_ns);
            }
            const auto team = std::ranges::find(collectives, stage.front().role, &CollectiveEvidence::role);
            if (team != collectives.end()) {
                if (*team->begin_ns < predecessor_end) return false;
                stage_end = *team->end_ns;
            }
            predecessor_end = stage_end;
            offset += count;
        }
    }
    return true;
}

auto supplied_workers(std::vector<SuppliedRank>& fixture) noexcept -> WorkerProviders {
    return {&fixture, fixture_services};
}

namespace {

auto encode_layout(const Role& role, const RankPlan& rank) -> nlohmann::json {
    const auto regions = [](const EpycBenchPackageLayoutV1& layout) {
        auto result = nlohmann::json::array();
        for (const auto& item : std::span{layout.regions}.first(layout.region_count))
            result.push_back({{"name", item.name}, {"kind", item.kind}, {"bytes", item.bytes},
                {"alignment", item.alignment}});
        return result;
    };
    auto row = nlohmann::json{{"role", role.id}, {"rank", rank.request.rank},
        {"partition_begin", rank.layout.partition_begin}, {"partition_count", rank.layout.partition_count},
        {"preparation_bytes", rank.layout.preparation_bytes},
        {"shared_preparation_bytes", rank.shared_layout ? rank.shared_layout->preparation_bytes : 0},
        {"regions", regions(rank.layout)},
        {"shared_regions", rank.shared_layout ? regions(*rank.shared_layout) : nlohmann::json::array()}};
    if (replica_policy(role.input_placement.policy)) row["replica_owner_rank"] = rank.replica_owner;
    return row;
}

auto admits_layouts(const nlohmann::json& geometry, std::span<const Role> roles,
                   std::span<const RankPlan> plans) -> std::expected<void, core::Error> {
    if (!geometry.is_array() || geometry.size() != plans.size())
        return std::unexpected{failure("runtime.package.layout_unadmitted", "The admitted plan has a different rank population")};
    for (const auto& plan : plans) {
        const auto& role = roles[plan.role_index];
        const auto identifies = [&](const nlohmann::json& row) {
            return row.is_object() && row.contains("role") && row.at("role") == role.id &&
                row.contains("rank") && row.at("rank") == plan.request.rank;
        };
        const auto row = std::find_if(geometry.begin(), geometry.end(), identifies);
        if (row == geometry.end())
            return std::unexpected{failure("runtime.package.layout_unadmitted", "No admitted layout for this role and rank",
                {{"role", role.id}, {"rank", plan.request.rank}})};
        const auto expected = encode_layout(role, plan);
        bool matches = row->size() == expected.size() + (row->contains("task_inspection") ? 1U : 0U);
        for (const auto& [key, value] : expected.items())
            matches = matches && row->contains(key) && row->at(key) == value;
        if (!matches)
            return std::unexpected{failure("runtime.package.layout_unadmitted", "The callback layout differs from the admitted geometry",
                {{"role", role.id}, {"rank", plan.request.rank}})};
    }
    return {};
}

} // namespace

auto inspect_layouts(std::span<const Role> roles, Composition composition, std::uint64_t page_bytes)
    -> std::expected<nlohmann::json, core::Error> {
    try {
        if (page_bytes != 0 && (!std::has_single_bit(page_bytes) || page_bytes > std::numeric_limits<int>::max()))
            return std::unexpected{failure("runtime.package.page_size_invalid", "Invalid package layout page size")};
        auto plans = plan_ranks(roles, composition, UINT64_MAX, page_bytes);
        if (!plans) return std::unexpected{std::move(plans.error())};
        auto result = nlohmann::json::array();
        for (const auto& rank : *plans) {
            const auto& role = roles[rank.role_index];
            auto row = encode_layout(role, rank);
            auto inspection = inspect_task(role.module, rank.request, rank.layout);
            if (!inspection) return std::unexpected{std::move(inspection.error())};
            if (*inspection) row["task_inspection"] = std::move(**inspection);
            result.push_back(std::move(row));
        }
        return result;
    } catch (const std::exception&) {
        return std::unexpected{failure("runtime.package.layout_failed", "Cannot inspect package layout")};
    }
}

auto run_workers(std::span<const Role> roles, Composition composition, const WorkerProviders& providers)
    -> std::expected<Group, core::Error> {
    try {
        if (providers.warmup) {
            auto decision = control::warmup_decision(*providers.warmup);
            if (!decision) return std::unexpected{std::move(decision.error())};
            if (composition != Composition::sequential || std::ranges::any_of(roles, [](const auto& role) {
                    return role.module.collective();
                }))
                return std::unexpected{failure("runtime.package.warmup_composition_invalid",
                    "Same-mapping warmup requires sequential independent worker teams")};
        }
        const auto native_page_size = providers.supplied ? 0L : ::sysconf(_SC_PAGESIZE);
        if (!providers.supplied && native_page_size <= 0)
            return std::unexpected{failure("runtime.package.page_size_unavailable", "Cannot determine native page size")};
        const auto page_bytes = static_cast<std::uint64_t>(native_page_size);
        auto admitted = providers;
        MemoryDemand demand;
        if (admitted.supplied) {
            if (admitted.maximum_region_bytes == 0) admitted.maximum_region_bytes = default_region_budget;
        } else {
            auto limit = native_memory_limit();
            if (!limit) return std::unexpected{std::move(limit.error())};
            admitted.maximum_region_bytes = admitted.maximum_region_bytes == 0 ? *limit :
                std::min(*limit, admitted.maximum_region_bytes);
        }
        const auto plans = plan_ranks(roles, composition, admitted.maximum_region_bytes, page_bytes,
            admitted.supplied ? nullptr : &demand);
        if (!plans) return std::unexpected{plans.error()};
        if (admitted.admitted_geometry) {
            auto confirmed = admits_layouts(*admitted.admitted_geometry, roles, *plans);
            if (!confirmed) return std::unexpected{std::move(confirmed.error())};
        }
        if (!admitted.supplied) {
            if (auto headroom = check_headroom(demand); !headroom)
                return std::unexpected{std::move(headroom.error())};
            admitted.maximum_region_bytes = demand.peak;
        }
        Group group{.supplied = providers.supplied != nullptr, .composition = composition,
                    .overlap_begin_ns = {}, .overlap_end_ns = {}, .ranks = {}, .collectives = {}};
        group.input_proofs.reserve(roles.size());
        for (const auto& role : roles) group.input_proofs.push_back({role.id});
        if (admitted.warmup) {
            group.warmup.reserve(roles.size());
            for (const auto& role : roles) group.warmup.push_back({role.id});
        }
        group.ranks.reserve(plans->size());
        for (const auto& plan : *plans) {
            const auto& role = roles[plan.role_index];
            if (replica_policy(role.input_placement.policy)) {
                const auto name = replica_group(role);
                const auto found = std::ranges::find(group.replicas, name, &Group::ReplicaEvidence::group);
                auto& replica = found == group.replicas.end() ? group.replicas.emplace_back(Group::ReplicaEvidence{
                    std::string{name}, role.input_placement.policy, {}, {}, {}}) : *found;
                if (plan.replica_owner == plan.request.rank)
                    replica.owners.push_back({role.id, plan.request.rank, memory_node(role, plan.request.rank)});
            }
            if (plan.shared_layout) {
                CollectiveEvidence collective;
                collective.role = role.id;
                if (role.module.collective_finalizer()) collective.finalization.emplace();
                group.collectives.push_back(std::move(collective));
            }
            group.ranks.push_back({.role = role.id, .rank = plan.request.rank,
                .rank_count = plan.request.rank_count, .trial = {},
                .placement = {.requested_cpu = role.cpus[plan.request.rank],
                              .requested_node = memory_node(role, plan.request.rank),
                              .cpu_before = {}, .cpu_after = {}, .page_bytes = page_bytes, .regions = {}, .verified = false},
                .transfers = {}, .error = {}});
        }
        auto results = std::span{group.ranks};
        const auto all_plans = std::span{*plans};
        if (composition == Composition::mixed) {
            execute_team(roles, all_plans, results, {}, admitted, group.collectives, group.replicas, group.warmup, group.input_proofs);
            std::uint64_t latest_start = 0;
            auto earliest_end = std::numeric_limits<std::uint64_t>::max();
            bool complete = true;
            for (const auto& rank : results) {
                if (!rank.trial || !rank.trial->begin_ns || !rank.trial->end_ns) {
                    complete = false;
                    break;
                }
                latest_start = std::max(latest_start, *rank.trial->begin_ns);
                earliest_end = std::min(earliest_end, *rank.trial->end_ns);
            }
            if (complete) {
                group.overlap_begin_ns = latest_start;
                group.overlap_end_ns = earliest_end;
            }
        } else if (composition == Composition::sequential && cross_role_replicas(roles)) {
            execute_team(roles, all_plans, results, {}, admitted, group.collectives, group.replicas, group.warmup, group.input_proofs, true);
        } else {
            std::span<RankTrial> predecessor;
            bool prior_collective_failed = false;
            for (std::size_t offset = 0; offset < plans->size();) {
                const auto count = all_plans[offset].request.rank_count;
                const auto stage = results.subspan(offset, count);
                if (prior_collective_failed) {
                    for (auto& rank : stage)
                        rank.error = failure("runtime.package.stage_collective_predecessor_failed",
                                             "Pipeline predecessor did not verify its collective output");
                    auto& proof = *std::ranges::find(group.input_proofs, stage.front().role, &RoleInputProof::role);
                    proof.error = stage.front().error;
                } else {
                    execute_team(roles, all_plans.subspan(offset, count), stage, predecessor, admitted, group.collectives,
                        group.replicas, group.warmup, group.input_proofs);
                }
                for (auto& prior : predecessor) {
                    if (prior.trial) prior.trial->outputs.clear();
                }
                if (composition == Composition::pipeline) {
                    predecessor = stage;
                    const auto team = std::ranges::find(group.collectives, stage.front().role, &CollectiveEvidence::role);
                    prior_collective_failed = prior_collective_failed ||
                        (team != group.collectives.end() && !team->valid());
                }
                offset += count;
            }
        }
        std::erase_if(group.warmup, [](const auto& role) { return role.attempts.empty(); });
        return group;
    } catch (const std::exception&) {
        return std::unexpected{failure("runtime.package.group_failed", "Cannot plan or execute package workers")};
    }
}
#include "platform_linux/package_stream_workers.inc"
} // namespace epycbench::platform_linux::package_execution
