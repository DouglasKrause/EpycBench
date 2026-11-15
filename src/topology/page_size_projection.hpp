#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/model.hpp"

#include <algorithm>
#include <expected>
#include <span>
#include <vector>

namespace epycbench::topology::detail {

[[nodiscard]] inline auto derive_machine_page_sizes(std::span<const NumaNode> nodes)
    -> std::expected<std::vector<core::ByteCount>, core::Error> {
    std::vector<core::ByteCount> result;
    for (const auto& node : nodes) {
        for (const auto page_size : node.page_sizes) {
            if (page_size.value() == 0U) {
                return std::unexpected{core::Error{
                    .category = core::ErrorCategory::topology,
                    .code = core::StableErrorCode{"topology.page_sizes.zero_capability"},
                    .message = "Page-size capabilities must be greater than zero",
                    .operation = "derive_machine_page_sizes",
                    .os_error = std::nullopt,
                    .context = nlohmann::json::object(),
                    .severity = core::ErrorSeverity::error,
                    .continuation = core::ContinuationPolicy::abort_command,
                }};
            }
            result.push_back(page_size);
        }
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace epycbench::topology::detail
