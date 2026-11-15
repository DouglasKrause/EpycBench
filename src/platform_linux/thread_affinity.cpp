#include "epycbench/platform_linux/thread_affinity.hpp"

#include "platform_linux/thread_operations.hpp"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <sched.h>
#include <string_view>
#include <utility>

namespace epycbench::platform_linux {
namespace detail {
namespace {

constexpr auto maximum_capacity =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;

[[nodiscard]] auto production_with_capacity(void*, std::size_t capacity)
    -> std::expected<DynamicCpuMask, core::Error> {
    return DynamicCpuMask::with_capacity(capacity);
}

[[nodiscard]] auto production_from_cpu_set(void*, const core::CpuSet& cpus)
    -> std::expected<DynamicCpuMask, core::Error> {
    return DynamicCpuMask::from_cpu_set(cpus);
}

[[nodiscard]] auto propagated_mask_error(std::string_view operation, const core::Error& error)
    -> core::Error {
    const auto& underlying = error.code.value();
    const auto classification = underlying == "linux.affinity_mask.allocation_failed"
                                    ? "allocation_failure"
                                    : "conversion_failed";
    return thread_error("linux.affinity.conversion_failed", "Affinity mask conversion failed",
                        operation, classification, std::nullopt, underlying);
}

[[nodiscard]] auto get_error(int error_number) -> core::Error {
    auto classification = std::string_view{"linux_error"};
    switch (error_number) {
    case EFAULT:
        classification = "bad_address";
        break;
    case ESRCH:
        classification = "thread_not_found";
        break;
    case EINTR:
        classification = "interrupted";
        break;
    default:
        break;
    }
    const auto captured =
        error_number == 0 ? std::optional<int>{} : std::optional<int>{error_number};
    return thread_error("linux.affinity.get_failed", "Current-thread affinity query failed",
                        "get_current_thread_affinity", classification, captured);
}

[[nodiscard]] auto set_error(int error_number) -> core::Error {
    auto classification = std::string_view{"linux_error"};
    switch (error_number) {
    case EINVAL:
        classification = "invalid_cpu_set";
        break;
    case EPERM:
        classification = "permission_denied";
        break;
    case ESRCH:
        classification = "thread_not_found";
        break;
    case EFAULT:
        classification = "bad_address";
        break;
    case EINTR:
        classification = "interrupted";
        break;
    default:
        break;
    }
    const auto captured =
        error_number == 0 ? std::optional<int>{} : std::optional<int>{error_number};
    return thread_error("linux.affinity.set_failed", "Current-thread affinity application failed",
                        "set_current_thread_affinity", classification, captured);
}

[[nodiscard]] auto readback_too_large() -> core::Error {
    return thread_error("linux.affinity.readback_too_large",
                        "Affinity readback exceeds the supported CPU identity domain",
                        "get_current_thread_affinity", "capacity_exhausted");
}

} // namespace

auto production_cpu_mask_operations() noexcept -> CpuMaskOperations {
    return CpuMaskOperations{
        .context = nullptr,
        .with_capacity = production_with_capacity,
        .from_cpu_set = production_from_cpu_set,
    };
}

auto ThreadAffinityAccess::get_current_thread_affinity(
    const ThreadAffinityDependencies& dependencies) -> std::expected<core::CpuSet, core::Error> {
    constexpr std::string_view operation = "get_current_thread_affinity";
    auto requested_capacity = static_cast<std::uint64_t>(CPU_SETSIZE);
    try {
        for (;;) {
            if (requested_capacity > maximum_capacity ||
                requested_capacity > std::numeric_limits<std::size_t>::max()) {
                return std::unexpected{readback_too_large()};
            }

            auto mask = dependencies.masks.with_capacity(
                dependencies.masks.context, static_cast<std::size_t>(requested_capacity));
            if (!mask) {
                return std::unexpected{propagated_mask_error(operation, mask.error())};
            }
            const auto result = dependencies.threads.get_affinity(dependencies.threads.context,
                                                                  mask->native_view());
            if (result.value == 0) {
                auto converted = mask->to_cpu_set();
                if (!converted) {
                    return std::unexpected{propagated_mask_error(operation, converted.error())};
                }
                if (converted->empty()) {
                    return std::unexpected{thread_error("linux.affinity.get_failed",
                                                        "Affinity readback was empty", operation,
                                                        "empty_readback")};
                }
                return converted;
            }
            if (result.error_number != EINVAL) {
                return std::unexpected{get_error(result.error_number)};
            }
            if (requested_capacity == maximum_capacity ||
                requested_capacity > maximum_capacity / 2U) {
                return std::unexpected{readback_too_large()};
            }
            requested_capacity *= 2U;
        }
    } catch (const std::exception&) {
        return std::unexpected{contained_thread_exception(operation)};
    } catch (...) {
        return std::unexpected{contained_unknown_thread_failure(operation)};
    }
}

auto ThreadAffinityAccess::set_current_thread_affinity(
    const core::CpuSet& cpus, const ThreadAffinityDependencies& dependencies)
    -> std::expected<void, core::Error> {
    constexpr std::string_view operation = "set_current_thread_affinity";
    try {
        if (cpus.empty()) {
            return std::unexpected{thread_error("linux.affinity.empty_request",
                                                "Affinity request must not be empty", operation,
                                                "empty_request")};
        }
        auto mask = dependencies.masks.from_cpu_set(dependencies.masks.context, cpus);
        if (!mask) {
            return std::unexpected{propagated_mask_error(operation, mask.error())};
        }
        const auto& const_mask = *mask;
        const auto result = dependencies.threads.set_affinity(dependencies.threads.context,
                                                              const_mask.native_view());
        if (result.value != 0) {
            return std::unexpected{set_error(result.error_number)};
        }
        return {};
    } catch (const std::exception&) {
        return std::unexpected{contained_thread_exception(operation)};
    } catch (...) {
        return std::unexpected{contained_unknown_thread_failure(operation)};
    }
}

} // namespace detail

auto get_current_thread_affinity() -> std::expected<core::CpuSet, core::Error> {
    return detail::ThreadAffinityAccess::get_current_thread_affinity(
        detail::ThreadAffinityDependencies{
            .threads = detail::production_thread_operations(),
            .masks = detail::production_cpu_mask_operations(),
        });
}

auto set_current_thread_affinity(const core::CpuSet& cpus) -> std::expected<void, core::Error> {
    return detail::ThreadAffinityAccess::set_current_thread_affinity(
        cpus, detail::ThreadAffinityDependencies{
                  .threads = detail::production_thread_operations(),
                  .masks = detail::production_cpu_mask_operations(),
              });
}

} // namespace epycbench::platform_linux
