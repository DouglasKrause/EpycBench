#pragma once

#include "platform_linux/kernel_modules.hpp"
#include "runtime/package_admission.hpp"

namespace epycbench::runtime {
inline auto package_directory(std::optional<std::filesystem::path> directory)
    -> std::expected<std::filesystem::path, core::Error> {
    if (!directory) {
        auto found = platform_linux::kernel_modules::default_directory("kernel-packages");
        if (!found)
            return std::unexpected{core::Error{core::ErrorCategory::kernel,
                                               core::StableErrorCode{"kernel.package.directory"},
                                               std::string(found.error()),
                                               "discover_packages",
                                               {},
                                               nlohmann::json::object(),
                                               core::ErrorSeverity::error,
                                               core::ContinuationPolicy::abort_command}};
        directory = std::move(*found);
    }
    return *directory;
}

inline auto discover_packages(std::optional<std::filesystem::path> directory)
    -> std::expected<kernel::packages::Catalog, core::Error> {
    auto path = package_directory(std::move(directory));
    if (!path)
        return std::unexpected{path.error()};
    auto catalog = kernel::packages::Catalog::discover(*path);
    if (catalog)
        catalog->inspect_layouts = inspect_package_layouts;
    return catalog;
}
} // namespace epycbench::runtime
