#include "epycbench/monitoring/package_energy_evidence.hpp"

namespace epycbench::monitoring {
namespace {
auto failure(std::string_view classification) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{"monitoring.package_energy.invalid_evidence"},
        .message = "Invalid package energy evidence",
        .operation = "package_energy_evidence", .os_error = {},
        .context = {{"classification", classification}},
        .continuation = core::ContinuationPolicy::skip_item}};
}
} // namespace

auto PackageEnergyEvidence::create(core::PackageId package, std::uint32_t linux_package_number,
    platform_linux::PackageEnergyReading before, platform_linux::PackageEnergyReading after,
    platform_linux::RawMonotonicTimestamp enclosing_start,
    platform_linux::RawMonotonicTimestamp enclosing_end)
    -> std::expected<PackageEnergyEvidence, core::Error> {
    if (before.linux_package_number != linux_package_number || after.linux_package_number != linux_package_number)
        return failure("package_binding");
    if (before.advertised_maximum != after.advertised_maximum || before.advertised_maximum.value() == 0U ||
        before.raw_energy > before.advertised_maximum || after.raw_energy > after.advertised_maximum)
        return failure("counter_domain");
    if (before.read_started > before.read_finished || before.read_finished > enclosing_start ||
        enclosing_start >= enclosing_end || enclosing_end > after.read_started || after.read_started > after.read_finished)
        return failure("interval_binding");
    return PackageEnergyEvidence{package, before, after};
}

auto PackageEnergyEvidence::unwrapped_delta(platform_linux::Microjoules maximum_increment) const noexcept
    -> std::optional<PackageEnergyDelta> {
    if (after_.raw_energy < before_.raw_energy) return std::nullopt;
    const auto headroom = before_.advertised_maximum.value() - before_.raw_energy.value();
    const auto difference = after_.raw_energy.value() - before_.raw_energy.value();
    if (maximum_increment.value() >= headroom || difference > maximum_increment.value())
        return std::nullopt;
    return PackageEnergyDelta{platform_linux::Microjoules::from_base_units(difference),
        core::Duration::from_base_units(after_.read_started.nanoseconds() - before_.read_finished.nanoseconds()),
        core::Duration::from_base_units(after_.read_finished.nanoseconds() - before_.read_started.nanoseconds())};
}

} // namespace epycbench::monitoring
