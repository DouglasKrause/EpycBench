#pragma once
#include "kernel/package_manifest.hpp"
#include "platform_linux/package_workers.hpp"

namespace epycbench::runtime {
[[nodiscard]] auto package_input_policy(std::string_view)
    -> platform_linux::package_execution::InputPolicy;
[[nodiscard]] auto package_input_policy_name(platform_linux::package_execution::InputPolicy)
    -> std::string_view;
[[nodiscard]] auto package_manifest_paths(const nlohmann::json&, const kernel::packages::Catalog&)
    -> std::vector<std::filesystem::path>;
[[nodiscard]] auto package_roles(const nlohmann::json&, const kernel::packages::Catalog&)
    -> std::expected<std::vector<platform_linux::package_execution::Role>, core::Error>;
[[nodiscard]] auto inspect_package_layouts(const kernel::packages::Catalog&, const nlohmann::json&,
                                           std::uint64_t page_bytes)
    -> std::expected<nlohmann::json, core::Error>;
[[nodiscard]] auto inspect_in_runner(const nlohmann::json&)
    -> std::expected<nlohmann::json, core::Error>;
} // namespace epycbench::runtime
