#pragma once
#include "epycbench/core/error.hpp"

#include <string_view>
#include <expected>

namespace epycbench::platform_linux::detail { class ControllerSignalScope; }

namespace epycbench::platform_linux::package_process {
using Block = std::expected<nlohmann::json,core::Error>;
using Execute = Block (*)(const nlohmann::json&);

[[nodiscard]] auto collect(const nlohmann::json& request, const detail::ControllerSignalScope* signals = nullptr) -> Block;
[[nodiscard]] int serve(Execute);
}
