#pragma once
#include "monitoring/host_q1_git_fixed_results.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1GitFixedResultAuthorityTestAccess final {
  public:
    using Operations = MonitoringQ1GitFixedResultAuthority::Operations;
    static auto run(Operations operations) {
        return MonitoringQ1GitFixedResultAuthority::run_with(operations);
    }
    static auto run_q2(Operations operations) {
        return MonitoringQ1GitFixedResultAuthority::run_with(operations, 4U);
    }
    static auto run_q3(Operations operations) {
        return MonitoringQ1GitFixedResultAuthority::run_with(operations, 5U);
    }
    static auto object_value(const MonitoringQ1GitObjectId& value) -> std::string_view {
        return value.value_;
    }
    static auto object_matches(const MonitoringQ1GitObjectId& value, const void* binding,
                               std::size_t width, std::size_t slot) -> bool {
        return MonitoringQ1GitSpawn::validate_blob_token(value, binding, width, slot);
    }
    static auto make_object(std::string value, const void* binding, std::size_t width,
                            std::size_t slot, std::size_t stdout_limit = 1U << 20U)
        -> MonitoringQ1GitObjectId {
        return MonitoringQ1GitObjectId{std::move(value), binding, width, slot, stdout_limit};
    }
    static auto record_path(const MonitoringQ1GitFixedRecord& value) -> std::string_view {
        return value.path_;
    }
    static auto record_object(const MonitoringQ1GitFixedRecord& value) -> std::string_view {
        return value.object_.value_;
    }
    static auto record_bytes(const MonitoringQ1GitFixedRecord& value) -> std::string_view {
        return value.bytes_;
    }
    static auto captured_head(const MonitoringQ1GitFixedResults& value) -> std::string_view {
        return value.head_;
    }
    static auto make_results(std::string head, const std::array<std::string, 3>& bytes)
        -> MonitoringQ1GitFixedResults {
        MonitoringQ1GitFixedResults result{std::move(head)};
        constexpr std::array paths{
            "config/monitoring-qualification/host-v2-policy.json",
            "config/monitoring-qualification/host-v2-profile.json",
            "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json"};
        for (std::size_t index = 0; index < paths.size(); ++index)
            result.records.push_back(MonitoringQ1GitFixedRecord{
                paths[index],
                MonitoringQ1GitObjectId{std::string(40, 'a'), result.frame_.get(), 40, index},
                bytes[index]});
        return result;
    }
    static auto make_results(std::string head, const std::array<std::string, 4>& bytes)
        -> MonitoringQ1GitFixedResults {
        MonitoringQ1GitFixedResults result{std::move(head)};
        constexpr std::array paths{
            "config/monitoring-qualification/host-v2-policy.json",
            "config/monitoring-qualification/host-v2-profile.json",
            "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
            "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json"};
        for (std::size_t index = 0; index < paths.size(); ++index)
            result.records.push_back(MonitoringQ1GitFixedRecord{
                paths[index],
                MonitoringQ1GitObjectId{std::string(40, 'a'), result.frame_.get(), 40, index},
                bytes[index]});
        return result;
    }
    static auto make_q3_results(std::string head, const std::array<std::string, 5>& bytes)
        -> MonitoringQ1GitFixedResults {
        MonitoringQ1GitFixedResults result{std::move(head)};
        constexpr std::array paths{
            "config/monitoring-qualification/host-v2-policy.json",
            "config/monitoring-qualification/host-v2-profile.json",
            "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
            "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json",
            "config/monitoring-qualification/host-v2-q3-temperature-collection-plan.json"};
        for (std::size_t index = 0; index < paths.size(); ++index)
            result.records.push_back(MonitoringQ1GitFixedRecord{
                paths[index],
                MonitoringQ1GitObjectId{std::string(40, 'a'), result.frame_.get(), 40, index},
                bytes[index]});
        return result;
    }
    static auto make_q3_results_with_q1_q2_reversed(std::string head,
                                                    const std::array<std::string, 5>& bytes)
        -> MonitoringQ1GitFixedResults {
        MonitoringQ1GitFixedResults result{std::move(head)};
        constexpr std::array paths{
            "config/monitoring-qualification/host-v2-policy.json",
            "config/monitoring-qualification/host-v2-profile.json",
            "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json",
            "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
            "config/monitoring-qualification/host-v2-q3-temperature-collection-plan.json"};
        for (std::size_t index = 0; index < paths.size(); ++index)
            result.records.push_back(MonitoringQ1GitFixedRecord{
                paths[index],
                MonitoringQ1GitObjectId{std::string(40, 'a'), result.frame_.get(), 40, index},
                bytes[index]});
        return result;
    }
};
} // namespace epycbench::monitoring::qualification::detail
