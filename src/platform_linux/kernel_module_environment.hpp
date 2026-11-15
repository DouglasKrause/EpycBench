#pragma once
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace epycbench::platform_linux {
[[nodiscard]] auto publish_kernel_module_session(std::string_view) -> std::expected<void, int>;
[[nodiscard]] auto inherited_kernel_module_session() -> std::optional<std::string>;
}
