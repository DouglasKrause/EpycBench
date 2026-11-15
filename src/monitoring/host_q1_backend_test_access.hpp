#pragma once
#include "monitoring/host_q1_backend.hpp"
#include "monitoring/host_q1_page_fault_collector.hpp"
#include "monitoring/host_q1_git_command_owner.hpp"
#include "epycbench/monitoring/host_input_template.hpp"
#include "epycbench/monitoring/host_runner_context_session.hpp"
namespace epycbench::monitoring::qualification::detail {
struct MonitoringHostQ1BackendOperations final {
    void* context{};
    std::expected<MonitoringHostInputTemplateAuthority, core::Error> (*load)(
        void*, const std::filesystem::path&);
    std::expected<RunnerContextAcquisitionSession, core::Error> (*acquire)(
        void*, MonitoringHostInputTemplateAuthority&&);
    std::expected<MonitoringQ1PageFaultCollection, MonitoringQ1PageFaultCollectionFailure> (*collect)(
        void*, MonitoringQ1CollectorPlan&&, runtime::RunnerCancellation&);
};
class MonitoringHostQ1BackendTestAccess final {
  public:
    using BuildProducer = nlohmann::json (*)(void*, std::string_view);
    using StageHook = std::expected<void, core::Error> (*)(void*, std::string_view);
    static void install_hooks(void*, BuildProducer, StageHook) noexcept;
    static void install_operations(MonitoringHostQ1BackendOperations*) noexcept;
    static void clear_hooks() noexcept;
    [[nodiscard]] static auto map_authority(MonitoringQ1GitCommandFailure)
        -> MonitoringHostQ1BackendFailure;
    [[nodiscard]] static auto attest(nlohmann::json, std::string_view)
        -> std::expected<nlohmann::ordered_json, core::Error>;
    [[nodiscard]] static auto evidence(MonitoringQ1PageFaultCollection&&,
                                       nlohmann::ordered_json)
        -> std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure>;
    [[nodiscard]] static auto contain(std::string_view, std::vector<core::Error>)
        -> MonitoringHostQ1BackendFailure;
};
}
