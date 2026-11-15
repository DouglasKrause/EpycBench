#pragma once

#include <functional>
#include <iosfwd>

namespace epycbench::cli {

using MenuAction = std::function<int()>;

[[nodiscard]] auto run_terminal_menu(std::istream& input, std::ostream& output,
                                     std::ostream& error, const MenuAction& help,
                                     const MenuAction& demo) -> int;

} // namespace epycbench::cli
