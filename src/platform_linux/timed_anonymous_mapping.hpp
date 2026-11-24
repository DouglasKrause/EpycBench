#pragma once

#include "platform_linux/clock_operations.hpp"
#include "platform_linux/mapping_operations.hpp"

#include <optional>

namespace epycbench::platform_linux::detail {

using MappingClockRead = std::expected<RawMonotonicTimestamp, core::Error>;
struct TimedAnonymousMapping {
    MappingClockRead start;
    std::optional<MappingClockRead> end{};
    std::optional<std::expected<AnonymousMapping, core::Error>> request{};
};

// The caller checks the nonzero extent. The end clock precedes interpretation
// of the raw return, construction of the owning handle and any cleanup.
[[nodiscard]] auto time_anonymous_mapping(std::size_t length, const MappingOperations& mapping,
                                          const ClockOperations& clock) -> TimedAnonymousMapping;

} // namespace epycbench::platform_linux::detail
