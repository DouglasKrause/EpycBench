#include "epycbench/platform_linux/page_location.hpp"

#include "platform_linux/numa_operations.hpp"

#include <cerrno>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace epycbench::platform_linux {
namespace {

[[nodiscard]] auto query_error(std::string_view code, std::string_view message,
                               std::string_view classification,
                               std::optional<int> number = std::nullopt) -> core::Error {
    std::optional<core::OsError> os_error;
    if (number && *number != 0) {
        os_error = core::OsError::capture(std::error_code{*number, std::generic_category()});
    }
    return {.category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{std::string{code}},
            .message = std::string{message},
            .operation = "query_page_locations",
            .os_error = std::move(os_error),
            .context = nlohmann::json{{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

[[nodiscard]] auto query_classification(int number) -> std::string_view {
    switch (number) {
    case EACCES:
    case EPERM: return "permission_denied";
    case EFAULT:
    case EINVAL: return "invalid_query_request";
    case ESRCH: return "process_unavailable";
    case ENOMEM: return "resource_unavailable";
    case EINTR: return "interrupted";
    default: return "query_failure";
    }
}

auto allocate_status(std::size_t count) -> std::vector<int> { return std::vector<int>(count); }
auto allocate_results(std::size_t count) -> std::vector<PageQueryStatus> {
    auto results = std::vector<PageQueryStatus>{};
    results.reserve(count);
    return results;
}

} // namespace

PageQueryStatus::PageQueryStatus(PageQueryStatusKind kind,
                                 std::optional<core::LinuxNumaNodeId> node,
                                 std::optional<int> error_number) noexcept
    : kind_{kind}, node_{node}, error_number_{error_number} {}
auto PageQueryStatus::resident(core::LinuxNumaNodeId node) -> PageQueryStatus {
    return {PageQueryStatusKind::resident, node, std::nullopt};
}
auto PageQueryStatus::unresolved() -> PageQueryStatus {
    return {PageQueryStatusKind::unresolved, std::nullopt, ENOENT};
}
auto PageQueryStatus::error(int error_number) -> std::expected<PageQueryStatus, core::Error> {
    if (error_number <= 0 || error_number == ENOENT) {
        return std::unexpected{query_error("linux.numa.invalid_status",
                                           "Invalid per-page NUMA error status",
                                           "invalid_page_status")};
    }
    return PageQueryStatus{PageQueryStatusKind::error, std::nullopt, error_number};
}
auto PageQueryStatus::kind() const noexcept -> PageQueryStatusKind { return kind_; }
auto PageQueryStatus::node() const noexcept -> std::optional<core::LinuxNumaNodeId> { return node_; }
auto PageQueryStatus::error_number() const noexcept -> std::optional<int> { return error_number_; }

namespace detail {

auto NumaAccess::query(std::span<void* const> pages, const NumaOperations& operations,
                       StatusAllocator status_allocator, ResultAllocator result_allocator,
                       std::uint64_t native_count_max)
    -> std::expected<std::vector<PageQueryStatus>, core::Error> {
    if (pages.empty()) return std::vector<PageQueryStatus>{};
    for (auto* page : pages) {
        if (page == nullptr) {
            return std::unexpected{query_error("linux.numa.invalid_argument",
                                               "NUMA page query contains a null pointer",
                                               "invalid_page_pointer")};
        }
    }
    if (pages.size() > native_count_max) {
        return std::unexpected{query_error("linux.numa.conversion_failed",
                                           "NUMA page count is not representable",
                                           "count_out_of_range")};
    }
    try {
        auto statuses = status_allocator(pages.size());
        if (statuses.size() != pages.size()) throw std::bad_alloc{};
        const auto raw = operations.query_pages(operations.context, 0,
                                                static_cast<unsigned long>(pages.size()),
                                                const_cast<void**>(pages.data()), nullptr,
                                                statuses.data(), 0);
        if (raw.value == -1 && raw.error_number != 0) {
            return std::unexpected{query_error("linux.numa.query_failed",
                                               "Linux NUMA page query failed",
                                               query_classification(raw.error_number),
                                               raw.error_number)};
        }
        if (raw.value != 0 || raw.error_number != 0) {
            return std::unexpected{query_error("linux.numa.query_failed",
                                               "Linux NUMA page query returned inconsistent status",
                                               "unexpected_native_result")};
        }
        auto results = result_allocator(pages.size());
        for (const auto status : statuses) {
            if (status >= 0) {
                results.push_back(PageQueryStatus::resident(
                    core::LinuxNumaNodeId{static_cast<std::uint32_t>(status)}));
            } else if (status == -ENOENT) {
                results.push_back(PageQueryStatus::unresolved());
            } else {
                if (status == std::numeric_limits<int>::min()) {
                    return std::unexpected{query_error("linux.numa.invalid_status",
                                                       "Invalid Linux NUMA page status",
                                                       "invalid_page_status")};
                }
                auto converted = PageQueryStatus::error(-status);
                if (!converted) return std::unexpected{converted.error()};
                results.push_back(*converted);
            }
        }
        return results;
    } catch (const std::bad_alloc&) {
        return std::unexpected{query_error("linux.numa.conversion_failed",
                                           "NUMA query allocation failed", "allocation_failure")};
    } catch (const std::exception&) {
        return std::unexpected{query_error("linux.numa.contained_exception",
                                           "NUMA query contained an exception",
                                           "contained_exception")};
    } catch (...) {
        return std::unexpected{query_error("linux.numa.contained_unknown_failure",
                                           "NUMA query contained an unknown failure",
                                           "contained_unknown_failure")};
    }
}

} // namespace detail

auto query_page_locations(std::span<void* const> pages)
    -> std::expected<std::vector<PageQueryStatus>, core::Error> {
    return detail::NumaAccess::query(pages, detail::production_numa_operations(), allocate_status,
                                     allocate_results);
}

} // namespace epycbench::platform_linux
