#include "runtime/pairwise_memory_owning_facade.hpp"

#include "memory/pairwise_memory_access.hpp"
#include "memory/retained_region_cleanup_access.hpp"
#include "memory/region_finalization_access.hpp"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <limits>
#include <numeric>
#include <ranges>
#include <type_traits>

namespace epycbench::runtime::detail {

auto project_pairwise_memory_finalization_failure(
    PairwiseMemoryFinalizationFailure failure) noexcept
    -> PairwiseProjectedMemoryFinalizationFailure {
    auto diagnostic = memory::detail::RegionFinalizationAccess::
        take_failure_diagnostic(std::move(failure.owner),
                                std::move(failure.post_run),
                                failure.cleanup_evidence, failure.path);
    return {.diagnostic=std::move(diagnostic),
            .primary=std::move(failure.primary),
            .cleanup=std::move(failure.cleanup)};
}
namespace {

[[nodiscard]] auto memory_failure(
    PairwiseFailureCode code, PairwiseFailureOperation operation,
    PairwiseFailureReason reason, PairwiseFailureContext context,
    std::optional<int> error_number = std::nullopt) noexcept -> PairwiseMemoryFailure {
    auto result = PairwiseMemoryFailure::create(code, operation, reason,
                                                std::move(context), error_number);
    if (!result)
        std::terminate();
    return std::move(*result);
}

[[nodiscard]] auto runtime_preparation_failure() noexcept -> PairwiseRuntimeFailure {
    auto result = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations,
        PairwiseFailureReason::contained_exception,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!result)
        std::terminate();
    return std::move(*result);
}

template <class Failure>
[[nodiscard]] auto nested_value(const Failure& failure) noexcept
    -> PairwiseNestedFailureValue {
    return std::visit(
        [&failure]<class Context>(const Context& context)
            -> PairwiseNestedFailureValue {
            if constexpr (std::is_same_v<Context, NestedFailureContext>)
                std::terminate();
            else
                return {failure.code(), failure.operation(), failure.reason(),
                        PairwiseFlatFailureContext{context},
                        failure.posix_errno()};
        },
        failure.context());
}

[[nodiscard]] auto nested_platform(const PairwisePlatformFailure& failure) noexcept
    -> PairwiseFailureContext {
    return NestedFailureContext{nested_value(failure), std::nullopt, std::nullopt};
}

[[nodiscard]] constexpr auto mapping_context() noexcept -> PairwiseFailureContext {
    return RequestedProtectionAdviceContext{
        PairwiseMappingProtection::unspecified,
        PairwiseMappingAdvice::no_huge_pages};
}

void before(const PairwiseMemoryOwningHooks& hooks,
            PairwiseMemoryOwningOperation operation) {
    if (hooks.before_operation != nullptr)
        hooks.before_operation(hooks.context, operation);
}

template <class Value>
[[nodiscard]] auto request_failed(PairwiseFailureCode code,
                                  PairwiseFailureReason reason) noexcept
    -> std::expected<Value, PairwiseMemoryFailure> {
    return std::unexpected{memory_failure(
        code, PairwiseFailureOperation::memory_region_request_create, reason,
        NoPairwiseFailureContext{})};
}

[[nodiscard]] auto duplicate_target(
    std::span<const memory::RegionNumaTarget> targets,
    auto projection) noexcept -> bool {
    for (std::size_t left = 0; left < targets.size(); ++left)
        for (std::size_t right = left + 1U; right < targets.size(); ++right)
            if (projection(targets[left]) == projection(targets[right]))
                return true;
    return false;
}

[[nodiscard]] auto binding_diagnosis(
    platform_linux::detail::NumaBindEinvalDiagnosis diagnosis) noexcept
    -> std::pair<PairwiseFailureCode, PairwiseFailureReason> {
    using Diagnosis = platform_linux::detail::NumaBindEinvalDiagnosis;
    switch (diagnosis) {
    case Diagnosis::selected_nodes_outside_allowed_mems:
        return {PairwiseFailureCode::memory_binding_bind_einval_selected_nodes_outside_allowed_mems,
                PairwiseFailureReason::selected_nodes_outside_allowed_mems};
    case Diagnosis::allowed_mems_observation_failed:
        return {PairwiseFailureCode::memory_binding_bind_einval_allowed_mems_observation_failed,
                PairwiseFailureReason::allowed_mems_observation_failed};
#define EPYCBENCH_MEMORY_DIAGNOSIS(name)                                                \
    case Diagnosis::name:                                                               \
        return {PairwiseFailureCode::memory_binding_bind_einval_##name,                 \
                PairwiseFailureReason::name}
        EPYCBENCH_MEMORY_DIAGNOSIS(mapping_not_base_page_aligned);
        EPYCBENCH_MEMORY_DIAGNOSIS(native_range_wraps);
        EPYCBENCH_MEMORY_DIAGNOSIS(native_range_not_base_page_multiple);
        EPYCBENCH_MEMORY_DIAGNOSIS(bind_mode_not_static_absolute);
        EPYCBENCH_MEMORY_DIAGNOSIS(bind_flags_not_zero);
        EPYCBENCH_MEMORY_DIAGNOSIS(selected_mask_empty);
        EPYCBENCH_MEMORY_DIAGNOSIS(selected_maxnode_outside_possible_domain);
        EPYCBENCH_MEMORY_DIAGNOSIS(selected_nodes_not_effective);
        EPYCBENCH_MEMORY_DIAGNOSIS(selected_nodes_not_online);
        EPYCBENCH_MEMORY_DIAGNOSIS(selected_nodes_not_memory_backed);
        EPYCBENCH_MEMORY_DIAGNOSIS(well_formed_request_rejected);
#undef EPYCBENCH_MEMORY_DIAGNOSIS
    case Diagnosis::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] auto region_mapping(memory::BoundRegion& region) noexcept
    -> memory::RegionMapping& {
    return memory::detail::BoundRegionAccess::mapping(region);
}

[[nodiscard]] auto region_mapping(memory::InitializedRegion& region) noexcept
    -> memory::RegionMapping& {
    return region_mapping(memory::detail::InitializedRegionAccess::region(region));
}

[[nodiscard]] auto region_mapping(memory::PlacementObservedRegion& region) noexcept
    -> memory::RegionMapping& {
    return region_mapping(memory::detail::PlacementObservedRegionAccess::region(region));
}

[[nodiscard]] auto region_mapping(memory::PlacementEvaluatedRegion& region) noexcept
    -> memory::RegionMapping& {
    return region_mapping(memory::detail::PlacementEvaluatedRegionAccess::region(region));
}

[[nodiscard]] constexpr auto qualification(
    memory::PlacementQualificationState state) noexcept -> PairwiseQualificationState {
    switch (state) {
    case memory::PlacementQualificationState::qualified:
        return PairwiseQualificationState::qualified;
    case memory::PlacementQualificationState::skipped:
        return PairwiseQualificationState::skipped;
    case memory::PlacementQualificationState::not_qualified:
        return PairwiseQualificationState::not_qualified;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto protection_context(
    memory::PlacementQualificationState state,
    PairwiseFailureReason reason) noexcept -> PairwiseFailureContext {
    return MemoryProtectionStateContext{
        PairwiseProtectionKind::read_only, PairwiseRangeKind::full_mapping,
        qualification(state), reason};
}

} // namespace

auto PairwiseMemoryOwningFacade::query_observation(
    const memory::InitializedRegion& initialized,
    PairwiseMemoryWorkspace& workspace, bool post_run) const noexcept
    -> std::expected<memory::RegionPlacementObservation, PairwiseMemoryFailure> {
    const auto failed = [](PairwiseFailureCode code, PairwiseFailureReason reason,
                           PairwiseFailureContext context = NoPairwiseFailureContext{},
                           std::optional<int> error_number = std::nullopt) {
        return std::unexpected{memory_failure(
            code, PairwiseFailureOperation::memory_region_placement_observe,
            reason, std::move(context), error_number)};
    };
    try {
        before(hooks_, PairwiseMemoryOwningOperation::placement_query);
        const auto& bound = memory::detail::InitializedRegionAccess::region(initialized);
        const auto& mapping = memory::detail::BoundRegionAccess::mapping(bound);
        const auto& owner = memory::detail::RegionMappingAccess::mapping(mapping);
        if (!owner.active())
            return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                          PairwiseFailureReason::mapping_inactive);
        if (owner.size() != mapping.mapping_bytes() ||
            owner.bytes().size() != mapping.mapping_bytes() ||
            mapping.full_mapping().size() != mapping.mapping_bytes())
            return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                          PairwiseFailureReason::placement_owner_invalid);
        if (owner.protection() != platform_linux::MemoryProtection::read_write &&
            owner.protection() != platform_linux::MemoryProtection::read_only)
            return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                          PairwiseFailureReason::placement_owner_invalid);

        const auto& request = mapping.request();
        const auto requested = request.requested_bytes().value();
        const auto page = request.page_size().value();
        if (requested == 0U)
            return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                          PairwiseFailureReason::requested_range_invalid);
        if (page == 0U)
            return failed(PairwiseFailureCode::memory_placement_query_invalid_argument,
                          PairwiseFailureReason::page_size_invalid);
        if (requested % page != 0U)
            return failed(PairwiseFailureCode::memory_placement_query_invalid_argument,
                          PairwiseFailureReason::page_alignment_invalid);
        if (requested > std::numeric_limits<std::size_t>::max() ||
            page > std::numeric_limits<std::size_t>::max())
            return failed(PairwiseFailureCode::memory_placement_query_invalid_argument,
                          PairwiseFailureReason::page_address_unrepresentable);
        const auto requested_region = mapping.requested_region();
        if (requested_region.empty() ||
            requested_region.size() != static_cast<std::size_t>(requested))
            return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                          PairwiseFailureReason::requested_range_invalid);
        auto total_pages = requested / page;
        if (total_pages == 0U || total_pages != workspace.total_pages_)
            return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                          PairwiseFailureReason::page_count_overflow);

        auto& available = post_run ? workspace.post_available_ : workspace.pre_available_;
        auto& node_counts = post_run ? workspace.post_counts_ : workspace.pre_counts_;
        if (!available)
            return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                          PairwiseFailureReason::query_input_invalid);
        available = false;
        node_counts.clear();
        std::uint64_t resident_pages{};
        std::uint64_t unresolved_pages{};
        std::uint64_t processed_pages{};
        std::size_t offset{};
        const auto page_size = static_cast<std::size_t>(page);
        while (processed_pages < total_pages) {
            const auto remaining = total_pages - processed_pages;
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                remaining, workspace.page_addresses_.size()));
            if (count == 0U || count > workspace.query_buffers_.capacity())
                return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                              PairwiseFailureReason::observation_inconsistent);
            for (std::size_t index = 0; index < count; ++index) {
                if (offset >= requested_region.size() ||
                    page_size > requested_region.size() - offset)
                    return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                                  PairwiseFailureReason::page_address_overflow);
                workspace.page_addresses_[index] =
                    const_cast<std::byte*>(requested_region.data() + offset);
                offset += page_size;
            }
            auto statuses = platform_.query_pages_prepared(
                std::span<void* const>{workspace.page_addresses_.data(), count},
                workspace.query_buffers_);
            if (!statuses)
                return failed(PairwiseFailureCode::memory_placement_query_failed,
                              PairwiseFailureReason::query_failed,
                              nested_platform(statuses.error()));
            if (statuses->size() != count)
                return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                              PairwiseFailureReason::returned_count_mismatch);
            for (const auto& status : *statuses) {
                switch (status.kind()) {
                case platform_linux::PageQueryStatusKind::resident: {
                    const auto node = status.node();
                    if (!node || status.error_number() ||
                        resident_pages == std::numeric_limits<std::uint64_t>::max())
                        return failed(
                            PairwiseFailureCode::memory_placement_query_invariant_failed,
                            PairwiseFailureReason::page_status_invalid);
                    ++resident_pages;
                    const auto position = std::lower_bound(
                        node_counts.begin(), node_counts.end(), *node,
                        [](const memory::LinuxNodePageCount& entry,
                           core::LinuxNumaNodeId value) {
                            return entry.linux_node() < value;
                        });
                    if (position != node_counts.end() &&
                        position->linux_node() == *node) {
                        if (position->page_count() ==
                            std::numeric_limits<std::uint64_t>::max())
                            return failed(
                                PairwiseFailureCode::memory_placement_query_invariant_failed,
                                PairwiseFailureReason::observation_inconsistent);
                        *position = memory::detail::PairwiseMemoryAccess::node_count(
                            *node, position->page_count() + 1U);
                    } else {
                        if (hooks_.before_node_count_insert != nullptr)
                            hooks_.before_node_count_insert(hooks_.context, post_run);
                        if (node_counts.size() == workspace.node_capacity_)
                            std::terminate();
                        node_counts.insert(
                            position,
                            memory::detail::PairwiseMemoryAccess::node_count(*node, 1U));
                    }
                    break;
                }
                case platform_linux::PageQueryStatusKind::unresolved:
                    if (status.node() || status.error_number() != ENOENT ||
                        unresolved_pages == std::numeric_limits<std::uint64_t>::max())
                        return failed(
                            PairwiseFailureCode::memory_placement_query_invariant_failed,
                            PairwiseFailureReason::page_status_invalid);
                    ++unresolved_pages;
                    break;
                case platform_linux::PageQueryStatusKind::error: {
                    const auto number = status.error_number();
                    if (status.node() || !number || *number <= 0 || *number == ENOENT)
                        return failed(
                            PairwiseFailureCode::memory_placement_query_invariant_failed,
                            PairwiseFailureReason::page_status_invalid);
                    return failed(PairwiseFailureCode::memory_placement_query_failed,
                                  PairwiseFailureReason::page_status_error,
                                  NoPairwiseFailureContext{}, number);
                }
                }
            }
            processed_pages += count;
        }
        if (processed_pages != total_pages || offset != requested_region.size() ||
            resident_pages > total_pages ||
            unresolved_pages != total_pages - resident_pages)
            return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                          PairwiseFailureReason::observation_inconsistent);
        std::uint64_t aggregate{};
        for (const auto& entry : node_counts) {
            if (entry.page_count() == 0U ||
                entry.page_count() > std::numeric_limits<std::uint64_t>::max() - aggregate)
                return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                              PairwiseFailureReason::observation_inconsistent);
            aggregate += entry.page_count();
        }
        if (aggregate != resident_pages)
            return failed(PairwiseFailureCode::memory_placement_query_invariant_failed,
                          PairwiseFailureReason::observation_inconsistent);
        if (hooks_.adjust_observation != nullptr)
            hooks_.adjust_observation(hooks_.context, post_run, total_pages,
                                      resident_pages, unresolved_pages);
        return memory::detail::PairwiseMemoryAccess::observation(
            total_pages, resident_pages, unresolved_pages, std::move(node_counts));
    } catch (const std::bad_alloc&) {
        return failed(PairwiseFailureCode::memory_placement_query_contained_exception,
                      PairwiseFailureReason::allocation_failure);
    } catch (const std::exception&) {
        return failed(PairwiseFailureCode::memory_placement_query_contained_exception,
                      PairwiseFailureReason::contained_exception);
    } catch (...) {
        return failed(
            PairwiseFailureCode::memory_placement_query_contained_unknown_failure,
            PairwiseFailureReason::contained_unknown_failure);
    }
}

