#pragma once

#include "scenario/package_scenario.hpp"

#include <iosfwd>

namespace epycbench::cli {
auto author_package_scenario(const kernel::packages::Catalog&,
                             const scenario::ResourceResolutionContext&,
                             const std::optional<std::string>& destination, std::istream&,
                             std::ostream&, std::ostream&) -> int;
auto author_package_sweep(const nlohmann::json& document, bool edit,
                          const scenario::ResourceResolutionContext&, std::istream&, std::ostream&,
                          std::ostream&) -> int;
} // namespace epycbench::cli
