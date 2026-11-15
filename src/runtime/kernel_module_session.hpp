#pragma once
#include "epycbench/kernel/sequential_read_bandwidth.hpp"
#include "epycbench/kernel/dependent_load_latency.hpp"
#include "epycbench/kernel/kernel_id.hpp"
#include "epycbench/kernel/descriptor.hpp"
#include "platform_linux/kernel_modules.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace epycbench::runtime {
[[nodiscard]] auto initialize_kernel_modules(std::optional<std::filesystem::path> directory = {})
    -> std::expected<void, std::string>;
[[nodiscard]] auto initialize_runner_kernel_modules() -> std::expected<void, std::string>;
// Hidden native qualification entrypoints use their existing untimed ownership
// and deadline fences, retaining an inherited catalog when one was supplied.
[[nodiscard]] auto initialize_qualification_kernel_modules() -> std::expected<void, std::string>;
[[nodiscard]] auto kernel_module_catalog() noexcept
    -> const platform_linux::kernel_modules::Catalog*;
enum class ModuleBindingError { no_session, unavailable, family, preparation };
// The fixed family verification remains authoritative; the module hook must
// agree with its recorded expected/actual facts. Call only after timing ends.
template<class Prepared, class Verification>
[[nodiscard]] auto module_verification_matches(const Prepared& prepared,
                                               const Verification& verification) noexcept -> bool {
    const auto* api = prepared.module();
    return !api || ((api->verify(verification.expected().value(), verification.actual().value()) == 0)
        == verification.matched());
}
[[nodiscard]] auto bind_kernel_module(kernel::PreparedSequentialReadBandwidth&, kernel::KernelId)
    -> std::expected<void, ModuleBindingError>;
[[nodiscard]] auto bind_kernel_module(kernel::PreparedDependentLoadLatency&, kernel::KernelId)
    -> std::expected<void, ModuleBindingError>;
[[nodiscard]] auto bind_kernel_module(kernel::PreparedSequentialReadBandwidth&, kernel::KernelImplementationId)
    -> std::expected<void, ModuleBindingError>;
[[nodiscard]] auto bind_kernel_module(kernel::PreparedDependentLoadLatency&, kernel::KernelImplementationId)
    -> std::expected<void, ModuleBindingError>;
}
