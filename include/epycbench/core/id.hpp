#pragma once

#include <compare>
#include <concepts>
#include <cstdint>

namespace epycbench::core {

template <typename Tag, std::unsigned_integral Representation> class StrongId {
  public:
    using representation_type = Representation;

    constexpr explicit StrongId(Representation value) noexcept : value_{value} {}

    [[nodiscard]] constexpr auto value() const noexcept -> Representation { return value_; }

    constexpr auto operator<=>(const StrongId&) const noexcept = default;

  private:
    Representation value_;
};

namespace detail {

struct LinuxCpuTag;
struct LinuxNumaNodeTag;
struct TopologyMachineTag;
struct ProcessingUnitTag;
struct CoreTag;
struct PackageTag;
struct DieTag;
struct ArchitectureGroupTag;
struct CacheTag;
struct NumaNodeTag;
struct WorkerTag;
struct ExperimentTag;
struct ObservationTag;

} // namespace detail

using LinuxCpuId = StrongId<detail::LinuxCpuTag, std::uint32_t>;
using LinuxNumaNodeId = StrongId<detail::LinuxNumaNodeTag, std::uint32_t>;
using TopologyMachineId = StrongId<detail::TopologyMachineTag, std::uint32_t>;
using ProcessingUnitId = StrongId<detail::ProcessingUnitTag, std::uint32_t>;
using CoreId = StrongId<detail::CoreTag, std::uint32_t>;
using PackageId = StrongId<detail::PackageTag, std::uint32_t>;
using DieId = StrongId<detail::DieTag, std::uint32_t>;
using ArchitectureGroupId = StrongId<detail::ArchitectureGroupTag, std::uint32_t>;
using CacheId = StrongId<detail::CacheTag, std::uint32_t>;
using NumaNodeId = StrongId<detail::NumaNodeTag, std::uint32_t>;
using WorkerId = StrongId<detail::WorkerTag, std::uint32_t>;
using ExperimentId = StrongId<detail::ExperimentTag, std::uint64_t>;
using ObservationId = StrongId<detail::ObservationTag, std::uint64_t>;

} // namespace epycbench::core