auto PairwiseMemoryFailureSequence::append(PairwiseEvidenceFailure failure) noexcept
    -> bool {
    if (size_ == values_.size())
        return false;
    values_[size_++].emplace(std::move(failure));
    return true;
}

auto PairwiseMemoryFailureSequence::operator[](std::size_t index) const noexcept
    -> const PairwiseEvidenceFailure& {
    if (index >= size_ || !values_[index])
        std::terminate();
    return *values_[index];
}

auto PairwiseMemoryOwningFacade::exact_ratio(
    std::uint64_t numerator, std::uint64_t denominator) const noexcept
    -> std::expected<memory::ExactRatio, PairwiseMemoryFailure> {
    try {
        before(hooks_, PairwiseMemoryOwningOperation::exact_ratio);
        if (denominator == 0U || numerator > denominator)
            return request_failed<memory::ExactRatio>(
                PairwiseFailureCode::memory_request_invalid_argument,
                PairwiseFailureReason::invalid_threshold);
        const auto divisor = std::gcd(numerator, denominator);
        return memory::detail::PairwiseMemoryAccess::ratio(
            numerator / divisor, denominator / divisor);
    } catch (const std::exception&) {
        return request_failed<memory::ExactRatio>(
            PairwiseFailureCode::memory_request_contained_exception,
            PairwiseFailureReason::contained_exception);
    } catch (...) {
        return request_failed<memory::ExactRatio>(
            PairwiseFailureCode::memory_request_contained_unknown_failure,
            PairwiseFailureReason::contained_unknown_failure);
    }
}

