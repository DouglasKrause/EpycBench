#pragma once
#include "monitoring/current_revision_tracked_capture.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringCurrentRevisionTrackedCaptureTestAccess final {
  public:
    using Operations = MonitoringCurrentRevisionTrackedCaptureCore::Operations;
    static auto run(Operations value) {
        return MonitoringCurrentRevisionTrackedCaptureCore::run_with(value);
    }
    static auto head(const MonitoringCurrentRevisionTrackedCapture& value) {
        return std::string_view{value.head_};
    }
    static auto count(const MonitoringCurrentRevisionTrackedCapture& value) {
        return value.records_.size();
    }
    static auto path(const MonitoringCurrentRevisionTrackedCapture& value, std::size_t slot) {
        return std::string_view{value.records_.at(slot).path_};
    }
    static auto bytes(const MonitoringCurrentRevisionTrackedCapture& value, std::size_t slot) {
        return std::string_view{value.records_.at(slot).bytes_};
    }
    static auto binding(const MonitoringCurrentRevisionTrackedCapture& value) noexcept {
        return value.frame_.get();
    }
    static auto token(const MonitoringCurrentRevisionTrackedCapture& value, std::size_t slot)
        -> const MonitoringQ1GitObjectId& {
        return value.records_.at(slot).object_;
    }
    static auto token_matches(const MonitoringQ1GitObjectId& value, const void* binding,
                              std::size_t width, std::size_t slot) noexcept {
        return value.binding_ == binding && value.width_ == width && value.slot_ == slot;
    }
    static auto token_stdout_limit(const MonitoringQ1GitObjectId& value) noexcept {
        return value.stdout_limit_;
    }
    static auto make(std::array<std::string, 18> bytes) {
        MonitoringCurrentRevisionTrackedCapture result{std::string(40, 'a')};
        constexpr std::array<std::string_view, 18> fixed_paths{
            "config/monitoring-qualification/host-v2-profile.json",
            "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
            "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json",
            "schemas/monitoring-q2-context-switch-collection-plan-v1.schema.json",
            "schemas/monitoring-host-q2-backend-result-v1.schema.json",
            "schemas/monitoring-host-q2-backend-error-v1.schema.json",
            "schemas/monitoring-host-q2-backend-authored-v1.schema.json",
            "config/monitoring-qualification/host-v2-q3-temperature-collection-plan.json",
            "schemas/monitoring-q3-temperature-collection-plan-v1.schema.json",
            "schemas/monitoring-host-q3-temperature-evidence-v1.schema.json",
            "schemas/monitoring-host-q3-backend-result-v1.schema.json",
            "config/monitoring-qualification/host-v2-q4-frequency-collection-plan.json",
            "schemas/monitoring-q4-frequency-collection-plan-v1.schema.json",
            "schemas/monitoring-host-q4-frequency-evidence-v1.schema.json",
            "schemas/monitoring-host-q4-backend-result-v1.schema.json",
            "schemas/monitoring-host-q4-backend-error-v1.schema.json",
            "schemas/monitoring-host-q4-backend-authored-v1.schema.json",
            "schemas/monitoring-host-q2-backend-result-v1-evidence-v2.schema.json"};
        for (std::size_t slot{}; slot < fixed_paths.size(); ++slot)
            result.records_.push_back(MonitoringCurrentRevisionTrackedRecord{
                std::string{fixed_paths[slot]},
                MonitoringQ1GitObjectId{std::string(40, 'b'), result.frame_.get(), 40, slot},
                std::move(bytes[slot])});
        return result;
    }
    static void erase_record(MonitoringCurrentRevisionTrackedCapture& value) {
        value.records_.pop_back();
    }
    static void replace_path(MonitoringCurrentRevisionTrackedCapture& value, std::size_t slot,
                             std::string path) {
        value.records_.at(slot).path_ = std::move(path);
    }
};
} // namespace epycbench::monitoring::qualification::detail
