#include "epycbench/runtime/pairwise_production_authority.hpp"

#include "core/json_ownership.hpp"
#include "runtime/pairwise_production_admission_access.hpp"

#include <cassert>
#include <limits>
#include <optional>
#include <utility>

namespace epycbench::runtime {
namespace {
auto exhausted_error() -> core::Error {
    auto context = core::detail::json_object<nlohmann::json>([](auto& value) {
        core::detail::set_json(value, "classification", nlohmann::json("request_id_exhausted"));
    });
    core::detail::JsonCleanup context_cleanup{context};
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.production_request_id.allocation_failed"},
            .message = "Production request ID source is exhausted",
            .operation = "allocate_request_id",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
} // namespace

auto ProductionRequestIdSource::allocate() noexcept -> std::expected<ipc::RequestId, core::Error> {
    return allocate_fallible();
}

auto ProductionRequestIdSource::allocate_fallible() -> std::expected<ipc::RequestId, core::Error> {
    std::scoped_lock lock{mutex_};
    if (exhausted_)
        return std::unexpected{exhausted_error()};

    const auto value = next_;
    if (value == std::numeric_limits<std::uint64_t>::max())
        exhausted_ = true;
    else
        ++next_;
    return ipc::RequestId::create(value);
}

struct PairwiseProductionAdmission::State final {
    State(ipc::PlacementRequest first_value, ipc::PlacementRequest second_value)
        : first{std::move(first_value)}, second{std::move(second_value)} {}

    ipc::PlacementRequest first;
    ipc::PlacementRequest second;
    // Declared last so the complete admitted borrow graph is destroyed before its request targets.
    std::optional<AdmittedPairwiseProductionInputs> admitted;
};

PairwiseProductionAdmission::PairwiseProductionAdmission(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}
PairwiseProductionAdmission::PairwiseProductionAdmission(PairwiseProductionAdmission&&) noexcept =
    default;
PairwiseProductionAdmission::~PairwiseProductionAdmission() = default;

auto PairwiseProductionAdmission::first_request() const noexcept
    -> const ipc::PlacementRequest& {
    assert(state_ != nullptr);
    return state_->first;
}
auto PairwiseProductionAdmission::second_request() const noexcept
    -> const ipc::PlacementRequest& {
    assert(state_ != nullptr);
    return state_->second;
}
auto PairwiseProductionAdmission::inputs() const noexcept
    -> const AdmittedPairwiseProductionInputs& {
    assert(state_ != nullptr && state_->admitted.has_value());
    return *state_->admitted;
}
auto PairwiseProductionAdmission::has_value() const noexcept -> bool { return state_ != nullptr; }

auto detail::PairwiseProductionAdmissionAccess::create(
    ipc::PlacementRequest first, ipc::PlacementRequest second, ipc::RequestId first_id,
    ipc::RequestId second_id, std::uint64_t resolved_comparison_order_seed,
    StatisticsComparisonPolicySource statistics_source, const RunnerContext& context) noexcept
    -> std::expected<PairwiseProductionAdmission, core::Error> {
    try {
        auto state = std::make_unique<PairwiseProductionAdmission::State>(std::move(first),
                                                                          std::move(second));
        auto first_admitted = admit_placement_request(first_id, state->first, context);
        if (!first_admitted)
            return std::unexpected{first_admitted.error()};
        auto second_admitted = admit_placement_request(second_id, state->second, context);
        if (!second_admitted)
            return std::unexpected{second_admitted.error()};
        auto admitted = AdmittedPairwiseProductionInputs::create(
            std::move(*first_admitted), std::move(*second_admitted),
            resolved_comparison_order_seed, std::move(statistics_source));
        if (!admitted)
            return std::unexpected{admitted.error()};
        state->admitted.emplace(std::move(*admitted));
        return PairwiseProductionAdmission{std::move(state)};
    } catch (const std::exception&) {
        return std::unexpected{core::Error{
            .category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.pairwise_production_admission.contained_failure"},
            .message = "Pairwise production admission construction failed",
            .operation = "construct_pairwise_production_admission",
            .os_error = std::nullopt,
            .context = {{"classification", "contained_exception"}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command}};
    } catch (...) {
        return std::unexpected{core::Error{
            .category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.pairwise_production_admission.contained_failure"},
            .message = "Pairwise production admission construction failed",
            .operation = "construct_pairwise_production_admission",
            .os_error = std::nullopt,
            .context = {{"classification", "contained_unknown_failure"}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command}};
    }
}

} // namespace epycbench::runtime
