#include "monitoring/privacy_validation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto normalized(std::string_view value) -> std::string {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(character == '-' ? '_' : static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))));
    return result;
}
} // namespace

auto privacy_prohibited(std::string_view value) -> bool {
    static constexpr std::array keys{
        "hostname", "username", "home", "project_path", "machine_id", "boot_id", "pid",
        "tid", "linux_tid", "perf_fd", "fd", "proc_path", "sysfs_path", "mmap_address",
        "raw_pointer", "serial", "serial_number", "mac", "mac_address", "gpu_uuid",
        "storage_serial", "process_listing"};
    const auto text = normalized(value);
    if (text.starts_with("/proc/") || text.starts_with("/sys/") ||
        text.starts_with("/home/") || text.contains('\\'))
        return true;
    std::string token;
    auto token_is_prohibited = [&] {
        return std::ranges::find(keys, std::string_view{token}) != keys.end();
    };
    for (const auto character : text) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_')
            token.push_back(character);
        else if (!token.empty()) {
            if (token_is_prohibited())
                return true;
            token.clear();
        }
    }
    return !token.empty() && token_is_prohibited();
}

auto privacy_safe(const nlohmann::json& value) -> bool {
    if (value.is_object()) {
        for (const auto& [key, child] : value.items())
            if (privacy_prohibited(key) || !privacy_safe(child))
                return false;
    } else if (value.is_array()) {
        for (const auto& child : value)
            if (!privacy_safe(child))
                return false;
    } else if (value.is_string() &&
               privacy_prohibited(value.get_ref<const std::string&>())) {
        return false;
    }
    return true;
}

} // namespace epycbench::monitoring::qualification::detail
