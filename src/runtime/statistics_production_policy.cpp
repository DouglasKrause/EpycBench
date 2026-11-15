#include "epycbench/runtime/statistics_production_policy.hpp"
#include "pairwise_v2_contextual_admission_classification.hpp"

#include "epycbench/core/seed.hpp"

#include <cmath>
#include <exception>
#include <string>
#include <utility>

namespace epycbench::runtime {
namespace {

auto policy_error(std::string code, std::string classification,
                  const core::Error* underlying = nullptr) -> core::Error {
    nlohmann::json context{{"classification", std::move(classification)}};
    if (underlying != nullptr) context["underlying_code"] = underlying->code.value();
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Statistics production policy projection failed",
            .operation = "statistics_policy_projection",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto invalid_input(std::string classification) -> core::Error {
    return policy_error("runtime.statistics_policy.invalid_input", std::move(classification));
}

auto conversion_failed(std::string classification) -> core::Error {
    return policy_error("runtime.statistics_policy.conversion_failed",
                        std::move(classification));
}

auto to_binary64(const scenario::ExactRatio& ratio, std::string classification)
    -> std::expected<double, core::Error> {
    const auto numerator = ratio.numerator();
    const auto denominator = ratio.denominator();
    const double nd = static_cast<double>(numerator);
    const double dd = static_cast<double>(denominator);
    const double value = nd / dd;
    if (denominator == 0U || !std::isfinite(nd) || !std::isfinite(dd) ||
        !std::isfinite(value) || value < 0.0 || value > 1.0)
        return std::unexpected{conversion_failed(std::move(classification))};
    if (numerator == 0U) {
        if (value != 0.0 || std::signbit(value))
            return std::unexpected{conversion_failed(std::move(classification))};
        return 0.0;
    }
    if (numerator == denominator) {
        if (value != 1.0)
            return std::unexpected{conversion_failed(std::move(classification))};
        return 1.0;
    }
    if (!(value > 0.0 && value < 1.0))
        return std::unexpected{conversion_failed(std::move(classification))};
    return value;
}

auto statistics_rejected(std::string classification, const core::Error& underlying)
    -> core::Error {
    return policy_error("runtime.statistics_policy.statistics_rejected",
                        std::move(classification), &underlying);
}

} // namespace

auto StatisticsComparisonPolicySource::create(
    StatisticsExperimentKind experiment_kind, scenario::ExactRatio comparison_confidence,
    std::uint64_t bootstrap_samples, scenario::ExactRatio global_practical_effect,
    std::optional<scenario::ExactRatio> local_practical_effect) noexcept
    -> std::expected<StatisticsComparisonPolicySource, core::Error> {
    switch (experiment_kind) {
    case StatisticsExperimentKind::comparison:
    case StatisticsExperimentKind::matrix_comparison:
        return StatisticsComparisonPolicySource{
            experiment_kind, comparison_confidence, bootstrap_samples, global_practical_effect,
            std::move(local_practical_effect)};
    }
    return std::unexpected{invalid_input("unknown_experiment_kind")};
}

auto detail::PairwiseV2AdmissionClassificationAccess::create_statistics_policy(
    const StatisticsComparisonPolicySource& source,
    const ipc::PlacementMeasurementInputs& measurement) noexcept
    -> std::expected<StatisticsProductionPolicy,
                     detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
    using Branch = detail::PairwiseV2ContextualAdmissionBranch;
    const auto mismatch = [](core::Error error)
        -> std::expected<StatisticsProductionPolicy,
                         detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::statistics_policy_mismatch, std::move(error)}};
    };
    try {
        const auto comparison_confidence =
            to_binary64(source.comparison_confidence(), "comparison_confidence");
        if (!comparison_confidence) return mismatch(comparison_confidence.error());
        const auto stopping_confidence =
            to_binary64(measurement.stopping_confidence(), "stopping_confidence");
        if (!stopping_confidence) return mismatch(stopping_confidence.error());
        const auto practical_effect =
            to_binary64(source.effective_practical_effect(), "practical_effect");
        if (!practical_effect) return mismatch(practical_effect.error());
        const auto stopping_target =
            to_binary64(measurement.stopping_precision(), "stopping_target");
        if (!stopping_target) return mismatch(stopping_target.error());

        const auto seed = core::derive_seed(
            measurement.root_seed(),
            core::SeedDerivationKey{.domain = core::SeedDomain::bootstrap, .components = {}});
        if (!seed)
            return mismatch(policy_error(
                "runtime.statistics_policy.seed_derivation_failed", "bootstrap_seed",
                &seed.error()));

        const auto comparison = statistics::BootstrapPolicy::create(
            source.bootstrap_samples(), *comparison_confidence, *seed);
        if (!comparison)
            return mismatch(
                statistics_rejected("comparison_bootstrap_policy", comparison.error()));
        const auto stopping = statistics::BootstrapPolicy::create(
            source.bootstrap_samples(), *stopping_confidence, *seed);
        if (!stopping)
            return mismatch(
                statistics_rejected("stopping_bootstrap_policy", stopping.error()));
        const auto threshold = statistics::PracticalEffectThreshold::create(*practical_effect);
        if (!threshold)
            return mismatch(
                statistics_rejected("practical_effect_threshold", threshold.error()));
        const auto target = statistics::RelativeHalfWidthTarget::create(*stopping_target);
        if (!target)
            return mismatch(
                statistics_rejected("relative_half_width_target", target.error()));
        return StatisticsProductionPolicy{*comparison, *stopping, *practical_effect, *threshold,
                                          *target};
    } catch (const std::bad_alloc&) {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::allocation, invalid_input("contained_exception")}};
    } catch (const std::exception&) {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::statistics_contained_exception,
            invalid_input("contained_exception")}};
    } catch (...) {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::statistics_contained_unknown_failure,
            invalid_input("contained_unknown_failure")}};
    }
}

auto StatisticsProductionPolicy::create(
    const StatisticsComparisonPolicySource& source,
    const ipc::PlacementMeasurementInputs& measurement) noexcept
    -> std::expected<StatisticsProductionPolicy, core::Error> {
    auto result = detail::PairwiseV2AdmissionClassificationAccess::create_statistics_policy(
        source, measurement);
    if (!result)
        return std::unexpected{std::move(result.error().legacy_)};
    return std::move(*result);
}

} // namespace epycbench::runtime
