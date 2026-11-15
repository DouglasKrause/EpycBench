#include "epycbench/kernel/sequential_read_bandwidth.hpp"

#include <exception>
#include <string>
#include <string_view>

namespace epycbench::kernel {
namespace {

[[nodiscard]] auto accounting_error(std::string_view code,
                                    std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Bandwidth work accounting failed",
            .operation = "kernel.bandwidth.accounting",
            .os_error = std::nullopt,
            .context = {{"phase", "work_accounting"},
                        {"implementation", "scalar-u64-sequential/v1"},
                        {"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto overflow_error() -> core::Error {
    return accounting_error("kernel.bandwidth.accounting.overflow",
                            "logical_byte_count_overflow");
}

[[nodiscard]] auto contained_standard() -> core::Error {
    return accounting_error("kernel.bandwidth.accounting.contained_exception",
                            "contained_exception");
}

[[nodiscard]] auto contained_unknown() -> core::Error {
    return accounting_error("kernel.bandwidth.accounting.contained_unknown_failure",
                            "contained_unknown_failure");
}

} // namespace

SequentialReadBandwidthWork::SequentialReadBandwidthWork(
    core::ByteCount logical_bytes_read) noexcept
    : logical_bytes_read_{logical_bytes_read} {}

auto account_sequential_read_bandwidth_work(
    const PreparedSequentialReadBandwidth& prepared,
    SequentialReadBandwidthPassCount passes) noexcept
    -> std::expected<SequentialReadBandwidthWork, core::Error> {
    try {
        const auto logical_bytes_read = prepared.input()
                                            .configuration()
                                            .working_set_per_worker()
                                            .checked_multiply(passes.value());
        if (!logical_bytes_read) {
            return std::unexpected{overflow_error()};
        }
        return SequentialReadBandwidthWork{*logical_bytes_read};
    } catch (const std::exception&) {
        return std::unexpected{contained_standard()};
    } catch (...) {
        return std::unexpected{contained_unknown()};
    }
}

} // namespace epycbench::kernel
