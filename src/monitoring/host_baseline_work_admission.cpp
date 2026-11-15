#include "monitoring/host_baseline_work_admission.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace work = platform_linux::host_work;
HostBaselineWorkAdmission::HostBaselineWorkAdmission(HostBaselinePreparation preparation,
    work::HeadroomReceipt headroom, work::LimitDemand demand) noexcept
    : preparation_{std::move(preparation)}, headroom_{headroom}, demand_{demand} {}
auto HostBaselineWorkAdmission::active() const noexcept -> bool { return preparation_.has_value(); }
auto HostBaselineWorkAdmission::headroom() const noexcept -> std::optional<work::HeadroomReceipt> {
    return active() ? std::optional{headroom_} : std::nullopt;
}
auto HostBaselineWorkAdmission::limit_demand() const noexcept -> std::optional<work::LimitDemand> {
    return active() ? std::optional{demand_} : std::nullopt;
}
auto HostBaselineWorkAdmission::take_preparation() noexcept
    -> std::expected<HostBaselinePreparation, work::Reason> {
    if (!active()) return std::unexpected{work::Reason::invalid_state};
    return std::move(preparation_);
}
auto admit_host_baseline_work(const RunnerContextAcquisitionSession& session,
    const MonitoringHostQualificationPlan& plan, const HostQualificationCalibrationInputs& inputs,
    core::ByteCount stack, std::size_t descriptors, const work::ReadOperations& operations) noexcept
    -> std::expected<HostBaselineWorkAdmission, HostBaselineWorkFailure> {
    auto prepared = prepare_host_baseline(session, plan, inputs);
    if (!prepared) return std::unexpected{HostBaselineWorkFailure{prepared.error(), std::nullopt}};
    const auto reject = [](work::Reason reason) {
        return std::unexpected{HostBaselineWorkFailure{std::nullopt, work::Failure{reason}}};
    };
    const auto summary = *prepared->resources();
    const auto latency = summary.latency_mapping.value();
    const auto verification = summary.retained_verification.value();
    if (verification > UINT64_MAX - latency) return reject(work::Reason::arithmetic);
    const auto peak = std::max(summary.bandwidth_mapping.value(), latency + verification);
    constexpr std::uint64_t reserve = UINT64_C(4294967296);
    if (peak > UINT64_MAX - reserve) return reject(work::Reason::arithmetic);
    const work::LimitDemand demand{core::ByteCount::from_base_units(peak + reserve), stack, descriptors};
    // Pure preflight; this does not apply limits to this process.
    if (const auto checked = work::check_limit_demand(demand); !checked)
        return reject(checked.error());
    std::array<core::LinuxNumaNodeId, 8> nodes{
        core::LinuxNumaNodeId{0}, core::LinuxNumaNodeId{0}, core::LinuxNumaNodeId{0}, core::LinuxNumaNodeId{0},
        core::LinuxNumaNodeId{0}, core::LinuxNumaNodeId{0}, core::LinuxNumaNodeId{0}, core::LinuxNumaNodeId{0}};
    std::size_t used{};
    for (const auto& binding : plan.bandwidth_memory_bindings()) {
        const auto selected = std::span{nodes}.first(used);
        if (std::ranges::find(selected, binding.linux_node) != selected.end()) continue;
        if (used == nodes.size()) return reject(work::Reason::capacity);
        nodes[used++] = binding.linux_node;
    }
    auto headroom = work::read_headroom(std::span{nodes}.first(used), operations);
    if (!headroom) return std::unexpected{HostBaselineWorkFailure{std::nullopt, headroom.error()}};
    return HostBaselineWorkAdmission{std::move(*prepared), *headroom, demand};
}
} // namespace epycbench::monitoring::qualification::detail
