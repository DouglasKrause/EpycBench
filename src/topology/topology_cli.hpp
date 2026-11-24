#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/invariants.hpp"
#include "epycbench/topology/reconciliation.hpp"
#include "epycbench/topology/reference_fixture.hpp"
#include "epycbench/topology/serialization.hpp"
#include "epycbench/topology/source_evidence.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>

namespace epycbench::topology::cli {

enum class ExitCode : int {
    success = 0,
    unexpected = 1,
    usage = 2,
    filesystem = 3,
    discovery = 4,
    topology = 5,
};

struct CliFailure {
    std::string code;
    std::string classification;
    ExitCode exit_code{ExitCode::unexpected};
};

struct Environment {
    std::function<std::expected<TopologySourceEvidence, core::Error>()> discover_hwloc;
    std::function<std::expected<TopologySourceEvidence, core::Error>()> discover_linux;
    std::function<std::expected<ReconciliationResult, core::Error>(const TopologySourceEvidence&,
                                                                   const TopologySourceEvidence&)>
        reconcile;
    std::function<std::expected<TopologySnapshot, ReferenceFixtureLoadError>()> load_fixture;
    std::function<ValidationReport(const TopologySnapshot&)> validate;
    std::function<std::expected<StableTopologyIds, core::Error>(const TopologySnapshot&)>
        derive_ids;
    std::function<std::expected<TopologyFingerprints, core::Error>(const TopologySnapshot&,
                                                                   const StableTopologyIds&)>
        derive_fingerprints;
    std::function<std::expected<SerializedTopology, core::Error>(
        const TopologySnapshot&, const StableTopologyIds&, const TopologyFingerprints&)>
        serialize;
    std::function<std::expected<LoadedTopology, core::Error>(std::string_view,
                                                             TopologyJsonInputPolicy)>
        deserialize;
    std::function<std::expected<std::string, CliFailure>(const std::filesystem::path&, std::size_t)>
        read_bounded_file;
    [[nodiscard]] auto read_file(const std::filesystem::path& path) const {
        return read_bounded_file(path, 64U * 1024U * 1024U);
    }
    std::function<std::expected<void, CliFailure>(const std::filesystem::path&, std::string_view,
                                                  bool)>
        publish_file;
};

[[nodiscard]] auto make_production_environment() -> Environment;

[[nodiscard]] auto run(int argc, char* argv[], Environment& environment, std::ostream& output,
                       std::ostream& error) -> int;

} // namespace epycbench::topology::cli
