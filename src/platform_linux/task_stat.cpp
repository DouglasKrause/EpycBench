#include "epycbench/platform_linux/task_stat.hpp"

#include "platform_linux/task_stat_operations.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux {
namespace detail {
namespace {

[[nodiscard]] auto task_stat_error(std::string_view code, std::string_view message,
                                   std::string_view operation, std::string_view classification,
                                   std::optional<int> error_number = std::nullopt) -> core::Error {
    std::optional<core::OsError> os_error;
    if (error_number && *error_number != 0) {
        os_error = core::OsError::capture(std::error_code{*error_number, std::generic_category()});
    }
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{std::string{code}},
        .message = std::string{message},
        .operation = std::string{operation},
        .os_error = std::move(os_error),
        .context = {{"classification", classification}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto bounded_failure(BoundedTaskStatFailureKind kind,
                                   int error_number = 0) noexcept
    -> BoundedTaskStatReadFailure {
    return {{kind, error_number}, std::nullopt};
}

[[nodiscard]] auto next_bounded_field(std::string_view& text) noexcept
    -> std::optional<std::string_view> {
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1U);
    if (text.empty())
        return std::nullopt;
    const auto end = text.find(' ');
    const auto result = end == std::string_view::npos ? text : text.substr(0U, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1U);
    return result;
}

[[nodiscard]] auto parse_bounded_unsigned(std::string_view token,
                                          BoundedTaskStatFailureKind malformed,
                                          BoundedTaskStatFailureKind range) noexcept
    -> std::expected<std::uint64_t, BoundedTaskStatReadFailure> {
    if (token.empty() || token.front() == '-')
        return std::unexpected{bounded_failure(malformed)};
    std::uint64_t value{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec == std::errc::result_out_of_range)
        return std::unexpected{bounded_failure(range)};
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
        return std::unexpected{bounded_failure(malformed)};
    return value;
}

[[nodiscard]] auto parse_bounded(std::string_view record,
                                 LinuxThreadId expected) noexcept
    -> std::expected<LinuxTaskPageFaultCounters, BoundedTaskStatReadFailure> {
    const auto first_space = record.find(' ');
    if (first_space == std::string_view::npos || first_space == 0U)
        return std::unexpected{bounded_failure(BoundedTaskStatFailureKind::task_id)};
    const auto id = record.substr(0U, first_space);
    std::uint64_t parsed_id{};
    const auto parsed = std::from_chars(id.data(), id.data() + id.size(), parsed_id);
    if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() ||
        parsed_id == 0U ||
        parsed_id > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
        return std::unexpected{bounded_failure(BoundedTaskStatFailureKind::task_id)};
    if (parsed_id != expected.value())
        return std::unexpected{
            bounded_failure(BoundedTaskStatFailureKind::task_identity_mismatch)};
    if (first_space + 2U >= record.size() || record[first_space + 1U] != '(')
        return std::unexpected{
            bounded_failure(BoundedTaskStatFailureKind::command_delimiter)};
    auto delimiter = std::string_view::npos;
    for (auto candidate = record.rfind(')'); candidate != std::string_view::npos;
         candidate = candidate == 0U ? std::string_view::npos
                                     : record.rfind(')', candidate - 1U)) {
        if (candidate + 4U < record.size() && record[candidate + 1U] == ' ' &&
            record[candidate + 3U] == ' ') {
            delimiter = candidate;
            break;
        }
    }
    if (delimiter == std::string_view::npos)
        return std::unexpected{
            bounded_failure(BoundedTaskStatFailureKind::command_delimiter)};
    auto suffix = record.substr(delimiter + 4U);
    std::array<std::string_view, 9> fields{};
    for (auto& field : fields) {
        auto next = next_bounded_field(suffix);
        if (!next)
            return std::unexpected{
                bounded_failure(BoundedTaskStatFailureKind::truncated_record)};
        field = *next;
    }
    auto minor = parse_bounded_unsigned(
        fields[6], BoundedTaskStatFailureKind::minor_faults,
        BoundedTaskStatFailureKind::minor_faults_out_of_range);
    if (!minor)
        return std::unexpected{std::move(minor.error())};
    auto major = parse_bounded_unsigned(
        fields[8], BoundedTaskStatFailureKind::major_faults,
        BoundedTaskStatFailureKind::major_faults_out_of_range);
    if (!major)
        return std::unexpected{std::move(major.error())};
    return LinuxTaskPageFaultCounters::create(*minor, *major);
}

auto raw_open(void*, LinuxThreadId thread) noexcept
    -> BoundedTaskStatOperations::IntegerResult {
    std::array<char, 64> path{};
    constexpr std::string_view prefix{"/proc/self/task/"};
    constexpr std::string_view suffix{"/stat"};
    std::ranges::copy(prefix, path.begin());
    auto converted = std::to_chars(path.data() + prefix.size(),
                                   path.data() + path.size() - suffix.size() - 1U,
                                   thread.value());
    if (converted.ec != std::errc{})
        return {-1, ENAMETOOLONG};
    converted.ptr = std::ranges::copy(suffix, converted.ptr).out;
    *converted.ptr = '\0';
    const auto descriptor = ::open(path.data(), O_RDONLY | O_CLOEXEC);
    return {descriptor, descriptor < 0 ? errno : 0};
}
auto raw_read(void*, int descriptor, void* output, std::size_t size) noexcept
    -> BoundedTaskStatOperations::IntegerResult {
    const auto result = ::read(descriptor, output, size);
    return {result, result < 0 ? errno : 0};
}
auto raw_close(void*, int descriptor) noexcept
    -> BoundedTaskStatOperations::IntegerResult {
    const auto result = ::close(descriptor);
    return {result, result != 0 ? errno : 0};
}

[[nodiscard]] auto bounded_to_core(BoundedTaskStatFailure value) -> core::Error {
    using Kind = BoundedTaskStatFailureKind;
    const auto code = value.kind == Kind::minor_faults_out_of_range ||
                              value.kind == Kind::major_faults_out_of_range
                          ? "linux.task_stat.counter_out_of_range"
                      : value.kind == Kind::task_identity_mismatch
                          ? "linux.task_stat.identity_mismatch"
                      : value.kind == Kind::task_id ||
                                value.kind == Kind::command_delimiter ||
                                value.kind == Kind::truncated_record ||
                                value.kind == Kind::minor_faults ||
                                value.kind == Kind::major_faults
                          ? "linux.task_stat.malformed"
                          : "linux.task_stat.read_failed";
    const auto operation = value.kind == Kind::task_id ||
                                   value.kind == Kind::task_identity_mismatch ||
                                   value.kind == Kind::command_delimiter ||
                                   value.kind == Kind::truncated_record ||
                                   value.kind == Kind::minor_faults ||
                                   value.kind == Kind::minor_faults_out_of_range ||
                                   value.kind == Kind::major_faults ||
                                   value.kind == Kind::major_faults_out_of_range
                               ? "parse_task_stat"
                               : "read_task_page_fault_counters";
    std::string_view classification;
    switch (value.kind) {
    case Kind::invalid_operations: classification = "invalid_operations"; break;
    case Kind::open_failed: classification = "open_failed"; break;
    case Kind::read_failed: classification = "read_failed"; break;
    case Kind::close_failed: classification = "close_failed"; break;
    case Kind::contained_exception: classification = "contained_exception"; break;
    case Kind::contained_unknown_failure: classification = "contained_unknown_failure"; break;
    case Kind::task_id: classification = "task_id"; break;
    case Kind::task_identity_mismatch: classification = "task_identity_mismatch"; break;
    case Kind::command_delimiter: classification = "command_delimiter"; break;
    case Kind::truncated_record: classification = "truncated_record"; break;
    case Kind::minor_faults:
    case Kind::minor_faults_out_of_range: classification = "minor_faults"; break;
    case Kind::major_faults:
    case Kind::major_faults_out_of_range: classification = "major_faults"; break;
    }
    return task_stat_error(code, "Unable to read Linux task-stat record", operation,
                           classification,
                           value.error_number == 0
                               ? std::optional<int>{}
                               : std::optional<int>{value.error_number});
}

} // namespace

auto production_bounded_task_stat_operations() noexcept
    -> BoundedTaskStatOperations {
    return {.context = nullptr,
            .open = raw_open,
            .read = raw_read,
            .close = raw_close,
            .before = nullptr};
}

auto TaskStatAccess::read_bounded(
    LinuxThreadId thread, std::span<char> workspace,
    const BoundedTaskStatOperations& operations) noexcept
    -> std::expected<LinuxTaskPageFaultCounters, BoundedTaskStatReadFailure> {
    if (workspace.empty() || operations.open == nullptr || operations.read == nullptr ||
        operations.close == nullptr)
        return std::unexpected{
            bounded_failure(BoundedTaskStatFailureKind::invalid_operations)};
    try {
        if (operations.before != nullptr)
            operations.before(operations.context);
        const auto opened = operations.open(operations.context, thread);
        if (opened.value < 0)
            return std::unexpected{bounded_failure(
                BoundedTaskStatFailureKind::open_failed, opened.error_number)};
        const auto descriptor = static_cast<int>(opened.value);
        std::size_t used{};
        while (used != workspace.size()) {
            const auto value = operations.read(operations.context, descriptor,
                                               workspace.data() + used,
                                               workspace.size() - used);
            if (value.value > 0) {
                used += static_cast<std::size_t>(value.value);
                continue;
            }
            if (value.value == 0)
                break;
            if (value.error_number == EINTR)
                continue;
            auto result = bounded_failure(BoundedTaskStatFailureKind::read_failed,
                                          value.error_number);
            const auto closed = operations.close(operations.context, descriptor);
            if (closed.value != 0)
                result.cleanup.emplace(BoundedTaskStatFailure{
                    BoundedTaskStatFailureKind::close_failed, closed.error_number});
            return std::unexpected{std::move(result)};
        }
        const auto closed = operations.close(operations.context, descriptor);
        if (closed.value != 0)
            return std::unexpected{bounded_failure(
                BoundedTaskStatFailureKind::close_failed, closed.error_number)};
        return parse_bounded(std::string_view{workspace.data(), used}, thread);
    } catch (const std::exception&) {
        return std::unexpected{
            bounded_failure(BoundedTaskStatFailureKind::contained_exception)};
    } catch (...) {
        return std::unexpected{
            bounded_failure(BoundedTaskStatFailureKind::contained_unknown_failure)};
    }
}

auto TaskStatAccess::parse(std::string_view record, LinuxThreadId expected_thread_id)
    -> std::expected<LinuxTaskPageFaultCounters, core::Error> {
    auto value = parse_bounded(record, expected_thread_id);
    if (!value)
        return std::unexpected{bounded_to_core(value.error().primary)};
    return *value;
}

auto TaskStatAccess::read(LinuxThreadId thread_id, const TaskStatOperations& operations) noexcept
    -> std::expected<LinuxTaskPageFaultCounters, core::Error> {
    if (operations.read == nullptr) {
        return std::unexpected{
            task_stat_error("linux.task_stat.read_failed", "Unable to read Linux task-stat record",
                            "read_task_page_fault_counters", "invalid_operations")};
    }
    auto record = operations.read(operations.context, thread_id);
    if (!record) {
        return std::unexpected{std::move(record.error())};
    }
    try {
        return parse(*record, thread_id);
    } catch (const std::exception&) {
        return std::unexpected{
            task_stat_error("linux.task_stat.read_failed", "Unable to read Linux task-stat record",
                            "read_task_page_fault_counters", "contained_exception")};
    } catch (...) {
        return std::unexpected{
            task_stat_error("linux.task_stat.read_failed", "Unable to read Linux task-stat record",
                            "read_task_page_fault_counters", "contained_unknown_failure")};
    }
}

} // namespace detail

auto read_task_page_fault_counters(LinuxThreadId thread_id) noexcept
    -> std::expected<LinuxTaskPageFaultCounters, core::Error> {
    std::array<char, 16U * 1024U> workspace{};
    auto value = detail::TaskStatAccess::read_bounded(
        thread_id, workspace, detail::production_bounded_task_stat_operations());
    if (!value)
        return std::unexpected{detail::bounded_to_core(value.error().primary)};
    return *value;
}

} // namespace epycbench::platform_linux