auto PairwiseMemoryOwningFacade::request(memory::RegionRequestInput input) const noexcept
    -> std::expected<memory::RegionRequest, PairwiseMemoryFailure> {
    try {
        before(hooks_, PairwiseMemoryOwningOperation::request);
        if (input.placement_policy != memory::PlacementPolicy::bind)
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invalid_argument,
                PairwiseFailureReason::unsupported_placement_policy);
        if (input.page_mode != memory::PageMode::base)
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invalid_argument,
                PairwiseFailureReason::unsupported_page_mode);
        if (input.requested_bytes.value() == 0U)
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invalid_argument,
                PairwiseFailureReason::zero_requested_bytes);
        if (input.page_size.value() == 0U)
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invalid_argument,
                PairwiseFailureReason::zero_page_size);
        if (input.requested_bytes.value() % input.page_size.value() != 0U)
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invalid_argument,
                PairwiseFailureReason::page_alignment_mismatch);
        if (input.numa_targets.empty())
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invalid_argument,
                PairwiseFailureReason::missing_numa_target);
        const std::span<const memory::RegionNumaTarget> targets{input.numa_targets};
        if (duplicate_target(targets, [](const auto& value) -> const auto& {
                return value.stable_node;
            }))
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invalid_argument,
                PairwiseFailureReason::duplicate_stable_numa_target);
        if (duplicate_target(targets, [](const auto& value) { return value.linux_node; }))
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invalid_argument,
                PairwiseFailureReason::duplicate_linux_numa_target);
        if (duplicate_target(targets, [](const auto& value) { return value.topology_node; }))
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invariant_failed,
                PairwiseFailureReason::duplicate_topology_numa_target);
        if (input.placement_threshold.denominator() == 0U ||
            input.placement_threshold.numerator() >
                input.placement_threshold.denominator())
            return request_failed<memory::RegionRequest>(
                PairwiseFailureCode::memory_request_invariant_failed,
                PairwiseFailureReason::invalid_threshold);
        return memory::detail::PairwiseMemoryAccess::request(std::move(input));
    } catch (const std::exception&) {
        return request_failed<memory::RegionRequest>(
            PairwiseFailureCode::memory_request_contained_exception,
            PairwiseFailureReason::contained_exception);
    } catch (...) {
        return request_failed<memory::RegionRequest>(
            PairwiseFailureCode::memory_request_contained_unknown_failure,
            PairwiseFailureReason::contained_unknown_failure);
    }
}

auto PairwiseMemoryOwningFacade::prepare_mask(
    const memory::RegionRequest& request,
    platform_linux::detail::NumaDomainMaxNode domain,
    platform_linux::detail::NumaBindInvocationFacts facts) const noexcept
    -> std::expected<PairwisePreparedMemoryMask, PairwiseMemoryFailure> {
    try {
        before(hooks_, PairwiseMemoryOwningOperation::binding);
        const auto targets = request.numa_targets();
        auto mask = platform_.prepare_numa_mask({
            .context = &targets,
            .count = targets.size(),
            .at = [](const void* context, std::size_t index) noexcept {
                const auto& values =
                    *static_cast<const std::span<const memory::RegionNumaTarget>*>(
                        context);
                return values[index].linux_node;
            }});
        if (!mask)
            return std::unexpected{memory_failure(
                PairwiseFailureCode::memory_binding_mask_creation_failed,
                PairwiseFailureOperation::memory_region_binding_create,
                PairwiseFailureReason::native_mask_creation_failed,
                nested_platform(mask.error()))};
        return PairwisePreparedMemoryMask{
            std::move(*mask), domain, facts};
    } catch (const std::exception&) {
        return std::unexpected{memory_failure(
            PairwiseFailureCode::memory_binding_contained_exception,
            PairwiseFailureOperation::memory_region_binding_create,
            PairwiseFailureReason::contained_exception,
            NoPairwiseFailureContext{})};
    } catch (...) {
        return std::unexpected{memory_failure(
            PairwiseFailureCode::memory_binding_contained_unknown_failure,
            PairwiseFailureOperation::memory_region_binding_create,
            PairwiseFailureReason::contained_unknown_failure,
            NoPairwiseFailureContext{})};
    }
}

auto PairwiseMemoryOwningFacade::prepare_workspace(
    PairwisePreparedMemoryMask prepared, std::uint64_t total_pages) const noexcept
    -> std::expected<PairwiseMemoryWorkspace, PairwiseRuntimeFailure> {
    try {
        if (total_pages == 0U ||
            total_pages > std::numeric_limits<std::size_t>::max())
            return std::unexpected{runtime_preparation_failure()};
        const auto batch = static_cast<std::size_t>(std::min<std::uint64_t>(
            total_pages, PairwiseMemoryWorkspace::query_batch_pages));
        const auto possible_nodes = static_cast<std::uint64_t>(prepared.domain_.value());
        const auto node_capacity_u64 = std::min(total_pages, possible_nodes);
        if (node_capacity_u64 == 0U ||
            node_capacity_u64 > std::numeric_limits<std::size_t>::max())
            return std::unexpected{runtime_preparation_failure()};
        const auto node_capacity = static_cast<std::size_t>(node_capacity_u64);
        PairwiseNumaBindWorkspace bind_workspace{
            .words = std::vector<unsigned long>(prepared.domain_.word_count(), 0UL)};
        auto query_buffers = PairwisePreparedNumaQueryBuffers::prepare(batch);
        auto pre_counts = std::vector<memory::LinuxNodePageCount>{};
        auto post_counts = std::vector<memory::LinuxNodePageCount>{};
        pre_counts.reserve(node_capacity);
        post_counts.reserve(node_capacity);
        return PairwiseMemoryWorkspace{
            std::move(prepared), std::move(bind_workspace), std::move(query_buffers),
            std::move(pre_counts), std::move(post_counts), total_pages, node_capacity};
    } catch (...) {
        return std::unexpected{runtime_preparation_failure()};
    }
}

auto PairwiseMemoryOwningFacade::map(memory::RegionRequest request) const noexcept
    -> std::expected<memory::RegionMapping, PairwiseMemoryMappingFailure> {
    const auto fail = [](PairwiseMemoryFailure primary,
                         std::optional<PairwiseMemoryFailure> cleanup = std::nullopt,
                         std::optional<platform_linux::AnonymousMapping> retained =
                             std::nullopt) {
        return std::unexpected{PairwiseMemoryMappingFailure{
            std::move(primary), std::move(cleanup), std::move(retained)}};
    };
    try {
        before(hooks_, PairwiseMemoryOwningOperation::mapping);
        const auto requested = request.requested_bytes().value();
        const auto page = request.page_size().value();
        if (requested == 0U || page == 0U)
            return fail(memory_failure(
                PairwiseFailureCode::memory_mapping_invariant_failed,
                PairwiseFailureOperation::memory_region_mapping_create,
                PairwiseFailureReason::mapping_owner_invalid, mapping_context()));
        auto rounded = requested;
        const auto remainder = requested % page;
        if (remainder != 0U) {
            const auto padding = page - remainder;
            if (padding > std::numeric_limits<std::uint64_t>::max() - requested)
                return fail(memory_failure(
                    PairwiseFailureCode::memory_mapping_size_overflow,
                    PairwiseFailureOperation::memory_region_mapping_create,
                    PairwiseFailureReason::page_rounding_overflow, mapping_context()));
            rounded += padding;
        }
        if (rounded > std::numeric_limits<std::size_t>::max())
            return fail(memory_failure(
                PairwiseFailureCode::memory_mapping_native_size_unrepresentable,
                PairwiseFailureOperation::memory_region_mapping_create,
                PairwiseFailureReason::mapping_size_unrepresentable, mapping_context()));
        const auto mapping_bytes = static_cast<std::size_t>(rounded);
        auto mapped = platform_.map(mapping_bytes);
        if (!mapped)
            return fail(memory_failure(
                PairwiseFailureCode::memory_mapping_creation_failed,
                PairwiseFailureOperation::memory_region_mapping_create,
                PairwiseFailureReason::mapping_creation_failed, mapping_context(),
                mapped.error().posix_errno()));
        auto owner = std::move(*mapped);
        if (hooks_.after_mapping != nullptr)
            hooks_.after_mapping(hooks_.context, owner);
        const auto owner_valid = owner.active() && owner.size() == mapping_bytes &&
                                 owner.writable_bytes().size() == mapping_bytes &&
                                 std::as_const(owner).bytes().size() == mapping_bytes;
        std::optional<PairwiseMemoryFailure> primary;
        if (!owner_valid) {
            primary.emplace(memory_failure(
                PairwiseFailureCode::memory_mapping_invariant_failed,
                PairwiseFailureOperation::memory_region_mapping_create,
                PairwiseFailureReason::mapping_owner_invalid, mapping_context()));
        } else {
            auto advised = platform_.advise_no_huge_pages(owner);
            if (!advised)
                primary.emplace(memory_failure(
                    PairwiseFailureCode::memory_mapping_creation_failed,
                    PairwiseFailureOperation::memory_region_mapping_create,
                    PairwiseFailureReason::base_page_advice_failed, mapping_context(),
                    advised.error().posix_errno()));
        }
        if (primary) {
            std::optional<PairwiseMemoryFailure> cleanup;
            if (owner.active()) {
                auto released = platform_.unmap(owner);
                if (!released)
                    cleanup.emplace(memory_failure(
                        PairwiseFailureCode::memory_mapping_invariant_failed,
                        PairwiseFailureOperation::memory_region_mapping_create,
                        PairwiseFailureReason::rollback_failed, mapping_context(),
                        released.error().posix_errno()));
            }
            std::optional<platform_linux::AnonymousMapping> retained;
            if (cleanup)
                retained.emplace(std::move(owner));
            return fail(std::move(*primary), std::move(cleanup),
                        std::move(retained));
        }
        return memory::detail::PairwiseMemoryAccess::mapping(
            std::move(request), std::move(owner), mapping_bytes);
    } catch (const std::exception&) {
        return fail(memory_failure(
            PairwiseFailureCode::memory_mapping_contained_exception,
            PairwiseFailureOperation::memory_region_mapping_create,
            PairwiseFailureReason::contained_exception, mapping_context()));
    } catch (...) {
        return fail(memory_failure(
            PairwiseFailureCode::memory_mapping_contained_unknown_failure,
            PairwiseFailureOperation::memory_region_mapping_create,
            PairwiseFailureReason::contained_unknown_failure, mapping_context()));
    }
}

