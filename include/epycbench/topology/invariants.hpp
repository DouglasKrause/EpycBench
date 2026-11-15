#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/model.hpp"

#include <vector>

namespace epycbench::topology {

struct ValidationReport {
    std::vector<core::Error> diagnostics;

    [[nodiscard]] auto valid() const noexcept -> bool { return diagnostics.empty(); }
    [[nodiscard]] auto has_errors() const noexcept -> bool { return !diagnostics.empty(); }
};

[[nodiscard]] auto validate_topology(const TopologySnapshot& topology) -> ValidationReport;

} // namespace epycbench::topology
