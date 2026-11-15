#pragma once

#include "monitoring/host_q2_backend.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace epycbench::monitoring::qualification::detail {

constexpr std::size_t monitoring_q2_diagnostic_transport_capacity = 1024U * 1024U;

struct MonitoringQ2DiagnosticBuffer final {
    std::array<char, monitoring_q2_diagnostic_transport_capacity> bytes{};
    std::size_t size{};
    bool failed{};

    void add(std::string_view) noexcept;
    void number(int) noexcept;
    void number(std::uint64_t) noexcept;
};

struct MonitoringQ2DiagnosticWriteOperations final {
    void* context{};
    std::ptrdiff_t (*write)(void*, int, const void*, std::size_t) noexcept {};
};

[[nodiscard]] auto monitoring_q2_phase_name(MonitoringHostQ2BackendPhase) noexcept
    -> std::string_view;
void append_monitoring_q2_projected_failure(MonitoringQ2DiagnosticBuffer&, std::string_view,
                                            std::string_view, int,
                                            std::string_view = {}) noexcept;
void append_monitoring_q2_factual_failure(MonitoringQ2DiagnosticBuffer&,
                                          const core::Error&) noexcept;
void append_monitoring_q2_native_cleanup(MonitoringQ2DiagnosticBuffer&, std::string_view,
                                         std::string_view, int) noexcept;
void finish_monitoring_q2_diagnostic_buffer(MonitoringQ2DiagnosticBuffer&, std::string_view,
                                            std::string_view, int) noexcept;
[[nodiscard]] auto
write_monitoring_q2_diagnostic_buffer(const MonitoringQ2DiagnosticBuffer&,
                                      MonitoringQ2DiagnosticWriteOperations) noexcept -> bool;
[[nodiscard]] auto serialize_monitoring_host_q2_failure(const MonitoringHostQ2BackendFailure&,
                                                        MonitoringQ2DiagnosticBuffer&) noexcept
    -> bool;

} // namespace epycbench::monitoring::qualification::detail
