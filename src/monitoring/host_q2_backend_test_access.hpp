#pragma once

#include "monitoring/host_q2_backend.hpp"
#include "monitoring/host_q2_authority_session.hpp"
#include "monitoring/host_q2_context_switch_collector.hpp"

namespace epycbench::monitoring::qualification::detail {

struct MonitoringHostQ2BackendOperations final {
    enum class CollectionDisposition : std::uint8_t { evidence, failed, terminal };
    void* context{};
    std::expected<void, MonitoringQ2AuthorityFailure> (*authenticate)(
        void*, const std::filesystem::path&, std::string_view) {};
    std::expected<void, core::Error> (*validate_build)(void*) {};
    std::expected<void, core::Error> (*acquire_session)(
        void*, runtime::ControllerProcessProductionAuthority&) {};
    std::expected<void, core::Error> (*admit_workers)(void*) {};
    std::expected<void, MonitoringQ2AuthorityFailure> (*bind)(void*) {};
    CollectionDisposition (*collect)(void*) {};
    MonitoringQ2FailedCollection (*take_collection_failure)(void*) noexcept {};
    std::expected<MonitoringHostQ2BackendResult, core::Error> (*construct_evidence)(void*) {};
    MonitoringQ2AuthoritySession::PrebindRetirement (*retire)(void*) noexcept {};
    void (*consume_terminal)(void*) noexcept {};
};

class MonitoringHostQ2BackendTestAccess final {
  public:
    static auto run(const std::filesystem::path&, std::string_view,
                    runtime::ControllerProcessProductionAuthority&,
                    const MonitoringHostQ2BackendOperations&) noexcept
        -> std::expected<MonitoringHostQ2BackendResult, MonitoringHostQ2BackendFailure>;
};

} // namespace epycbench::monitoring::qualification::detail
