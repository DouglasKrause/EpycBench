#include "epycbench/memory/region_binding.hpp"

#include "memory/region_binding_access.hpp"
#include "memory/region_mapping_access.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::memory {
namespace {

[[nodiscard]] auto binding_error(std::string_view code, std::string_view classification,
                                 const core::Error* underlying = nullptr) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    std::optional<core::OsError> os_error;
    if (underlying != nullptr) {
        context["underlying_code"] = underlying->code.value();
        os_error = underlying->os_error;
    }
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Memory region binding failed",
            .operation = "memory.region_binding.create",
            .os_error = std::move(os_error),
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto production_create_mask(void*, const core::LinuxNumaNodeSet& nodes)
    -> std::expected<platform_linux::NativeNumaMask, core::Error> {
    return platform_linux::NativeNumaMask::from_node_set(nodes);
}

[[nodiscard]] auto production_bind(void*, const platform_linux::AnonymousMapping& mapping,
                                   const platform_linux::NativeNumaMask& mask)
    -> std::expected<void, core::Error> {
    return platform_linux::bind_memory(mapping, mask);
}

[[nodiscard]] auto production_operations() noexcept -> detail::RegionBindingOperations {
    return {.context = nullptr, .create_mask = production_create_mask, .bind = production_bind};
}

[[nodiscard]] auto node_set(const RegionRequest& request) -> core::LinuxNumaNodeSet {
    core::LinuxNumaNodeSet nodes;
    for (const auto& target : request.numa_targets()) {
        nodes = nodes.set_union(core::LinuxNumaNodeSet{target.linux_node});
    }
    return nodes;
}
auto diagnosed_code(platform_linux::detail::NumaBindEinvalDiagnosis diagnosis)
    -> std::pair<std::string_view, std::string_view> {
    using Diagnosis = platform_linux::detail::NumaBindEinvalDiagnosis;
    switch (diagnosis) {
    case Diagnosis::selected_nodes_outside_allowed_mems:
        return {"memory.region_binding.bind_einval_selected_nodes_outside_allowed_mems",
                "selected_nodes_outside_allowed_mems"};
    case Diagnosis::allowed_mems_observation_failed:
        return {"memory.region_binding.bind_einval_allowed_mems_observation_failed",
                "allowed_mems_observation_failed"};
#define EPYCBENCH_DIAGNOSIS_CASE(name)                                                       \
    case Diagnosis::name:                                                                    \
        return {"memory.region_binding.bind_einval_" #name, #name}
    EPYCBENCH_DIAGNOSIS_CASE(mapping_not_base_page_aligned);
    EPYCBENCH_DIAGNOSIS_CASE(native_range_wraps);
    EPYCBENCH_DIAGNOSIS_CASE(native_range_not_base_page_multiple);
    EPYCBENCH_DIAGNOSIS_CASE(bind_mode_not_static_absolute);
    EPYCBENCH_DIAGNOSIS_CASE(bind_flags_not_zero);
    EPYCBENCH_DIAGNOSIS_CASE(selected_mask_empty);
    EPYCBENCH_DIAGNOSIS_CASE(selected_maxnode_outside_possible_domain);
    EPYCBENCH_DIAGNOSIS_CASE(selected_nodes_not_effective);
    EPYCBENCH_DIAGNOSIS_CASE(selected_nodes_not_online);
    EPYCBENCH_DIAGNOSIS_CASE(selected_nodes_not_memory_backed);
    EPYCBENCH_DIAGNOSIS_CASE(well_formed_request_rejected);
#undef EPYCBENCH_DIAGNOSIS_CASE
    case Diagnosis::count:
        break;
    }
    std::terminate();
}

} // namespace

auto BoundRegion::create(RegionMapping mapping) noexcept
    -> std::expected<BoundRegion, RegionBindingFailure> {
    return detail::BoundRegionAccess::create_with_operations(std::move(mapping),
                                                             production_operations());
}

auto detail::BoundRegionAccess::create_with_operations(
    RegionMapping mapping, const RegionBindingOperations& operations) noexcept
    -> std::expected<BoundRegion, RegionBindingFailure> {
    const auto fail = [&mapping](core::Error error)
        -> std::expected<BoundRegion, RegionBindingFailure> {
        return std::unexpected{failure(std::move(error), std::move(mapping))};
    };

    try {
        const auto& request = mapping.request();
        if (request.placement_policy() != PlacementPolicy::bind) {
            return fail(binding_error("memory.region_binding.invalid_argument",
                                      "unsupported_placement_policy"));
        }
        if (request.numa_targets().empty()) {
            return fail(binding_error("memory.region_binding.invalid_argument",
                                      "missing_numa_target"));
        }

        auto& owner = RegionMappingAccess::mapping(mapping);
        if (!owner.active()) {
            return fail(binding_error("memory.region_binding.invariant_failed",
                                      "mapping_inactive"));
        }
        if (owner.size() != mapping.mapping_bytes() ||
            std::as_const(owner).bytes().size() != mapping.mapping_bytes() ||
            std::as_const(mapping).full_mapping().size() != mapping.mapping_bytes()) {
            return fail(binding_error("memory.region_binding.invariant_failed",
                                      "mapping_range_invalid"));
        }
        if (mapping.mapping_bytes() == 0U) {
            return fail(binding_error("memory.region_binding.invariant_failed",
                                      "mapping_range_invalid"));
        }
        if (owner.protection() != platform_linux::MemoryProtection::read_write ||
            owner.writable_bytes().size() != mapping.mapping_bytes() ||
            mapping.full_mapping().size() != mapping.mapping_bytes()) {
            return fail(binding_error("memory.region_binding.invariant_failed",
                                      "mapping_not_writable"));
        }

        const auto nodes = node_set(request);
        const auto prepared_evidence = evidence();
        if (operations.create_mask == nullptr ||
            ((operations.bind == nullptr) == (operations.bind_diagnosed == nullptr))) {
            return fail(binding_error("memory.region_binding.invariant_failed",
                                      "binding_owner_invalid"));
        }

        auto mask = operations.create_mask(operations.context, nodes);
        if (!mask) {
            return fail(binding_error("memory.region_binding.mask_creation_failed",
                                      "native_mask_creation_failed", &mask.error()));
        }

        if (operations.bind_diagnosed != nullptr) {
            if (operations.domain == nullptr || operations.workspace == nullptr ||
                operations.invocation_facts == nullptr) {
                return fail(binding_error("memory.region_binding.invariant_failed",
                                          "binding_owner_invalid"));
            }
            auto bound = operations.bind_diagnosed(operations.context, owner, *mask,
                                                    *operations.domain,
                                                    *operations.invocation_facts,
                                                    *operations.workspace);
            if (!bound) {
                if (operations.before_diagnosed_projection != nullptr) {
                    operations.before_diagnosed_projection(operations.context);
                }
                if (bound.error().diagnosis) {
                    const auto [code, classification] =
                        diagnosed_code(*bound.error().diagnosis);
                    return fail(binding_error(code, classification, &bound.error().primary));
                }
                return fail(binding_error("memory.region_binding.bind_failed", "binding_failed",
                                          &bound.error().primary));
            }
        } else {
            if (operations.domain != nullptr || operations.workspace != nullptr ||
                operations.invocation_facts != nullptr ||
                operations.before_diagnosed_projection != nullptr) {
                return fail(binding_error("memory.region_binding.invariant_failed",
                                          "binding_owner_invalid"));
            }
            auto bound = operations.bind(operations.context, owner, *mask);
            if (!bound) {
                return fail(binding_error("memory.region_binding.bind_failed", "binding_failed",
                                          &bound.error()));
            }
        }

        return success(std::move(mapping), prepared_evidence);
    } catch (const std::exception&) {
        return fail(binding_error("memory.region_binding.contained_exception",
                                  "contained_exception"));
    } catch (...) {
        return fail(binding_error("memory.region_binding.contained_unknown_failure",
                                  "contained_unknown_failure"));
    }
}

} // namespace epycbench::memory
