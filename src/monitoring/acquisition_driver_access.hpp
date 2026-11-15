#pragma once

#include "epycbench/core/error.hpp"

#include <cstddef>
#include <cstdint>

namespace epycbench::monitoring {
class MonitoringAcquisitionDriver;
class MonitoringObservationCapture;
}

namespace epycbench::monitoring::detail {

enum class AcquisitionDriverExceptionBoundary {
    none, create, service, begin, begin_after_active, complete, abort, stop,
    stop_retirement, stop_wake, stop_join, stop_terminalization
};
enum class AcquisitionDriverExceptionKind { standard, unknown };
enum class MonitoringRegistrationTestFault : std::uint8_t { wrong_generation, stale_unregistered };

struct AcquisitionDriverSnapshot {
    bool active{};
    bool arm_requested{};
    bool armed{};
    bool finish_requested{};
    bool abort_requested{};
    bool joined{};
    bool terminal{};
    bool service_thread{};
    std::uint8_t registration_state{};
    std::uint64_t registration_generation{};
    std::size_t raw_item_count{};
    std::uint64_t scheduled{};
    std::uint64_t completed{};
    std::uint64_t wakeups{};
};

struct AcquisitionDriverAccess {
    static void inject(AcquisitionDriverExceptionBoundary, AcquisitionDriverExceptionKind) noexcept;
    static void clear() noexcept;
    static void corrupt_registration_for_test(MonitoringObservationCapture&,
                                               MonitoringRegistrationTestFault) noexcept;
    static void consume_retired_registration_for_test(MonitoringObservationCapture&) noexcept;
    static void inject_service_error_for_test(MonitoringAcquisitionDriver&, core::Error) noexcept;
    [[nodiscard]] static auto snapshot(const MonitoringAcquisitionDriver&) noexcept
        -> AcquisitionDriverSnapshot;
};

} // namespace epycbench::monitoring::detail
