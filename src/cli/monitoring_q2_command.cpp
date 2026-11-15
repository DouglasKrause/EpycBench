#include "cli/monitoring_q2_command.hpp"
#include "cli/monitoring_q2_command_test_access.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "monitoring/host_q2_diagnostic_transport.hpp"
#include "runtime/controller_process_production_authority.hpp"
#include <array>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <ostream>
#include <ranges>
namespace epycbench::cli {
namespace {
thread_local detail::MonitoringQ2Dispatch dispatch{};
thread_local detail::MonitoringQ2BeforeFailureSerialization before_failure_serialization{};
thread_local detail::MonitoringQ2BeforeFailureSerialization after_failure_serialization{};
thread_local monitoring::qualification::detail::MonitoringQ2DiagnosticBuffer diagnostic_buffer{};
thread_local bool diagnostic_buffer_in_use{};
auto valid_qid(std::string_view value) noexcept -> bool {
    constexpr std::string_view prefix = "MON-007.host-v2.";
    if (!value.starts_with(prefix) || value.size() <= prefix.size() ||
        value.size() > prefix.size() + 64U)
        return false;
    auto suffix = value.substr(prefix.size());
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return alnum(suffix.front()) && alnum(suffix.back()) &&
           std::ranges::all_of(suffix, [&](char c) { return alnum(c) || c == '-'; });
}
auto write_json(const nlohmann::ordered_json& value, std::ostream& stream, int status) noexcept
    -> int {
    try {
        auto bytes = core::canonicalize_json(value);
        if (!bytes || bytes->size() + 1U > 1024U * 1024U)
            return 1;
        stream << *bytes << '\n';
        stream.flush();
        return stream ? status : 1;
    } catch (...) {
        return 1;
    }
}
auto write_failure(const monitoring::qualification::detail::MonitoringHostQ2BackendFailure& value,
                   std::ostream& stream, int status) noexcept -> int {
    using namespace monitoring::qualification::detail;
    if (diagnostic_buffer_in_use)
        return 1;
    struct Lease final {
        Lease() noexcept { diagnostic_buffer_in_use = true; }
        ~Lease() noexcept { diagnostic_buffer_in_use = false; }
    } lease;
    try {
        diagnostic_buffer.size = 0U;
        diagnostic_buffer.failed = false;
        if (before_failure_serialization)
            before_failure_serialization();
        if (!serialize_monitoring_host_q2_failure(value, diagnostic_buffer))
            return 1;
        if (after_failure_serialization)
            after_failure_serialization();
        const MonitoringQ2DiagnosticWriteOperations operations{
            &stream,
            [](void* context, int, const void* bytes, std::size_t size) noexcept -> std::ptrdiff_t {
                try {
                    auto& target = *static_cast<std::ostream*>(context);
                    auto* buffer = target.rdbuf();
                    if (!buffer || size > static_cast<std::size_t>(
                                              std::numeric_limits<std::streamsize>::max())) {
                        errno = EIO;
                        return -1;
                    }
                    const auto written = buffer->sputn(static_cast<const char*>(bytes),
                                                       static_cast<std::streamsize>(size));
                    if (written == static_cast<std::streamsize>(size))
                        return static_cast<std::ptrdiff_t>(size);
                    errno = EIO;
                    return written < 0 ? -1 : static_cast<std::ptrdiff_t>(written);
                } catch (...) {
                    errno = EIO;
                    return -1;
                }
            }};
        const auto written = write_monitoring_q2_diagnostic_buffer(diagnostic_buffer, operations);
        auto* buffer = stream.rdbuf();
        return written && buffer && buffer->pubsync() == 0 ? status : 1;
    } catch (...) {
        return 1;
    }
}
} // namespace
auto run_monitoring_host_q2_command(int argc, char* argv[],
                                    runtime::ControllerProcessProductionAuthority& authority,
                                    std::ostream& output, std::ostream& error) noexcept -> int {
    using namespace monitoring::qualification::detail;
    if (argc != 6 || std::string_view{argv[1]} != "__monitoring-host-q2-context-switch" ||
        std::string_view{argv[2]} != "--repository-root" || std::string_view{argv[3]}.empty() ||
        !std::filesystem::path{argv[3]}.is_absolute() ||
        std::string_view{argv[4]} != "--qualification-id" || !valid_qid(argv[5]))
        return write_failure(monitoring_host_q2_usage_failure(), error, 2);
    auto result = dispatch ? dispatch(argv[3], argv[5], authority)
                           : run_monitoring_host_q2_backend(argv[3], argv[5], authority);
    if (!result)
        return write_failure(result.error(), error, 1);
    if (auto* authored = std::get_if<MonitoringHostQ2BackendAuthored>(&*result)) {
        auto projected = project_monitoring_host_q2_authored(authored->diagnostics);
        return projected ? write_json(*projected, error, 6) : 1;
    }
    try {
        const auto& bytes = std::get<MonitoringHostQ2BackendBytes>(*result).canonical_json;
        if (!validate_monitoring_host_q2_backend_success(bytes))
            return 1;
        output << bytes;
        output.flush();
        return output ? 0 : 1;
    } catch (...) {
        return 1;
    }
}
void detail::MonitoringQ2CommandTestAccess::set_dispatch(MonitoringQ2Dispatch value) noexcept {
    dispatch = value;
}
void detail::MonitoringQ2CommandTestAccess::set_before_failure_serialization(
    MonitoringQ2BeforeFailureSerialization value) noexcept {
    before_failure_serialization = value;
}
void detail::MonitoringQ2CommandTestAccess::set_after_failure_serialization(
    MonitoringQ2BeforeFailureSerialization value) noexcept {
    after_failure_serialization = value;
}
void detail::MonitoringQ2CommandTestAccess::reset() noexcept {
    dispatch = nullptr;
    before_failure_serialization = nullptr;
    after_failure_serialization = nullptr;
}
} // namespace epycbench::cli
