#include "epycbench/memory/region_placement_evaluation.hpp"

#include "memory/region_placement_evaluation_access.hpp"

#include <compare>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>

namespace epycbench::memory {
namespace {

auto evaluation_error(std::string_view code, std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Memory region placement evaluation failed",
            .operation = "memory.region_placement_evaluation.evaluate",
            .os_error = std::nullopt,
            .context = nlohmann::json{{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept -> bool {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

auto requested_node(const RegionRequest& request, core::LinuxNumaNodeId observed) noexcept -> bool {
    for (const auto& target : request.numa_targets()) {
        if (target.linux_node == observed)
            return true;
    }
    return false;
}

} // namespace

auto detail::PlacementEvaluatedRegionAccess::compare_exact_ratios(const ExactRatio& left,
                                                                  const ExactRatio& right) noexcept
    -> detail::ExactRatioOrdering {
    auto left_numerator = left.numerator();
    auto left_denominator = left.denominator();
    auto right_numerator = right.numerator();
    auto right_denominator = right.denominator();
    bool inverted = false;

    const auto ordering = [](std::strong_ordering value,
                             bool invert) noexcept -> detail::ExactRatioOrdering {
        if (value == std::strong_ordering::equal)
            return detail::ExactRatioOrdering::equal;
        const bool less = value == std::strong_ordering::less;
        return less != invert ? detail::ExactRatioOrdering::less
                              : detail::ExactRatioOrdering::greater;
    };

    while (true) {
        const auto left_quotient = left_numerator / left_denominator;
        const auto right_quotient = right_numerator / right_denominator;
        if (left_quotient != right_quotient)
            return ordering(left_quotient <=> right_quotient, inverted);

        const auto left_remainder = left_numerator % left_denominator;
        const auto right_remainder = right_numerator % right_denominator;
        if (left_remainder == 0U || right_remainder == 0U)
            return ordering(left_remainder <=> right_remainder, inverted);

        left_numerator = left_denominator;
        left_denominator = left_remainder;
        right_numerator = right_denominator;
        right_denominator = right_remainder;
        inverted = !inverted;
    }
}

auto detail::PlacementEvaluatedRegionAccess::evaluate_region_placement(
    const RegionRequest& request, const RegionPlacementObservation& observation) noexcept
    -> std::expected<detail::PlacementEvaluationResult, core::Error> {
    try {
        const auto fail = [](std::string_view code, std::string_view classification)
            -> std::expected<detail::PlacementEvaluationResult, core::Error> {
            return std::unexpected{evaluation_error(code, classification)};
        };

        const auto total_pages = observation.total_pages();
        const auto resident_pages = observation.resident_pages();
        const auto unresolved_pages = observation.unresolved_pages();
        if (total_pages == 0U)
            return fail("memory.region_placement_evaluation.invalid_argument",
                        "denominator_invalid");
        std::uint64_t observed_total{};
        if (!checked_add(resident_pages, unresolved_pages, observed_total) ||
            observed_total != total_pages)
            return fail("memory.region_placement_evaluation.invariant_failed",
                        "observation_inconsistent");

        std::uint64_t resident_sum{};
        std::uint64_t requested_pages{};
        std::uint64_t unexpected_pages{};
        std::optional<core::LinuxNumaNodeId> prior_node;
        for (const auto& node_count : observation.resident_page_counts()) {
            if (node_count.page_count() == 0U ||
                (prior_node && node_count.linux_node() <= *prior_node))
                return fail("memory.region_placement_evaluation.invariant_failed",
                            "observation_inconsistent");
            prior_node = node_count.linux_node();
            if (!checked_add(resident_sum, node_count.page_count(), resident_sum))
                return fail("memory.region_placement_evaluation.arithmetic_failed",
                            "resident_count_inconsistent");
            const auto is_requested = requested_node(request, node_count.linux_node());
            auto& destination = is_requested ? requested_pages : unexpected_pages;
            if (!checked_add(destination, node_count.page_count(), destination))
                return fail("memory.region_placement_evaluation.arithmetic_failed",
                            is_requested ? "requested_count_overflow"
                                         : "unexpected_count_overflow");
        }
        if (resident_sum != resident_pages)
            return fail("memory.region_placement_evaluation.invariant_failed",
                        "resident_count_inconsistent");
        std::uint64_t classified_resident{};
        if (!checked_add(requested_pages, unexpected_pages, classified_resident) ||
            classified_resident != resident_pages)
            return fail("memory.region_placement_evaluation.invariant_failed",
                        "observation_inconsistent");
        std::uint64_t classified_total{};
        if (!checked_add(classified_resident, unresolved_pages, classified_total) ||
            classified_total != total_pages)
            return fail("memory.region_placement_evaluation.invariant_failed",
                        "observation_inconsistent");

        auto ratio = ExactRatio::create(requested_pages, total_pages);
        if (!ratio)
            return fail("memory.region_placement_evaluation.arithmetic_failed",
                        "ratio_construction_failed");
        const auto comparison = compare_exact_ratios(*ratio, request.placement_threshold());
        const auto state = comparison == detail::ExactRatioOrdering::less
                               ? PlacementQualificationState::not_qualified
                               : PlacementQualificationState::qualified;
        return detail::PlacementEvaluationResult{
            .evaluation = evaluation(requested_pages, unexpected_pages, unresolved_pages, *ratio),
            .qualification_state = state};
    } catch (const std::exception&) {
        return std::unexpected{evaluation_error(
            "memory.region_placement_evaluation.contained_exception", "contained_exception")};
    } catch (...) {
        return std::unexpected{
            evaluation_error("memory.region_placement_evaluation.contained_unknown_failure",
                             "contained_unknown_failure")};
    }
}

auto PlacementEvaluatedRegion::create(PlacementObservedRegion region) noexcept
    -> std::expected<PlacementEvaluatedRegion, RegionPlacementEvaluationFailure> {
    try {
        const auto fail_error = [&region](core::Error error)
            -> std::expected<PlacementEvaluatedRegion, RegionPlacementEvaluationFailure> {
            return std::unexpected{detail::PlacementEvaluatedRegionAccess::failure(
                std::move(error), std::move(region))};
        };
        const auto fail = [&fail_error](std::string_view code, std::string_view classification)
            -> std::expected<PlacementEvaluatedRegion, RegionPlacementEvaluationFailure> {
            return fail_error(evaluation_error(code, classification));
        };

        const auto query_state = region.query_state();
        const auto& observation = region.observation();
        if (query_state == PreRunPlacementQueryState::skipped) {
            if (observation)
                return fail("memory.region_placement_evaluation.invariant_failed",
                            "observation_unexpected");
            if (region.request().verify_before())
                return fail("memory.region_placement_evaluation.invariant_failed",
                            "qualification_owner_invalid");
            return detail::PlacementEvaluatedRegionAccess::success(
                std::move(region), PlacementQualificationState::skipped, std::nullopt);
        }

        if (query_state != PreRunPlacementQueryState::observed)
            return fail("memory.region_placement_evaluation.invalid_argument",
                        "query_state_invalid");
        if (!observation)
            return fail("memory.region_placement_evaluation.invariant_failed",
                        "observation_missing");
        if (!region.request().verify_before())
            return fail("memory.region_placement_evaluation.invariant_failed",
                        "qualification_owner_invalid");

        auto evaluated = detail::PlacementEvaluatedRegionAccess::evaluate_region_placement(
            region.request(), *observation);
        if (!evaluated)
            return fail_error(std::move(evaluated.error()));
        return detail::PlacementEvaluatedRegionAccess::success(
            std::move(region), evaluated->qualification_state, std::move(evaluated->evaluation));
    } catch (const std::exception&) {
        return std::unexpected{detail::PlacementEvaluatedRegionAccess::failure(
            evaluation_error("memory.region_placement_evaluation.contained_exception",
                             "contained_exception"),
            std::move(region))};
    } catch (...) {
        return std::unexpected{detail::PlacementEvaluatedRegionAccess::failure(
            evaluation_error("memory.region_placement_evaluation.contained_unknown_failure",
                             "contained_unknown_failure"),
            std::move(region))};
    }
}

} // namespace epycbench::memory
