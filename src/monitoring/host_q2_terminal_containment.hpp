#pragma once
#include "monitoring/host_q2_compact_diagnostic.hpp"
#include "monitoring/host_q2_context_switch_source.hpp"
#include "monitoring/host_q2_helper_provider.hpp"

namespace epycbench::monitoring::qualification::detail {
enum class MonitoringQ2CollectionStage : std::uint8_t {
    preparation,
    session,
    source_preparation,
    voluntary_event,
    preemption_event,
    drain,
    association,
    clock,
    completeness,
    finalization
};
class MonitoringQ2TerminalContainmentTestAccess;
class MonitoringQ2TerminalContainment;
class MonitoringQ2TerminalContainmentStock final {
  public:
    MonitoringQ2TerminalContainmentStock(MonitoringQ2TerminalContainmentStock&&) noexcept;
    MonitoringQ2TerminalContainmentStock(const MonitoringQ2TerminalContainmentStock&) = delete;
    ~MonitoringQ2TerminalContainmentStock() noexcept;

  private:
    struct Impl;
    explicit MonitoringQ2TerminalContainmentStock(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class MonitoringQ2TerminalContainment;
    friend class MonitoringQ2ContextSwitchCollector;
};
struct MonitoringQ2TerminalContainmentOperations final {
    void* context{};
    std::ptrdiff_t (*write)(void*, int, const void*, std::size_t) noexcept {};
    void (*exit)(void*, int) noexcept {};
};
[[noreturn]] void
consume_monitoring_q2_terminal_containment(MonitoringQ2TerminalContainment&&) noexcept;
[[noreturn]] void
consume_monitoring_q2_terminal_containment_with(MonitoringQ2TerminalContainment&&,
                                                MonitoringQ2TerminalContainmentOperations) noexcept;
class MonitoringQ2TerminalContainment final {
  public:
    MonitoringQ2TerminalContainment(MonitoringQ2TerminalContainment&&) noexcept;
    MonitoringQ2TerminalContainment(const MonitoringQ2TerminalContainment&) = delete;
    ~MonitoringQ2TerminalContainment() noexcept;

  private:
    static auto reserve(core::Error) noexcept
        -> std::expected<MonitoringQ2TerminalContainmentStock, core::Error>;
    static auto
    contain(MonitoringQ2TerminalContainmentStock&&, MonitoringQ2BridgeRetirementRecovery&&,
            MonitoringQ2ContextSwitchSourceOwner&&, MonitoringQ2CollectorPlan&&,
            MonitoringQ2CollectionStage, core::Error, std::array<std::optional<core::Error>, 64>&&,
            std::size_t, bool, std::optional<MonitoringQ2HelperProvider>&&) noexcept
        -> MonitoringQ2TerminalContainment;
    static auto contain_session(MonitoringQ2TerminalContainmentStock&&, MonitoringQ2CollectorPlan&&,
                                MonitoringQ2CollectionStage, core::Error,
                                std::array<std::optional<core::Error>, 64>&&, std::size_t, bool,
                                bool, std::optional<MonitoringQ2HelperProvider>&&) noexcept
        -> MonitoringQ2TerminalContainment;
    explicit MonitoringQ2TerminalContainment(
        std::unique_ptr<MonitoringQ2TerminalContainmentStock::Impl>) noexcept;
    std::unique_ptr<MonitoringQ2TerminalContainmentStock::Impl> impl_;
    friend class MonitoringQ2ContextSwitchCollector;
    friend class MonitoringQ2TerminalContainmentTestAccess;
    friend void
    consume_monitoring_q2_terminal_containment(MonitoringQ2TerminalContainment&&) noexcept;
    friend void consume_monitoring_q2_terminal_containment_with(
        MonitoringQ2TerminalContainment&&, MonitoringQ2TerminalContainmentOperations) noexcept;
    friend class MonitoringQ2TerminalContainmentTestAccess;
};
} // namespace epycbench::monitoring::qualification::detail
