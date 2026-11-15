#include "epycbench/monitoring/monitor_source.hpp"

#include <optional>
#include <string>
#include <utility>

namespace epycbench::monitoring {

MonitoringImplementationId::MonitoringImplementationId(std::string identifier) noexcept
    : identifier_{std::move(identifier)} {}

auto MonitoringImplementationId::create(std::string identifier) -> MonitoringImplementationId {
    assert(!identifier.empty());
    return MonitoringImplementationId{std::move(identifier)};
}

auto MonitoringImplementationId::value() const noexcept -> std::string_view { return identifier_; }

namespace detail {

auto invalid_source_operations_error(std::string_view missing_operation) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{"monitoring.source.invalid_operations"},
        .message = "Monitoring source operations are invalid",
        .operation = "monitoring_source",
        .os_error = std::nullopt,
        .context = {{"missing_operation", missing_operation}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment,
    };
}

} // namespace detail

auto MonitoringClockOperations::create(void* context, NowOperation operation)
    -> std::expected<MonitoringClockOperations, core::Error> {
    if (operation == nullptr) {
        return std::unexpected{detail::invalid_source_operations_error("clock_now")};
    }
    return MonitoringClockOperations{context, operation};
}

} // namespace epycbench::monitoring
