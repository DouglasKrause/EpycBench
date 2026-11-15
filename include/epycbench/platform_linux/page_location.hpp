#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"

#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::platform_linux {

namespace detail {
class NumaAccess;
}

enum class PageQueryStatusKind { resident, unresolved, error };

class PageQueryStatus {
  public:
    [[nodiscard]] static auto resident(core::LinuxNumaNodeId node) -> PageQueryStatus;
    [[nodiscard]] static auto unresolved() -> PageQueryStatus;
    [[nodiscard]] static auto error(int error_number)
        -> std::expected<PageQueryStatus, core::Error>;

    [[nodiscard]] auto kind() const noexcept -> PageQueryStatusKind;
    [[nodiscard]] auto node() const noexcept -> std::optional<core::LinuxNumaNodeId>;
    [[nodiscard]] auto error_number() const noexcept -> std::optional<int>;

  private:
    PageQueryStatus(PageQueryStatusKind kind, std::optional<core::LinuxNumaNodeId> node,
                    std::optional<int> error_number) noexcept;

    PageQueryStatusKind kind_;
    std::optional<core::LinuxNumaNodeId> node_;
    std::optional<int> error_number_;

    friend class detail::NumaAccess;
};

[[nodiscard]] auto query_page_locations(std::span<void* const> pages)
    -> std::expected<std::vector<PageQueryStatus>, core::Error>;

} // namespace epycbench::platform_linux
