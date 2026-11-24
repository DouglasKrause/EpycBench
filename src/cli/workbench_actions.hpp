#pragma once

#include "cli/terminal_menu.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace epycbench::topology::cli { struct Environment; }

namespace epycbench::cli {

// The dispatcher receives ordinary command-line words, including the program name.
using WorkbenchDispatch = std::function<int(std::span<const std::string>, std::ostream&)>;
// Publishes a new file at the supplied destination and reports any failure.
using WorkbenchPublish = std::function<int(const std::filesystem::path&, std::string_view)>;

[[nodiscard]] auto make_workbench_entries(std::istream& input, std::ostream& output,
                                          WorkbenchDispatch dispatch, WorkbenchPublish publish = {},
                                          MenuAction edit_scenario = {})
    -> std::array<MenuEntry, 19>;

[[nodiscard]] auto edit_saved_scenario(std::istream&, std::ostream&, std::ostream&, topology::cli::Environment&,
                                       const std::optional<std::filesystem::path>& module_directory = {}) -> int;

} // namespace epycbench::cli
