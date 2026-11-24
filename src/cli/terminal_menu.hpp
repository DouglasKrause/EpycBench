#pragma once

#include <functional>
#include <iosfwd>
#include <span>
#include <string_view>

namespace epycbench::cli {

using MenuAction = std::function<int()>;
struct MenuEntry {
    std::string_view label;
    MenuAction action;
};

[[nodiscard]] auto run_terminal_menu(std::istream& input, std::ostream& output,
                                     std::ostream& error, const MenuAction& help,
                                     const MenuAction& demo,
                                     const MenuAction& inspect = {}, const MenuAction& view = {},
                                     std::span<const MenuEntry> additional = {}) -> int;

} // namespace epycbench::cli
