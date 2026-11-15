#include "monitoring/host_baseline_preparation.hpp"

#include "epycbench/monitoring/host_q0b_v2_projection.hpp"
#include "memory/region_mapping_access.hpp"
#include "monitoring/host_qualification_plan_access.hpp"
#include "topology/fingerprint_projection.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Failure = HostBaselinePreparationFailure;
thread_local bool fail_owner_allocation{};

auto valid_limits(HostBaselinePreparationLimits limits) noexcept -> bool {
    const HostBaselinePreparationLimits maximum;
    return limits.bandwidth_workers <= maximum.bandwidth_workers &&
        limits.peak_mapping <= maximum.peak_mapping &&
        limits.retained_verification <= maximum.retained_verification &&
        limits.projection_bytes != 0 && limits.projection_bytes <= maximum.projection_bytes;
}
auto add(std::uint64_t left, std::uint64_t right) noexcept
    -> std::expected<std::uint64_t, Failure> {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return std::unexpected{Failure::native_arithmetic};
    return left + right;
}
auto mapping_sum(std::span<const HostBaselineRegionDemand> regions)
    -> std::expected<std::uint64_t, Failure> {
    std::uint64_t sum{};
    for (const auto& region : regions) {
        if (region.requested.value() == 0 || region.page.value() == 0)
            return std::unexpected{Failure::invalid_input};
        const auto mapped = memory::detail::RegionMappingAccess::calculate_mapping_bytes(
            region.requested, region.page);
        if (!mapped) {
            const auto& code = mapped.error().code.value();
            return std::unexpected{code == "memory.region_mapping.size_overflow" ||
                code == "memory.region_mapping.native_size_unrepresentable"
                    ? Failure::native_arithmetic : Failure::contained_exception};
        }
        const auto next = add(sum, *mapped);
        if (!next) return std::unexpected{next.error()};
        sum = *next;
    }
    return sum;
}
} // namespace

auto checked_host_baseline_payload_bytes(std::uint64_t count, std::uint64_t element_bytes) noexcept
    -> std::expected<std::uint64_t, Failure> {
    if (count == 0 || element_bytes == 0) return std::unexpected{Failure::invalid_input};
    if (count > std::numeric_limits<std::uint64_t>::max() / element_bytes)
        return std::unexpected{Failure::native_arithmetic};
    return count * element_bytes;
}

auto calculate_host_baseline_latency_verification(std::uint64_t nodes) noexcept
    -> std::expected<std::uint64_t, Failure> {
    if (nodes == 0) return std::unexpected{Failure::invalid_input};
    if (nodes > std::numeric_limits<std::size_t>::max() ||
        nodes > std::vector<std::uint64_t>{}.max_size() ||
        nodes > std::vector<std::size_t>{}.max_size())
        return std::unexpected{Failure::native_arithmetic};
    const auto permutation = checked_host_baseline_payload_bytes(nodes, sizeof(std::uint64_t));
    if (!permutation) return std::unexpected{permutation.error()};
    const auto inverse = checked_host_baseline_payload_bytes(nodes, sizeof(std::size_t));
    if (!inverse) return std::unexpected{inverse.error()};
    return add(*permutation, *inverse);
}

