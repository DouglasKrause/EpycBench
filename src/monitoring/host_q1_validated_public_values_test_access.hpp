#pragma once
#include "monitoring/host_q1_validated_public_values.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1ValidatedPublicValuesTestAccess final {
  public:
    static auto bounded_preflight(std::string_view bytes) -> bool {
        return MonitoringQ1ValidatedPublicValues::bounded_preflight(bytes);
    }
    static auto profile_schema(std::string_view profile, std::string_view policy) -> bool {
        return MonitoringQ1ValidatedPublicValues::profile_schema_for_test(profile, policy);
    }
    static void replace_q0(MonitoringQ1ValidatedPublicValues& value,
                           MonitoringQ1Q0PublicProjection projection) {
        value.q0_ = std::move(projection);
    }
    static void replace_policy(MonitoringQ1ValidatedPublicValues& value,
                               std::string identity, std::string sha256) {
        value.policy_identity_ = std::move(identity);
        value.policy_sha_ = std::move(sha256);
    }
};
} // namespace epycbench::monitoring::qualification::detail