auto PairwiseMemoryOwningFacade::bind(
    memory::RegionMapping mapping, PairwiseMemoryWorkspace& workspace) const noexcept
    -> std::expected<memory::BoundRegion, PairwiseMemoryBindingFailure> {
    const auto fail = [&mapping](PairwiseMemoryFailure primary) {
        return std::unexpected{PairwiseMemoryBindingFailure{
            std::move(primary), std::move(mapping)}};
    };
    try {
        before(hooks_, PairwiseMemoryOwningOperation::binding);
        const auto& request = mapping.request();
        if (request.placement_policy() != memory::PlacementPolicy::bind)
            return fail(memory_failure(
                PairwiseFailureCode::memory_binding_invalid_argument,
                PairwiseFailureOperation::memory_region_binding_create,
                PairwiseFailureReason::unsupported_placement_policy,
                NoPairwiseFailureContext{}));
        if (request.numa_targets().empty())
            return fail(memory_failure(
                PairwiseFailureCode::memory_binding_invalid_argument,
                PairwiseFailureOperation::memory_region_binding_create,
                PairwiseFailureReason::missing_numa_target,
                NoPairwiseFailureContext{}));
        auto& owner = memory::detail::RegionMappingAccess::mapping(mapping);
        if (!owner.active())
            return fail(memory_failure(
                PairwiseFailureCode::memory_binding_invariant_failed,
                PairwiseFailureOperation::memory_region_binding_create,
                PairwiseFailureReason::mapping_inactive, NoPairwiseFailureContext{}));
        if (owner.size() != mapping.mapping_bytes() || mapping.mapping_bytes() == 0U ||
            owner.bytes().size() != mapping.mapping_bytes() ||
            std::as_const(mapping).full_mapping().size() != mapping.mapping_bytes())
            return fail(memory_failure(
                PairwiseFailureCode::memory_binding_invariant_failed,
                PairwiseFailureOperation::memory_region_binding_create,
                PairwiseFailureReason::mapping_range_invalid,
                NoPairwiseFailureContext{}));
        if (owner.protection() != platform_linux::MemoryProtection::read_write ||
            owner.writable_bytes().size() != mapping.mapping_bytes() ||
            mapping.full_mapping().size() != mapping.mapping_bytes())
            return fail(memory_failure(
                PairwiseFailureCode::memory_binding_invariant_failed,
                PairwiseFailureOperation::memory_region_binding_create,
                PairwiseFailureReason::mapping_not_writable,
                NoPairwiseFailureContext{}));
        auto bound = platform_.bind_diagnosed(
            owner, workspace.prepared_.mask_, workspace.prepared_.domain_,
            workspace.prepared_.facts_, workspace.bind_workspace_);
        if (!bound) {
            if (!bound.error().diagnosis)
                return fail(memory_failure(
                    PairwiseFailureCode::memory_binding_bind_failed,
                    PairwiseFailureOperation::memory_region_binding_create,
                    PairwiseFailureReason::binding_failed,
                    nested_platform(bound.error().primary)));
            const auto [code, reason] = binding_diagnosis(*bound.error().diagnosis);
            return fail(memory_failure(
                code, PairwiseFailureOperation::memory_region_binding_create, reason,
                nested_platform(bound.error().primary)));
        }
        if (hooks_.after_binding != nullptr)
            hooks_.after_binding(hooks_.context, owner);
        if (!owner.active() || owner.size() != mapping.mapping_bytes() ||
            owner.bytes().size() != mapping.mapping_bytes() ||
            owner.protection() != platform_linux::MemoryProtection::read_write ||
            owner.writable_bytes().size() != mapping.mapping_bytes())
            return fail(memory_failure(
                PairwiseFailureCode::memory_binding_invariant_failed,
                PairwiseFailureOperation::memory_region_binding_create,
                PairwiseFailureReason::binding_owner_invalid,
                NoPairwiseFailureContext{}));
        return memory::detail::PairwiseMemoryAccess::bound(std::move(mapping));
    } catch (const std::exception&) {
        return fail(memory_failure(
            PairwiseFailureCode::memory_binding_contained_exception,
            PairwiseFailureOperation::memory_region_binding_create,
            PairwiseFailureReason::contained_exception,
            NoPairwiseFailureContext{}));
    } catch (...) {
        return fail(memory_failure(
            PairwiseFailureCode::memory_binding_contained_unknown_failure,
            PairwiseFailureOperation::memory_region_binding_create,
            PairwiseFailureReason::contained_unknown_failure,
            NoPairwiseFailureContext{}));
    }
}

auto PairwiseMemoryOwningFacade::initialize(
    memory::BoundRegion region,
    const PairwisePreparedRegionInitializer* initializer) const noexcept
    -> std::expected<memory::InitializedRegion,
                     PairwiseMemoryInitializationFailure> {
    const auto fail = [&region](PairwiseMemoryInitializationPrimary primary) {
        return std::unexpected{PairwiseMemoryInitializationFailure{
            std::move(primary), std::move(region)}};
    };
    const auto memory_fail = [&fail](PairwiseFailureCode code,
                                     PairwiseFailureReason reason) {
        return fail(PairwiseMemoryInitializationPrimary{std::in_place_type<PairwiseMemoryFailure>,
            memory_failure(code,
                           PairwiseFailureOperation::memory_region_initialization_create,
                           reason, NoPairwiseFailureContext{})});
    };
    try {
        before(hooks_, PairwiseMemoryOwningOperation::initialization);
        auto& mapping = memory::detail::BoundRegionAccess::mapping(region);
        auto& owner = memory::detail::RegionMappingAccess::mapping(mapping);
        if (!owner.active())
            return memory_fail(
                PairwiseFailureCode::memory_initialization_invariant_failed,
                PairwiseFailureReason::mapping_inactive);
        if (owner.size() != mapping.mapping_bytes() ||
            std::as_const(owner).bytes().size() != mapping.mapping_bytes() ||
            std::as_const(mapping).full_mapping().size() != mapping.mapping_bytes())
            return memory_fail(
                PairwiseFailureCode::memory_initialization_invariant_failed,
                PairwiseFailureReason::initialization_owner_invalid);
        if (owner.protection() != platform_linux::MemoryProtection::read_write ||
            owner.writable_bytes().size() != mapping.mapping_bytes() ||
            mapping.full_mapping().size() != mapping.mapping_bytes())
            return memory_fail(
                PairwiseFailureCode::memory_initialization_invariant_failed,
                PairwiseFailureReason::mapping_not_writable);
        auto requested_region = mapping.requested_region();
        const auto requested = mapping.request().requested_bytes().value();
        if (requested_region.empty())
            return memory_fail(
                PairwiseFailureCode::memory_initialization_invariant_failed,
                PairwiseFailureReason::requested_region_unavailable);
        if (requested > std::numeric_limits<std::size_t>::max() ||
            requested_region.size() != static_cast<std::size_t>(requested) ||
            requested_region.size() > mapping.full_mapping().size() || requested == 0U)
            return memory_fail(
                PairwiseFailureCode::memory_initialization_invariant_failed,
                PairwiseFailureReason::requested_range_invalid);
        const auto page = mapping.request().page_size().value();
        if (page == 0U || page > std::numeric_limits<std::size_t>::max())
            return memory_fail(
                PairwiseFailureCode::memory_initialization_invalid_argument,
                PairwiseFailureReason::page_size_invalid);
        if (requested % page != 0U)
            return memory_fail(
                PairwiseFailureCode::memory_initialization_invalid_argument,
                PairwiseFailureReason::page_alignment_invalid);
        if (initializer == nullptr)
            return memory_fail(
                PairwiseFailureCode::memory_initialization_invalid_argument,
                PairwiseFailureReason::initializer_invalid);

        bool wrote_prefault = false;
        if (mapping.request().prefault()) {
            const auto page_offset = static_cast<std::size_t>(page);
            std::size_t offset{};
            while (offset < requested_region.size()) {
                bool written{};
                try {
                    if (hooks_.write_prefault_byte != nullptr)
                        written = hooks_.write_prefault_byte(
                            hooks_.context, requested_region, offset, std::byte{0});
                    else {
                        requested_region[offset] = std::byte{0};
                        written = true;
                    }
                } catch (const std::exception&) {
                    return memory_fail(
                        PairwiseFailureCode::memory_initialization_contained_exception,
                        wrote_prefault ? PairwiseFailureReason::partial_initialization
                                       : PairwiseFailureReason::contained_exception);
                } catch (...) {
                    return memory_fail(
                        PairwiseFailureCode::memory_initialization_contained_unknown_failure,
                        wrote_prefault ? PairwiseFailureReason::partial_initialization
                                       : PairwiseFailureReason::contained_unknown_failure);
                }
                if (!written)
                    return memory_fail(
                        PairwiseFailureCode::memory_initialization_prefault_failed,
                        wrote_prefault ? PairwiseFailureReason::partial_initialization
                                       : PairwiseFailureReason::prefault_failed);
                wrote_prefault = true;
                const auto remaining = requested_region.size() - offset;
                offset = remaining <= page_offset ? requested_region.size()
                                                  : offset + page_offset;
            }
        }
        auto initialized = kernel_.initialize_region(*initializer, requested_region);
        if (!initialized)
            return fail(PairwiseMemoryInitializationPrimary{
                std::in_place_type<PairwiseKernelFailure>,
                std::move(initialized.error())});
        const auto prefault = mapping.request().prefault();
        return memory::detail::PairwiseMemoryAccess::initialized(
            std::move(region), prefault);
    } catch (const std::exception&) {
        return memory_fail(
            PairwiseFailureCode::memory_initialization_contained_exception,
            PairwiseFailureReason::contained_exception);
    } catch (...) {
        return memory_fail(
            PairwiseFailureCode::memory_initialization_contained_unknown_failure,
            PairwiseFailureReason::contained_unknown_failure);
    }
}

