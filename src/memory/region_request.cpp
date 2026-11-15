#include "epycbench/memory/region_request.hpp"

#include <exception>
#include <numeric>
#include <string>
#include <string_view>

namespace epycbench::memory {
namespace {

auto region_error(std::string_view code, std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Memory region request construction failed",
            .operation = "memory.region_request.create",
            .os_error = std::nullopt,
            .context = nlohmann::json{{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto invalid(std::string_view classification) -> std::unexpected<core::Error> {
    return std::unexpected{
        region_error("memory.region_request.invalid_argument", classification)};
}

template <typename Projection>
auto contains_duplicate(std::span<const RegionNumaTarget> targets, Projection project) -> bool {
    for (std::size_t left = 0; left < targets.size(); ++left) {
        for (std::size_t right = left + 1; right < targets.size(); ++right) {
            if (project(targets[left]) == project(targets[right])) return true;
        }
    }
    return false;
}

} // namespace

auto ExactRatio::create(std::uint64_t numerator, std::uint64_t denominator) noexcept
    -> std::expected<ExactRatio, core::Error> {
    try {
        if (denominator == 0U || numerator > denominator) return invalid("invalid_threshold");
        const auto divisor = std::gcd(numerator, denominator);
        return ExactRatio{numerator / divisor, denominator / divisor};
    } catch (const std::exception&) {
        return std::unexpected{region_error("memory.region_request.contained_exception",
                                            "contained_exception")};
    } catch (...) {
        return std::unexpected{
            region_error("memory.region_request.contained_unknown_failure",
                         "contained_unknown_failure")};
    }
}

auto RegionRequest::create(RegionRequestInput input) noexcept
    -> std::expected<RegionRequest, core::Error> {
    try {
        if (input.placement_policy != PlacementPolicy::bind)
            return invalid("unsupported_placement_policy");
        if (input.page_mode != PageMode::base) return invalid("unsupported_page_mode");
        if (input.requested_bytes.value() == 0U) return invalid("zero_requested_bytes");
        if (input.page_size.value() == 0U) return invalid("zero_page_size");
        if (input.requested_bytes.value() % input.page_size.value() != 0U)
            return invalid("page_alignment_mismatch");
        if (input.numa_targets.empty()) return invalid("missing_numa_target");

        const std::span<const RegionNumaTarget> targets{input.numa_targets};
        if (contains_duplicate(targets, [](const auto& target) -> const auto& {
                return target.stable_node;
            }))
            return invalid("duplicate_stable_numa_target");
        if (contains_duplicate(targets,
                               [](const auto& target) { return target.linux_node; }))
            return invalid("duplicate_linux_numa_target");
        if (contains_duplicate(targets,
                               [](const auto& target) { return target.topology_node; }))
            return invalid("duplicate_topology_numa_target");

        if (input.placement_threshold.denominator() == 0U ||
            input.placement_threshold.numerator() > input.placement_threshold.denominator())
            return std::unexpected{region_error("memory.region_request.invariant_failed",
                                                "invalid_threshold")};

        return RegionRequest{std::move(input)};
    } catch (const std::exception&) {
        return std::unexpected{region_error("memory.region_request.contained_exception",
                                            "contained_exception")};
    } catch (...) {
        return std::unexpected{
            region_error("memory.region_request.contained_unknown_failure",
                         "contained_unknown_failure")};
    }
}

} // namespace epycbench::memory
