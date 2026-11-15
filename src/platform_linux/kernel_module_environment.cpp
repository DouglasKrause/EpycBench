#include "platform_linux/kernel_module_environment.hpp"
#include <cerrno>
#include <cstdlib>

namespace epycbench::platform_linux {
namespace { constexpr auto name = "EPYCBENCH_INTERNAL_MODULE_SESSION_V1"; }
auto publish_kernel_module_session(std::string_view text) -> std::expected<void, int> {
    const std::string owned{text};
    if (::setenv(name, owned.c_str(), 1) != 0) return std::unexpected{errno};
    return {};
}
auto inherited_kernel_module_session() -> std::optional<std::string> {
    const auto* value = ::getenv(name);
    if (!value) return {};
    return std::string{value};
}
}
