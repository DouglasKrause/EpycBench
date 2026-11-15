#pragma once
#include "monitoring/host_q1_private_q0_value.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1PrivateQ0ValueTestAccess final {
  public:
    static auto retain(int root, const MonitoringQ1ValidatedPublicValues& authority,
                       MonitoringQ1PrivateQ0Value::Operations operations) {
        return MonitoringQ1PrivateQ0Value::retain_with(root, authority, operations);
    }
    static auto validate(const std::array<std::string, 4>& bytes,
                         const MonitoringQ1ValidatedPublicValues& authority) {
        return MonitoringQ1PrivateQ0Value::validate(bytes, authority);
    }
    static auto validate_successor_subset(std::string_view environment, std::string_view evidence,
        const MonitoringQ1ValidatedPublicValues& authority) {
        return MonitoringQ1PrivateQ0Value::validate_successor_subset(environment, evidence, authority);
    }
    static auto validate_document(std::size_t index, const std::array<std::string, 4>& bytes,
                                  const MonitoringQ1ValidatedPublicValues& authority) {
        return MonitoringQ1PrivateQ0Value::validate_document_for_test(index, bytes, authority);
    }
    static auto descriptors(const MonitoringQ1PrivateQ0Value& value) noexcept {
        return value.descriptors_;
    }
    static auto owned(std::array<int, 8> descriptors,
                      MonitoringQ1PrivateQ0Value::Operations operations) {
        const auto error = [](std::string classification) {
            return core::Error{
                .category = core::ErrorCategory::monitoring,
                .code = core::StableErrorCode{"monitoring.q1_plan_authority.contained_failure"},
                .message = "contained",
                .operation = "authenticate_monitoring_q1_plan",
                .os_error = std::nullopt,
                .context = {{"classification", std::move(classification)}},
                .severity = core::ErrorSeverity::error,
                .continuation = core::ContinuationPolicy::abort_command};
        };
        return MonitoringQ1PrivateQ0Value{-1,
                                          descriptors,
                                          {},
                                          {},
                                          operations,
                                          error("contained_standard_exception"),
                                          error("contained_unknown_exception"),
                                          error("contained_standard_exception"),
                                          error("contained_unknown_exception")};
    }
};
} // namespace epycbench::monitoring::qualification::detail
