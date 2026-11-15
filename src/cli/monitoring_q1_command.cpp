#include "cli/monitoring_q1_command.hpp"
#include "cli/monitoring_q1_command_test_access.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "runtime/controller_process_production_authority.hpp"
#include <ostream>
#include <ranges>
#include <string_view>

namespace epycbench::cli {
namespace {
thread_local detail::MonitoringQ1Dispatch dispatch{};
auto valid_qid(std::string_view value) noexcept -> bool {
    constexpr std::string_view prefix = "MON-007.host-v2.";
    if (!value.starts_with(prefix)) return false;
    const auto suffix = value.substr(prefix.size());
    return !suffix.empty() && suffix.size() <= 64U && suffix.front() != '-' && suffix.back() != '-' &&
        std::ranges::all_of(suffix, [](char c) { return (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-'; });
}
auto write_failure(const monitoring::qualification::detail::MonitoringHostQ1BackendFailure& value,
                   std::ostream& error, int status) noexcept -> int {
    try {
        auto projected = monitoring::qualification::detail::project_monitoring_host_q1_error(value);
        if (!projected) throw 0;
        auto bytes = core::canonicalize_json(*projected);
        if (!bytes || bytes->size() + 1U > 1024U * 1024U) throw 0;
        error << *bytes << '\n'; error.flush();
        return error ? status : 1;
    } catch (...) {
        return 1;
    }
}
}
auto run_monitoring_host_q1_command(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, std::ostream& output,
    std::ostream& error) noexcept -> int {
    using namespace monitoring::qualification::detail;
    if (argc != 6 || std::string_view{argv[1]} != "__monitoring-host-q1-page-fault" ||
        std::string_view{argv[2]} != "--repository-root" || std::string_view{argv[3]}.empty() ||
        std::string_view{argv[4]} != "--qualification-id" || !valid_qid(argv[5]))
        return write_failure(monitoring_host_q1_usage_failure(), error, 2);
    auto result = dispatch ? dispatch(argv[3], argv[5], authority)
                           : run_monitoring_host_q1_backend(argv[3], argv[5], authority);
    if (!result) return write_failure(result.error(), error, 1);
    if (const auto* authored = std::get_if<MonitoringHostQ1BackendAuthored>(&*result)) {
        auto projected = project_monitoring_host_q1_authored(authored->diagnostics);
        if (!projected) return write_failure({projected.error(), {}}, error, 1);
        try {
            auto bytes = core::canonicalize_json(*projected);
            if (!bytes || bytes->size() + 1U > 1024U * 1024U) return 1;
            error << *bytes << '\n'; error.flush(); return error ? 6 : 1;
        } catch (...) { return 1; }
    }
    try {
        const auto& bytes = std::get<MonitoringHostQ1BackendBytes>(*result).canonical_json;
        if (bytes.size() > 10U * 1024U * 1024U) return 1;
        output << bytes; output.flush(); return output ? 0 : 1;
    } catch (...) { return 1; }
}
void detail::MonitoringQ1CommandTestAccess::set_dispatch(MonitoringQ1Dispatch value) noexcept {
    dispatch = value;
}
void detail::MonitoringQ1CommandTestAccess::reset() noexcept { dispatch = nullptr; }
} // namespace epycbench::cli