auto calculate_host_baseline_resources(
    std::span<const HostBaselineRegionDemand> bandwidth_first,
    std::span<const HostBaselineRegionDemand> bandwidth_second,
    std::span<const HostBaselineRegionDemand> latency_first,
    std::span<const HostBaselineRegionDemand> latency_second,
    HostBaselinePreparationLimits limits) noexcept
    -> std::expected<HostBaselineResourceSummary, Failure> {
    try {
        if (!valid_limits(limits) || bandwidth_first.empty() ||
            bandwidth_first.size() != bandwidth_second.size() ||
            latency_first.size() != 1 || latency_second.size() != 1)
            return std::unexpected{Failure::invalid_input};
        if (bandwidth_first.size() > limits.bandwidth_workers)
            return std::unexpected{Failure::capacity};
        const auto bw1 = mapping_sum(bandwidth_first), bw2 = mapping_sum(bandwidth_second);
        const auto lat1 = mapping_sum(latency_first), lat2 = mapping_sum(latency_second);
        if (!bw1) return std::unexpected{bw1.error()};
        if (!bw2) return std::unexpected{bw2.error()};
        if (!lat1) return std::unexpected{lat1.error()};
        if (!lat2) return std::unexpected{lat2.error()};
        const auto bandwidth = add(*bw1, *bw2), latency = add(*lat1, *lat2);
        if (!bandwidth) return std::unexpected{bandwidth.error()};
        if (!latency) return std::unexpected{latency.error()};
        const auto verification1 = calculate_host_baseline_latency_verification(
            latency_first[0].requested.value() / 64U);
        const auto verification2 = calculate_host_baseline_latency_verification(
            latency_second[0].requested.value() / 64U);
        if (!verification1) return std::unexpected{verification1.error()};
        if (!verification2) return std::unexpected{verification2.error()};
        const auto verification = add(*verification1, *verification2);
        if (!verification) return std::unexpected{verification.error()};
        const auto peak = std::max(*bandwidth, *latency);
        if (peak > limits.peak_mapping.value() || *verification > limits.retained_verification.value())
            return std::unexpected{Failure::capacity};
        return HostBaselineResourceSummary{core::ByteCount::from_base_units(*bandwidth),
            core::ByteCount::from_base_units(*latency), core::ByteCount::from_base_units(peak),
            core::ByteCount::from_base_units(*verification), bandwidth_first.size(), 1,
            bandwidth_first.size()};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (...) {
        return std::unexpected{Failure::contained_exception};
    }
}

struct HostBaselinePreparation::State final {
    std::string projection;
    core::Sha256Digest sha256;
    HostBaselineResourceSummary summary;
};
HostBaselinePreparation::HostBaselinePreparation(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}
HostBaselinePreparation::HostBaselinePreparation(HostBaselinePreparation&&) noexcept = default;
auto HostBaselinePreparation::operator=(HostBaselinePreparation&&) noexcept
    -> HostBaselinePreparation& = default;
HostBaselinePreparation::~HostBaselinePreparation() noexcept = default;
auto HostBaselinePreparation::has_value() const noexcept -> bool { return state_ != nullptr; }
auto HostBaselinePreparation::physical_projection() const noexcept -> std::string_view {
    return state_ ? std::string_view{state_->projection} : std::string_view{};
}
auto HostBaselinePreparation::physical_sha256() const noexcept -> std::optional<core::Sha256Digest> {
    return state_ ? std::optional{state_->sha256} : std::nullopt;
}
auto HostBaselinePreparation::resources() const noexcept -> std::optional<HostBaselineResourceSummary> {
    return state_ ? std::optional{state_->summary} : std::nullopt;
}

auto prepare_host_baseline(const RunnerContextAcquisitionSession& session,
    const MonitoringHostQualificationPlan& plan, const HostQualificationCalibrationInputs& inputs,
    HostBaselinePreparationLimits limits) noexcept
    -> std::expected<HostBaselinePreparation, Failure> {
    try {
        if (!inputs.bandwidth().has_value() || !inputs.latency().has_value() ||
            !valid_limits(limits) || plan.bandwidth_workers().empty() ||
            plan.latency_workers().size() != 1)
            return std::unexpected{Failure::invalid_input};
        const auto& bw = inputs.bandwidth();
        const auto& lat = inputs.latency();
        if (plan.bandwidth_workers().size() > limits.bandwidth_workers ||
            bw.first().workers().size() > limits.bandwidth_workers ||
            bw.second().workers().size() > limits.bandwidth_workers)
            return std::unexpected{Failure::capacity};
        if (bw.first().workers().empty() || bw.second().workers().empty() ||
            lat.first().workers().size() != 1 || lat.second().workers().size() != 1)
            return std::unexpected{Failure::invalid_input};
        const auto binding = detail_HostQualificationPlanAccess::binding(session);
        const auto context = session.runner_context();
        if (!binding || !context) return std::unexpected{Failure::invalid_input};
        if (*binding != detail_HostQualificationPlanAccess::plan_binding(plan) ||
            &bw.first().context() != &context->get() || &bw.second().context() != &context->get() ||
            &lat.first().context() != &context->get() || &lat.second().context() != &context->get())
            return std::unexpected{Failure::foreign_input};
        std::array<HostBaselineRegionDemand, 8> bw1{}, bw2{};
        const auto copy = [](const auto& admitted, auto& demands) {
            for (std::size_t index = 0; index < admitted.workers().size(); ++index) {
                const auto& region = admitted.workers()[index].request().region();
                demands[index] = {region.bytes(), region.page_size_bytes()};
            }
        };
        copy(bw.first(), bw1); copy(bw.second(), bw2);
        std::array<HostBaselineRegionDemand, 1> lat1{}, lat2{};
        copy(lat.first(), lat1); copy(lat.second(), lat2);
        const auto resources = calculate_host_baseline_resources(
            std::span{bw1}.first(bw.first().workers().size()),
            std::span{bw2}.first(bw.second().workers().size()), lat1, lat2, limits);
        if (!resources) return std::unexpected{resources.error()};
        for (const auto family : {HostQ0bFamily::bandwidth, HostQ0bFamily::latency}) {
            for (const auto variant : {runtime::PairwiseVariant::first, runtime::PairwiseVariant::second}) {
                const auto facts = project_monitoring_host_admitted_production_facts(
                    session, plan, family == HostQ0bFamily::bandwidth ? bw : lat, family, variant);
                if (!facts) return std::unexpected{Failure::invalid_input};
            }
        }
        auto projection = topology::detail::canonical_physical_fingerprint_projection(
            context->get().topology(), context->get().stable_ids());
        if (!projection) return std::unexpected{Failure::projection};
        if (projection->size() > limits.projection_bytes) return std::unexpected{Failure::capacity};
        const auto digest = core::sha256(*projection);
        if (digest != context->get().fingerprints().physical.digest() ||
            digest != plan.physical_fingerprint().digest())
            return std::unexpected{Failure::digest};
        if (std::exchange(fail_owner_allocation, false)) throw std::bad_alloc{};
        return HostBaselinePreparation{std::make_unique<HostBaselinePreparation::State>(
            HostBaselinePreparation::State{std::move(*projection), digest, *resources})};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (...) {
        return std::unexpected{Failure::contained_exception};
    }
}
void fail_next_host_baseline_owner_allocation_for_test() noexcept { fail_owner_allocation = true; }
} // namespace epycbench::monitoring::qualification::detail
