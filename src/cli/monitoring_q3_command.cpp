#include "cli/monitoring_q3_command.hpp"
#include "cli/monitoring_q3_command_test_access.hpp"
#include "monitoring/host_q3_backend.hpp"
#include <filesystem>
#include <ostream>
#include <ranges>
namespace epycbench::cli {
namespace {
constexpr std::size_t stdout_limit = 1U << 20U;
constexpr std::size_t stderr_limit = 1U << 16U;
thread_local detail::MonitoringQ3Dispatch dispatch{};
thread_local detail::MonitoringQ3Validate validate{};
thread_local detail::MonitoringQ3SerializeFailure serialize_failure{};
thread_local detail::MonitoringQ3SerializeAuthored serialize_authored{};
auto valid_qid(std::string_view value) noexcept -> bool {
    constexpr std::string_view prefix = "MON-007.host-v2.";
    if (!value.starts_with(prefix) || value.size() <= prefix.size() ||
        value.size() > prefix.size() + 64U)
        return false;
    const auto suffix = value.substr(prefix.size());
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return alnum(suffix.front()) && alnum(suffix.back()) &&
           std::ranges::all_of(suffix, [&](char c) { return alnum(c) || c == '-'; });
}
auto write_exact(std::string_view bytes, std::ostream& stream, std::size_t limit,
                 int success) noexcept -> int {
    if (bytes.size() > limit)
        return 1;
    try {
        auto* buffer = stream.rdbuf();
        if (buffer == nullptr ||
            buffer->sputn(bytes.data(), static_cast<std::streamsize>(bytes.size())) !=
                static_cast<std::streamsize>(bytes.size()) ||
            buffer->pubsync() != 0)
            return 1;
        return success;
    } catch (...) {
        return 1;
    }
}
auto emit_failure(const monitoring::qualification::detail::MonitoringHostQ3BackendFailure& value,
                  std::ostream& error, int status) noexcept -> int {
    try {
        auto bytes =
            serialize_failure
                ? serialize_failure(value)
                : monitoring::qualification::detail::serialize_monitoring_host_q3_backend_failure(
                      value);
        if (!bytes)
            return 1;
        return write_exact(*bytes, error, stderr_limit, status);
    } catch (...) {
        return 1;
    }
}
} // namespace
auto run_monitoring_host_q3_command(int argc, char* argv[],
                                    runtime::ControllerProcessProductionAuthority& authority,
                                    std::ostream& output, std::ostream& error) noexcept -> int {
    using namespace monitoring::qualification::detail;
    try {
        if (argc != 6 || std::string_view{argv[1]} != "__monitoring-host-q3-temperature" ||
            std::string_view{argv[2]} != "--repository-root" || std::string_view{argv[3]}.empty() ||
            !std::filesystem::path{argv[3]}.is_absolute() ||
            std::string_view{argv[4]} != "--qualification-id" || !valid_qid(argv[5]))
            return emit_failure(monitoring_host_q3_usage_failure(), error, 2);
        auto result = dispatch ? dispatch(argv[3], argv[5], authority)
                               : run_monitoring_host_q3_backend(argv[3], argv[5], authority);
        if (!result)
            return emit_failure(result.error(), error, 1);
        if (const auto* authored = std::get_if<MonitoringHostQ3BackendAuthored>(&*result)) {
            auto bytes = serialize_authored ? serialize_authored(*authored)
                                            : serialize_monitoring_host_q3_authored(*authored);
            if (!bytes)
                return 1;
            return write_exact(*bytes, error, stderr_limit, 6);
        }
        const auto& bytes = std::get<MonitoringHostQ3BackendBytes>(*result).canonical_json;
        if (bytes.size() > stdout_limit ||
            !(validate ? validate(bytes) : validate_monitoring_host_q3_backend_success(bytes)))
            return 1;
        return write_exact(bytes, output, stdout_limit, 0);
    } catch (...) {
        return 1;
    }
}
void detail::MonitoringQ3CommandTestAccess::set(MonitoringQ3Dispatch d, MonitoringQ3Validate v,
                                                MonitoringQ3SerializeFailure f,
                                                MonitoringQ3SerializeAuthored a) noexcept {
    dispatch = d;
    validate = v;
    serialize_failure = f;
    serialize_authored = a;
}
void detail::MonitoringQ3CommandTestAccess::reset() noexcept {
    dispatch = nullptr;
    validate = nullptr;
    serialize_failure = nullptr;
    serialize_authored = nullptr;
}
} // namespace epycbench::cli