auto PairwiseMemoryOwningFacade::observe(
    memory::InitializedRegion region,
    PairwiseMemoryWorkspace& workspace) const noexcept
    -> std::expected<memory::PlacementObservedRegion, PairwiseMemoryQueryFailure> {
    const auto fail = [&region](PairwiseMemoryFailure primary) {
        return std::unexpected{PairwiseMemoryQueryFailure{
            std::move(primary), std::move(region)}};
    };
    try {
        if (!region.request().verify_before())
            return memory::detail::PairwiseMemoryAccess::observed(
                std::move(region), memory::PreRunPlacementQueryState::skipped,
                std::nullopt);
        auto observation = query_observation(region, workspace, false);
        if (!observation)
            return fail(std::move(observation.error()));
        return memory::detail::PairwiseMemoryAccess::observed(
            std::move(region), memory::PreRunPlacementQueryState::observed,
            std::optional<memory::RegionPlacementObservation>{
                std::move(*observation)});
    } catch (const std::exception&) {
        return fail(memory_failure(
            PairwiseFailureCode::memory_placement_query_contained_exception,
            PairwiseFailureOperation::memory_region_placement_observe,
            PairwiseFailureReason::contained_exception,
            NoPairwiseFailureContext{}));
    } catch (...) {
        return fail(memory_failure(
            PairwiseFailureCode::memory_placement_query_contained_unknown_failure,
            PairwiseFailureOperation::memory_region_placement_observe,
            PairwiseFailureReason::contained_unknown_failure,
            NoPairwiseFailureContext{}));
    }
}

auto PairwiseMemoryOwningFacade::evaluate(
    memory::PlacementObservedRegion region) const noexcept
    -> std::expected<memory::PlacementEvaluatedRegion,
                     PairwiseMemoryEvaluationFailure> {
    const auto fail = [&region](PairwiseFailureCode code,
                                PairwiseFailureReason reason) {
        return std::unexpected{PairwiseMemoryEvaluationFailure{
            memory_failure(code,
                           PairwiseFailureOperation::memory_region_placement_evaluate,
                           reason, NoPairwiseFailureContext{}),
            std::move(region)}};
    };
    try {
        before(hooks_, PairwiseMemoryOwningOperation::placement_evaluation);
        const auto state = region.query_state();
        const auto& observation = region.observation();
        if (state == memory::PreRunPlacementQueryState::skipped) {
            if (observation)
                return fail(
                    PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                    PairwiseFailureReason::observation_unexpected);
            if (region.request().verify_before())
                return fail(
                    PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                    PairwiseFailureReason::qualification_owner_invalid);
            return memory::detail::PairwiseMemoryAccess::evaluated(
                std::move(region), memory::PlacementQualificationState::skipped,
                std::nullopt);
        }
        if (state != memory::PreRunPlacementQueryState::observed)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invalid_argument,
                PairwiseFailureReason::query_state_invalid);
        if (!observation)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                PairwiseFailureReason::observation_missing);
        if (!region.request().verify_before())
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                PairwiseFailureReason::qualification_owner_invalid);

        const auto total = observation->total_pages();
        const auto resident = observation->resident_pages();
        const auto unresolved = observation->unresolved_pages();
        if (total == 0U)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invalid_argument,
                PairwiseFailureReason::denominator_invalid);
        if (unresolved > std::numeric_limits<std::uint64_t>::max() - resident ||
            resident + unresolved != total)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                PairwiseFailureReason::observation_inconsistent);
        std::uint64_t resident_sum{};
        std::uint64_t requested_pages{};
        std::uint64_t unexpected_pages{};
        std::optional<core::LinuxNumaNodeId> prior;
        for (const auto& count : observation->resident_page_counts()) {
            if (count.page_count() == 0U ||
                (prior && count.linux_node() <= *prior))
                return fail(
                    PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                    PairwiseFailureReason::observation_inconsistent);
            prior = count.linux_node();
            if (count.page_count() >
                std::numeric_limits<std::uint64_t>::max() - resident_sum)
                return fail(
                    PairwiseFailureCode::memory_placement_evaluation_arithmetic_failed,
                    PairwiseFailureReason::resident_count_inconsistent);
            resident_sum += count.page_count();
            const auto requested_node = std::ranges::any_of(
                region.request().numa_targets(), [&](const auto& target) {
                    return target.linux_node == count.linux_node();
                });
            auto& destination = requested_node ? requested_pages : unexpected_pages;
            if (count.page_count() >
                std::numeric_limits<std::uint64_t>::max() - destination)
                return fail(
                    PairwiseFailureCode::memory_placement_evaluation_arithmetic_failed,
                    requested_node ? PairwiseFailureReason::requested_count_overflow
                                   : PairwiseFailureReason::unexpected_count_overflow);
            destination += count.page_count();
        }
        if (resident_sum != resident)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                PairwiseFailureReason::resident_count_inconsistent);
        if (unexpected_pages >
                std::numeric_limits<std::uint64_t>::max() - requested_pages ||
            requested_pages + unexpected_pages != resident ||
            unresolved > std::numeric_limits<std::uint64_t>::max() - resident ||
            resident + unresolved != total)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                PairwiseFailureReason::observation_inconsistent);
        if (requested_pages > total)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_arithmetic_failed,
                PairwiseFailureReason::ratio_construction_failed);
        const auto divisor = std::gcd(requested_pages, total);
        if (divisor == 0U)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_arithmetic_failed,
                PairwiseFailureReason::ratio_construction_failed);
        const auto ratio = memory::detail::PairwiseMemoryAccess::ratio(
            requested_pages / divisor, total / divisor);
        const auto comparison =
            memory::detail::PlacementEvaluatedRegionAccess::compare_exact_ratios(
                ratio, region.request().placement_threshold());
        const auto qualification_state =
            comparison == memory::detail::ExactRatioOrdering::less
                ? memory::PlacementQualificationState::not_qualified
                : memory::PlacementQualificationState::qualified;
        auto value = memory::detail::PlacementEvaluatedRegionAccess::evaluation(
            requested_pages, unexpected_pages, unresolved, ratio);
        return memory::detail::PairwiseMemoryAccess::evaluated(
            std::move(region), qualification_state,
            std::optional<memory::RegionPlacementEvaluation>{std::move(value)});
    } catch (const std::exception&) {
        return fail(
            PairwiseFailureCode::memory_placement_evaluation_contained_exception,
            PairwiseFailureReason::contained_exception);
    } catch (...) {
        return fail(
            PairwiseFailureCode::memory_placement_evaluation_contained_unknown_failure,
            PairwiseFailureReason::contained_unknown_failure);
    }
}

