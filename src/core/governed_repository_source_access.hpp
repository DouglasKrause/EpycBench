#pragma once

#include "epycbench/core/governed_repository_file.hpp"

namespace epycbench::monitoring::qualification::detail {
struct HostFrozenSourceLaunchState;
}

namespace epycbench::core::detail {

// Value construction only. The actual source caller must first finish its
// bounded, root-bound native reader, including every consuming close. This
// private access opens no file and creates no input/publication authority.
class GovernedRepositorySourceAccess final {
  private:
    [[nodiscard]] static auto make(std::filesystem::path relative, std::string bytes) noexcept
        -> GovernedRepositoryFile;
    friend struct monitoring::qualification::detail::HostFrozenSourceLaunchState;
};

} // namespace epycbench::core::detail
