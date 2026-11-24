#include "platform_linux/kernel_modules.hpp"
#include <array>
#include <limits.h>
#include <unistd.h>

namespace epycbench::platform_linux::kernel_modules {
auto default_directory(std::string_view directory) -> std::expected<std::filesystem::path, std::string_view> {
    std::array<char, PATH_MAX> path{};
    const auto count = ::readlink("/proc/self/exe", path.data(), path.size());
    if (count < 0 || static_cast<std::size_t>(count) == path.size())
        return std::unexpected{"Cannot locate the executable's module directory"};
    return std::filesystem::path{std::string_view{path.data(), static_cast<std::size_t>(count)}}.parent_path() / directory;
}
} // namespace epycbench::platform_linux::kernel_modules