auto PairwiseMemoryOwningFacade::protect(
    memory::PlacementEvaluatedRegion region) const noexcept
    -> std::expected<memory::ReadOnlyRegion, PairwiseMemoryProtectionFailure> {
    const auto state = region.qualification_state();
    const auto fail = [&region, state](PairwiseFailureCode code,
                                       PairwiseFailureReason reason,
                                       std::optional<int> error_number = std::nullopt) {
        return std::unexpected{PairwiseMemoryProtectionFailure{
            memory_failure(code, PairwiseFailureOperation::memory_region_protect,
                           reason, protection_context(state, reason), error_number),
            std::move(region)}};
    };
    try {
        before(hooks_, PairwiseMemoryOwningOperation::protection);
        if (!region.execution_eligible())
            return fail(PairwiseFailureCode::memory_protection_invalid_argument,
                        PairwiseFailureReason::execution_not_eligible);
        if (state == memory::PlacementQualificationState::skipped &&
            region.request().verify_before())
            return fail(PairwiseFailureCode::memory_protection_invariant_failed,
                        PairwiseFailureReason::protection_owner_invalid);
        if (!region.request().requires_read_only())
            return fail(PairwiseFailureCode::memory_protection_invalid_argument,
                        PairwiseFailureReason::read_only_not_required);
        auto& mapping = region_mapping(region);
        auto& owner = memory::detail::RegionMappingAccess::mapping(mapping);
        if (!owner.active())
            return fail(PairwiseFailureCode::memory_protection_invalid_argument,
                        PairwiseFailureReason::mapping_inactive);
        if (owner.protection() == platform_linux::MemoryProtection::read_only)
            return fail(PairwiseFailureCode::memory_protection_invalid_argument,
                        PairwiseFailureReason::mapping_already_read_only);
        if (owner.protection() != platform_linux::MemoryProtection::read_write ||
            owner.writable_bytes().size() != mapping.mapping_bytes())
            return fail(PairwiseFailureCode::memory_protection_invalid_argument,
                        PairwiseFailureReason::mapping_not_writable);
        if (owner.size() != mapping.mapping_bytes() ||
            owner.bytes().size() != mapping.mapping_bytes() ||
            mapping.full_mapping().size() != mapping.mapping_bytes() ||
            mapping.requested_region().size() != mapping.requested_bytes().value())
            return fail(PairwiseFailureCode::memory_protection_invariant_failed,
                        PairwiseFailureReason::mapping_range_invalid);
        auto protected_result = platform_.protect_read_only(owner);
        if (!protected_result)
            return fail(PairwiseFailureCode::memory_protection_failed,
                        PairwiseFailureReason::protection_failed,
                        protected_result.error().posix_errno());
        if (hooks_.after_protection != nullptr)
            hooks_.after_protection(hooks_.context, owner);
        if (!owner.active() ||
            owner.protection() != platform_linux::MemoryProtection::read_only ||
            !owner.writable_bytes().empty() ||
            owner.bytes().size() != mapping.mapping_bytes())
            return fail(PairwiseFailureCode::memory_protection_invariant_failed,
                        PairwiseFailureReason::protection_state_inconsistent);
        return memory::detail::PairwiseMemoryAccess::read_only(std::move(region));
    } catch (const std::bad_alloc&) {
        return fail(PairwiseFailureCode::memory_protection_contained_exception,
                    PairwiseFailureReason::allocation_failure);
    } catch (const std::exception&) {
        return fail(PairwiseFailureCode::memory_protection_contained_exception,
                    PairwiseFailureReason::contained_exception);
    } catch (...) {
        return fail(PairwiseFailureCode::memory_protection_contained_unknown_failure,
                    PairwiseFailureReason::contained_unknown_failure);
    }
}

namespace {

[[nodiscard]] auto finalization_context() noexcept -> PairwiseFailureContext {
    return LifecyclePhaseContext{PairwiseLifecyclePhase::finalization};
}

[[nodiscard]] auto nested_memory(const PairwiseMemoryFailure& lower) noexcept
    -> PairwiseFailureContext {
    return std::visit(
        [&lower]<class Context>(const Context& context) -> PairwiseFailureContext {
            if constexpr (std::is_same_v<Context, NestedFailureContext>) {
                return NestedFailureContext{
                    PairwiseNestedFailureValue{
                        lower.code(), lower.operation(), lower.reason(),
                        NoPairwiseFailureContext{}, lower.posix_errno()},
                    PairwiseLifecyclePhase::finalization, context.failure};
            } else {
                return NestedFailureContext{
                    PairwiseNestedFailureValue{
                        lower.code(), lower.operation(), lower.reason(),
                        PairwiseFlatFailureContext{context}, lower.posix_errno()},
                    PairwiseLifecyclePhase::finalization, std::nullopt};
            }
        },
        lower.context());
}

[[nodiscard]] auto evaluated_initialized(
    const memory::PlacementEvaluatedRegion& value) noexcept
    -> const memory::InitializedRegion& {
    return memory::detail::PlacementObservedRegionAccess::region(
        memory::detail::PlacementEvaluatedRegionAccess::region(value));
}

[[nodiscard]] auto evaluated_mapping(
    memory::PlacementEvaluatedRegion& value) noexcept -> memory::RegionMapping& {
    return region_mapping(value);
}

[[nodiscard]] auto finalization_mapping(
    PairwiseMemoryFinalizationOwner& owner) noexcept -> memory::RegionMapping& {
    return std::visit(
        []<class Owner>(Owner& value) -> memory::RegionMapping& {
            if constexpr (std::is_same_v<Owner, memory::ReadOnlyRegion>)
                return evaluated_mapping(
                    memory::detail::ReadOnlyRegionAccess::region(value));
            else
                return evaluated_mapping(value);
        },
        owner);
}

[[nodiscard]] auto finalization_evaluated(
    PairwiseMemoryFinalizationOwner& owner) noexcept
    -> memory::PlacementEvaluatedRegion& {
    return std::visit(
        []<class Owner>(Owner& value) -> memory::PlacementEvaluatedRegion& {
            if constexpr (std::is_same_v<Owner, memory::ReadOnlyRegion>)
                return memory::detail::ReadOnlyRegionAccess::region(value);
            else
                return value;
        },
        owner);
}

[[nodiscard]] auto finalization_failure(PairwiseFailureCode code,
                                         PairwiseFailureReason reason,
                                         PairwiseFailureContext context) noexcept
    -> PairwiseMemoryFailure {
    return memory_failure(code, PairwiseFailureOperation::memory_region_finalize,
                          reason, std::move(context));
}

struct TypedPlacementEvaluation final {
    memory::RegionPlacementEvaluation evaluation;
    memory::PlacementQualificationState qualification;
};

[[nodiscard]] auto evaluate_observation_typed(
    const memory::RegionRequest& request,
    const memory::RegionPlacementObservation& observation) noexcept
    -> std::expected<TypedPlacementEvaluation, PairwiseMemoryFailure> {
    const auto fail = [](PairwiseFailureCode code, PairwiseFailureReason reason) {
        return std::unexpected{memory_failure(
            code, PairwiseFailureOperation::memory_region_placement_evaluate,
            reason, NoPairwiseFailureContext{})};
    };
    try {
        const auto total = observation.total_pages();
        const auto resident = observation.resident_pages();
        const auto unresolved = observation.unresolved_pages();
        if (total == 0U)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invalid_argument,
                PairwiseFailureReason::denominator_invalid);
        if (unresolved > std::numeric_limits<std::uint64_t>::max() - resident ||
            resident + unresolved != total)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                PairwiseFailureReason::observation_inconsistent);
        std::uint64_t resident_sum{};
        std::uint64_t requested_pages{};
        std::uint64_t unexpected_pages{};
        std::optional<core::LinuxNumaNodeId> prior;
        for (const auto& count : observation.resident_page_counts()) {
            if (count.page_count() == 0U ||
                (prior && count.linux_node() <= *prior))
                return fail(
                    PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                    PairwiseFailureReason::observation_inconsistent);
            prior = count.linux_node();
            if (count.page_count() >
                std::numeric_limits<std::uint64_t>::max() - resident_sum)
                return fail(
                    PairwiseFailureCode::memory_placement_evaluation_arithmetic_failed,
                    PairwiseFailureReason::resident_count_inconsistent);
            resident_sum += count.page_count();
            const auto requested_node = std::ranges::any_of(
                request.numa_targets(), [&](const auto& target) {
                    return target.linux_node == count.linux_node();
                });
            auto& destination = requested_node ? requested_pages : unexpected_pages;
            if (count.page_count() >
                std::numeric_limits<std::uint64_t>::max() - destination)
                return fail(
                    PairwiseFailureCode::memory_placement_evaluation_arithmetic_failed,
                    requested_node ? PairwiseFailureReason::requested_count_overflow
                                   : PairwiseFailureReason::unexpected_count_overflow);
            destination += count.page_count();
        }
        if (resident_sum != resident)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                PairwiseFailureReason::resident_count_inconsistent);
        if (unexpected_pages >
                std::numeric_limits<std::uint64_t>::max() - requested_pages ||
            requested_pages + unexpected_pages != resident)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_invariant_failed,
                PairwiseFailureReason::observation_inconsistent);
        if (requested_pages > total)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_arithmetic_failed,
                PairwiseFailureReason::ratio_construction_failed);
        const auto divisor = std::gcd(requested_pages, total);
        if (divisor == 0U)
            return fail(
                PairwiseFailureCode::memory_placement_evaluation_arithmetic_failed,
                PairwiseFailureReason::ratio_construction_failed);
        const auto ratio = memory::detail::PairwiseMemoryAccess::ratio(
            requested_pages / divisor, total / divisor);
        const auto ordering =
            memory::detail::PlacementEvaluatedRegionAccess::compare_exact_ratios(
                ratio, request.placement_threshold());
        const auto state = ordering == memory::detail::ExactRatioOrdering::less
            ? memory::PlacementQualificationState::not_qualified
            : memory::PlacementQualificationState::qualified;
        return TypedPlacementEvaluation{
            memory::detail::PlacementEvaluatedRegionAccess::evaluation(
                requested_pages, unexpected_pages, unresolved, ratio),
            state};
    } catch (const std::exception&) {
        return fail(
            PairwiseFailureCode::memory_placement_evaluation_contained_exception,
            PairwiseFailureReason::contained_exception);
    } catch (...) {
        return fail(
            PairwiseFailureCode::memory_placement_evaluation_contained_unknown_failure,
            PairwiseFailureReason::contained_unknown_failure);
    }
}

