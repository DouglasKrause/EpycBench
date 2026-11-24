#pragma once

#include "epycbench/monitoring/package_energy_evidence.hpp"
#include "epycbench/topology/serialization.hpp"
#include "epycbench/core/error.hpp"
#include "results/package_result.hpp"

namespace epycbench::cli::run_energy {

inline constexpr std::size_t maximum_bytes = 16384U;

enum class Unavailable { package, device, access, ambiguous_device, read, clock, bracket, supplied_workers };

struct IncrementBound {
    std::string physical_topology;
    std::uint32_t linux_package_number;
    std::string boot_sha256;
    std::string common_build_id;
    std::string msr_build_id;
    std::uint32_t unit_exponent;
    std::uint64_t valid_from_boottime_ns;
    std::uint64_t valid_until_boottime_ns;
    std::uint64_t maximum_interval_ns;
    platform_linux::Microjoules maximum_increment;
    std::string source_sha256;
    std::string review_sha256;
};

struct Qualification {
    std::optional<platform_linux::PackageEnergySourceSnapshot> before;
    std::optional<platform_linux::PackageEnergySourceSnapshot> after;
    std::optional<IncrementBound> bound;
};

struct Report {
    std::uint32_t linux_package_number;
    std::string physical_topology;
    std::optional<core::PackageId> package;
    std::optional<std::string> stable_package;
    std::optional<std::string> powercap_zone;
    std::optional<platform_linux::RawMonotonicTimestamp> run_started;
    std::optional<platform_linux::RawMonotonicTimestamp> run_finished;
    std::optional<platform_linux::PackageEnergyReading> before;
    std::optional<platform_linux::PackageEnergyReading> after;
    std::optional<Unavailable> unavailable;
    std::optional<Qualification> qualification{};
};

enum class ResetModel { accumulation_or_reset_to_zero, unknown };

struct EstimateOptions {
    std::uint64_t assumed_max_package_power_milliwatts;
    std::uint64_t assumed_max_counter_delay_ns;
    ResetModel reset_model;
};

class Capture {
  public:
    [[nodiscard]] static auto begin(const topology::LoadedTopology&, std::uint32_t linux_package_number,
        std::optional<IncrementBound> = {}, bool supplied = false) -> Capture;
    void finish();
    [[nodiscard]] auto report() const noexcept -> const Report& { return report_; }
  private:
    static auto begin(const topology::TopologySnapshot&, const topology::StableTopologyIds&,
        const topology::TopologyFingerprints&, std::uint32_t, std::optional<IncrementBound>, bool) -> Capture;
    explicit Capture(std::uint32_t number) : report_{.linux_package_number = number,
        .physical_topology = {}, .package = {}, .stable_package = {}, .powercap_zone = {},
        .run_started = {}, .run_finished = {},
        .before = {}, .after = {}, .unavailable = Unavailable::package} {}
    Report report_;
    std::optional<platform_linux::PackageEnergyZone> zone_;
};

[[nodiscard]] auto read_increment_bound(const std::filesystem::path&)
    -> std::expected<IncrementBound, core::Error>;

[[nodiscard]] auto save(const Report&, const results::PackageResult&, const std::filesystem::path& result_path)
    -> std::expected<std::filesystem::path, core::Error>;
[[nodiscard]] auto load(const std::filesystem::path& result_path, const results::PackageResult&)
    -> std::expected<std::string, core::Error>;
[[nodiscard]] auto load_estimate(const std::filesystem::path& result_path, const results::PackageResult&,
    const EstimateOptions&) -> std::expected<std::string, core::Error>;

} // namespace epycbench::cli::run_energy