[[nodiscard]] auto released_state(
    const platform_linux::AnonymousMapping& mapping) noexcept -> bool {
    return !mapping.active() && mapping.size() == 0U &&
           mapping.protection() == platform_linux::MemoryProtection::none &&
           mapping.bytes().empty();
}

} // namespace

auto PairwiseMemoryOwningFacade::finish_finalization(
    PairwiseMemoryFinalizationOwner owner,
    std::optional<memory::PostRunPlacementEvidence> evidence,
    memory::RegionFinalizationPath path,
    std::optional<PairwiseMemoryFailure> primary) const noexcept
    -> std::expected<memory::FinalizedRegion,
                     PairwiseMemoryFinalizationFailure> {
    try {
        auto& native = memory::detail::RegionMappingAccess::mapping(
            finalization_mapping(owner));
        auto released = platform_.unmap(native);
        if (!released) {
            auto cleanup = finalization_failure(
                PairwiseFailureCode::memory_finalization_release_failed,
                PairwiseFailureReason::release_failed,
                nested_platform(released.error()));
            if (primary)
                return std::unexpected{PairwiseMemoryFinalizationFailure{
                    std::move(*primary), std::optional<PairwiseMemoryFailure>{
                        std::move(cleanup)}, std::move(owner), std::move(evidence),
                    memory::detail::RegionFinalizationAccess::cleanup(false), path}};
            return std::unexpected{PairwiseMemoryFinalizationFailure{
                std::move(cleanup), std::nullopt, std::move(owner),
                std::move(evidence),
                memory::detail::RegionFinalizationAccess::cleanup(false), path}};
        }
        if (!released_state(native)) {
            auto cleanup = finalization_failure(
                PairwiseFailureCode::memory_finalization_invariant_failed,
                PairwiseFailureReason::release_state_inconsistent,
                finalization_context());
            if (primary)
                return std::unexpected{PairwiseMemoryFinalizationFailure{
                    std::move(*primary), std::optional<PairwiseMemoryFailure>{
                        std::move(cleanup)}, std::move(owner), std::move(evidence),
                    memory::detail::RegionFinalizationAccess::cleanup(false), path}};
            return std::unexpected{PairwiseMemoryFinalizationFailure{
                std::move(cleanup), std::nullopt, std::move(owner),
                std::move(evidence),
                memory::detail::RegionFinalizationAccess::cleanup(false), path}};
        }
        if (primary)
            return std::unexpected{PairwiseMemoryFinalizationFailure{
                std::move(*primary), std::nullopt, std::move(owner),
                std::move(evidence),
                memory::detail::RegionFinalizationAccess::cleanup(true), path}};
        if (!evidence)
            std::terminate();
        return memory::detail::RegionFinalizationAccess::success(
            std::move(owner), std::move(*evidence),
            memory::detail::RegionFinalizationAccess::cleanup(true), path);
    } catch (const std::exception&) {
        auto failure = finalization_failure(
            PairwiseFailureCode::memory_finalization_contained_exception,
            PairwiseFailureReason::contained_exception, finalization_context());
        return std::unexpected{PairwiseMemoryFinalizationFailure{
            std::move(primary ? *primary : failure), std::nullopt,
            std::move(owner), std::move(evidence),
            memory::detail::RegionFinalizationAccess::cleanup_not_attempted(true), path}};
    } catch (...) {
        auto failure = finalization_failure(
            PairwiseFailureCode::memory_finalization_contained_unknown_failure,
            PairwiseFailureReason::contained_unknown_failure,
            finalization_context());
        return std::unexpected{PairwiseMemoryFinalizationFailure{
            std::move(primary ? *primary : failure), std::nullopt,
            std::move(owner), std::move(evidence),
            memory::detail::RegionFinalizationAccess::cleanup_not_attempted(true), path}};
    }
}

auto PairwiseMemoryOwningFacade::finalize(
    memory::ReadOnlyRegion region,
    memory::PostRunVerificationApplicability applicability,
    PairwiseMemoryWorkspace& workspace) const noexcept
    -> std::expected<memory::FinalizedRegion,
                     PairwiseMemoryFinalizationFailure> {
    PairwiseMemoryFinalizationOwner owner{
        std::in_place_type<memory::ReadOnlyRegion>, std::move(region)};
    const auto path = memory::RegionFinalizationPath::protected_execution;
    try {
        before(hooks_, PairwiseMemoryOwningOperation::finalization);
        auto& evaluated = finalization_evaluated(owner);
        auto& mapping = finalization_mapping(owner);
        auto& native = memory::detail::RegionMappingAccess::mapping(mapping);
        if (!native.active() || native.size() != mapping.mapping_bytes() ||
            native.bytes().size() != mapping.mapping_bytes())
            return finish_finalization(
                std::move(owner), std::nullopt, path,
                finalization_failure(
                    PairwiseFailureCode::memory_finalization_invariant_failed,
                    PairwiseFailureReason::finalization_owner_invalid,
                    finalization_context()));
        if (native.protection() != platform_linux::MemoryProtection::read_only ||
            !native.writable_bytes().empty())
            return finish_finalization(
                std::move(owner), std::nullopt, path,
                finalization_failure(
                    PairwiseFailureCode::memory_finalization_invalid_argument,
                    PairwiseFailureReason::lifecycle_state_invalid,
                    finalization_context()));
        if (applicability != memory::PostRunVerificationApplicability::applicable &&
            applicability != memory::PostRunVerificationApplicability::not_applicable)
            return finish_finalization(
                std::move(owner), std::nullopt, path,
                finalization_failure(
                    PairwiseFailureCode::memory_finalization_invalid_argument,
                    PairwiseFailureReason::post_verification_not_applicable,
                    finalization_context()));
        if (applicability ==
            memory::PostRunVerificationApplicability::not_applicable)
            return finish_finalization(
                std::move(owner),
                memory::detail::RegionFinalizationAccess::post_run(
                    memory::PostRunVerificationState::not_applicable),
                path, std::nullopt);
        if (!evaluated.request().verify_after())
            return finish_finalization(
                std::move(owner),
                memory::detail::RegionFinalizationAccess::post_run(
                    memory::PostRunVerificationState::skipped),
                path, std::nullopt);
        auto observation = query_observation(
            evaluated_initialized(evaluated), workspace, true);
        if (!observation)
            return finish_finalization(
                std::move(owner), std::nullopt, path,
                finalization_failure(
                    PairwiseFailureCode::memory_finalization_post_query_failed,
                    PairwiseFailureReason::post_observation_inconsistent,
                    nested_memory(observation.error())));
        auto evaluation = evaluate_observation_typed(
            evaluated.request(), *observation);
        if (!evaluation)
            return finish_finalization(
                std::move(owner),
                memory::detail::RegionFinalizationAccess::post_run(
                    memory::PostRunVerificationState::observed,
                    std::move(*observation)), path,
                finalization_failure(
                    PairwiseFailureCode::memory_finalization_post_evaluation_failed,
                    PairwiseFailureReason::post_evaluation_failed,
                    nested_memory(evaluation.error())));
        auto post = memory::detail::RegionFinalizationAccess::post_run(
            memory::PostRunVerificationState::observed,
            std::optional<memory::RegionPlacementObservation>{
                std::move(*observation)},
            std::optional<memory::RegionPlacementEvaluation>{
                std::move(evaluation->evaluation)},
            evaluation->qualification);
        return finish_finalization(
            std::move(owner), std::move(post), path, std::nullopt);
    } catch (const std::exception&) {
        return finish_finalization(
            std::move(owner), std::nullopt, path,
            finalization_failure(
                PairwiseFailureCode::memory_finalization_contained_exception,
                PairwiseFailureReason::contained_exception,
                finalization_context()));
    } catch (...) {
        return finish_finalization(
            std::move(owner), std::nullopt, path,
            finalization_failure(
                PairwiseFailureCode::memory_finalization_contained_unknown_failure,
                PairwiseFailureReason::contained_unknown_failure,
                finalization_context()));
    }
}

auto PairwiseMemoryOwningFacade::finalize_unprotected(
    memory::PlacementEvaluatedRegion region,
    memory::UnprotectedFinalizationReason reason) const noexcept
    -> std::expected<memory::FinalizedRegion,
                     PairwiseMemoryFinalizationFailure> {
    auto path = reason == memory::UnprotectedFinalizationReason::protection_failed
        ? memory::RegionFinalizationPath::unprotected_protection_failed
        : memory::RegionFinalizationPath::unprotected_execution_ineligible;
    PairwiseMemoryFinalizationOwner owner{
        std::in_place_type<memory::PlacementEvaluatedRegion>,
        std::move(region)};
    try {
        before(hooks_, PairwiseMemoryOwningOperation::finalization);
        auto& evaluated = finalization_evaluated(owner);
        auto& mapping = finalization_mapping(owner);
        auto& native = memory::detail::RegionMappingAccess::mapping(mapping);
        if (!native.active() || native.size() != mapping.mapping_bytes() ||
            native.bytes().size() != mapping.mapping_bytes())
            return finish_finalization(
                std::move(owner), std::nullopt, path,
                finalization_failure(
                    PairwiseFailureCode::memory_finalization_invariant_failed,
                    PairwiseFailureReason::finalization_owner_invalid,
                    finalization_context()));
        if (native.protection() != platform_linux::MemoryProtection::read_write ||
            native.writable_bytes().size() != mapping.mapping_bytes())
            return finish_finalization(
                std::move(owner), std::nullopt, path,
                finalization_failure(
                    PairwiseFailureCode::memory_finalization_invalid_argument,
                    PairwiseFailureReason::lifecycle_state_invalid,
                    finalization_context()));
        const auto reason_valid =
            (reason == memory::UnprotectedFinalizationReason::execution_ineligible &&
             !evaluated.execution_eligible() &&
             evaluated.qualification_state() ==
                 memory::PlacementQualificationState::not_qualified) ||
            (reason == memory::UnprotectedFinalizationReason::protection_failed &&
             evaluated.execution_eligible());
        if (!reason_valid)
            return finish_finalization(
                std::move(owner), std::nullopt, path,
                finalization_failure(
                    PairwiseFailureCode::memory_finalization_invalid_argument,
                    PairwiseFailureReason::unprotected_reason_invalid,
                    finalization_context()));
        return finish_finalization(
            std::move(owner),
            memory::detail::RegionFinalizationAccess::post_run(
                memory::PostRunVerificationState::not_applicable),
            path, std::nullopt);
    } catch (const std::exception&) {
        return finish_finalization(
            std::move(owner), std::nullopt, path,
            finalization_failure(
                PairwiseFailureCode::memory_finalization_contained_exception,
                PairwiseFailureReason::contained_exception,
                finalization_context()));
    } catch (...) {
        return finish_finalization(
            std::move(owner), std::nullopt, path,
            finalization_failure(
                PairwiseFailureCode::memory_finalization_contained_unknown_failure,
                PairwiseFailureReason::contained_unknown_failure,
                finalization_context()));
    }
}

namespace {

[[nodiscard]] auto cleanup_mapping(memory::RegionMapping& value) noexcept
    -> memory::RegionMapping& { return value; }
[[nodiscard]] auto cleanup_mapping(memory::BoundRegion& value) noexcept
    -> memory::RegionMapping& { return region_mapping(value); }
[[nodiscard]] auto cleanup_mapping(memory::InitializedRegion& value) noexcept
    -> memory::RegionMapping& { return region_mapping(value); }
[[nodiscard]] auto cleanup_mapping(memory::PlacementObservedRegion& value) noexcept
    -> memory::RegionMapping& { return region_mapping(value); }

[[nodiscard]] constexpr auto cleanup_phase(
    memory::RetainedRegionSourceState source) noexcept -> PairwiseLifecyclePhase {
    switch (source) {
    case memory::RetainedRegionSourceState::mapping:
        return PairwiseLifecyclePhase::retained_region_release_mapping;
    case memory::RetainedRegionSourceState::bound:
        return PairwiseLifecyclePhase::retained_region_release_bound;
    case memory::RetainedRegionSourceState::initialized:
        return PairwiseLifecyclePhase::retained_region_release_initialized;
    case memory::RetainedRegionSourceState::placement_observed:
        return PairwiseLifecyclePhase::retained_region_release_placement_observed;
    }
    std::terminate();
}

template <class Owner>
[[nodiscard]] auto cleanup_owned(
    Owner owner, memory::RetainedRegionSourceState source,
    const PairwisePlatformLeafFacade& platform,
    const PairwiseMemoryOwningHooks& hooks) noexcept
    -> std::expected<memory::CleanedRetainedRegion,
                     PairwiseMemoryCleanupFailure<Owner>> {
    bool attempted = false;
    const auto evidence = [](bool did_attempt, bool succeeded,
                             bool active_after) noexcept {
        return memory::detail::RetainedRegionCleanupAccess::evidence(
            did_attempt, succeeded, active_after);
    };
    const auto fail = [&owner, source](PairwiseFailureCode code,
                                       PairwiseFailureReason reason,
                                       memory::RegionCleanupEvidence cleanup_evidence,
                                       std::optional<int> error_number = std::nullopt) {
        return std::unexpected{PairwiseMemoryCleanupFailure<Owner>{
            memory_failure(
                code, PairwiseFailureOperation::memory_retained_cleanup, reason,
                LifecyclePhaseContext{cleanup_phase(source)}, error_number),
            cleanup_evidence, source, std::move(owner)}};
    };
    try {
        before(hooks, PairwiseMemoryOwningOperation::retained_cleanup);
        auto& region = cleanup_mapping(owner);
        auto& native = memory::detail::RegionMappingAccess::mapping(region);
        const auto valid = native.active() && native.size() > 0U &&
            native.protection() == platform_linux::MemoryProtection::read_write &&
            native.size() == region.mapping_bytes() &&
            native.bytes().size() == region.mapping_bytes() &&
            native.writable_bytes().size() == region.mapping_bytes() &&
            region.full_mapping().size() == region.mapping_bytes();
        if (!valid)
            return fail(PairwiseFailureCode::memory_cleanup_invalid_argument,
                        PairwiseFailureReason::lifecycle_state_invalid,
                        evidence(false, false, native.active()));
        attempted = true;
        auto result = platform.unmap(native);
        if (!result)
            return fail(PairwiseFailureCode::memory_cleanup_release_failed,
                        PairwiseFailureReason::release_failed,
                        evidence(true, false, true),
                        result.error().posix_errno());
        if (!released_state(native))
            return fail(PairwiseFailureCode::memory_cleanup_invariant_failed,
                        PairwiseFailureReason::release_state_inconsistent,
                        evidence(true, false, native.active()));
        return memory::detail::RetainedRegionCleanupAccess::success(
            source, evidence(true, true, false));
    } catch (const std::exception&) {
        const auto active =
            memory::detail::RegionMappingAccess::mapping(cleanup_mapping(owner)).active();
        return fail(PairwiseFailureCode::memory_cleanup_contained_exception,
                    PairwiseFailureReason::contained_exception,
                    evidence(attempted, false, active));
    } catch (...) {
        const auto active =
            memory::detail::RegionMappingAccess::mapping(cleanup_mapping(owner)).active();
        return fail(PairwiseFailureCode::memory_cleanup_contained_unknown_failure,
                    PairwiseFailureReason::contained_unknown_failure,
                    evidence(attempted, false, active));
    }
}

} // namespace

auto PairwiseMemoryOwningFacade::cleanup(memory::RegionMapping region) const noexcept
    -> std::expected<memory::CleanedRetainedRegion,
                     PairwiseMemoryCleanupFailure<memory::RegionMapping>> {
    return cleanup_owned(std::move(region),
                         memory::RetainedRegionSourceState::mapping,
                         platform_, hooks_);
}

auto PairwiseMemoryOwningFacade::cleanup(memory::BoundRegion region) const noexcept
    -> std::expected<memory::CleanedRetainedRegion,
                     PairwiseMemoryCleanupFailure<memory::BoundRegion>> {
    return cleanup_owned(std::move(region),
                         memory::RetainedRegionSourceState::bound,
                         platform_, hooks_);
}

auto PairwiseMemoryOwningFacade::cleanup(memory::InitializedRegion region) const noexcept
    -> std::expected<memory::CleanedRetainedRegion,
                     PairwiseMemoryCleanupFailure<memory::InitializedRegion>> {
    return cleanup_owned(std::move(region),
                         memory::RetainedRegionSourceState::initialized,
                         platform_, hooks_);
}

auto PairwiseMemoryOwningFacade::cleanup(
    memory::PlacementObservedRegion region) const noexcept
    -> std::expected<
        memory::CleanedRetainedRegion,
        PairwiseMemoryCleanupFailure<memory::PlacementObservedRegion>> {
    return cleanup_owned(
        std::move(region),
        memory::RetainedRegionSourceState::placement_observed,
        platform_, hooks_);
}

} // namespace epycbench::runtime::detail
