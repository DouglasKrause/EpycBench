#include "cli/terminal_menu.hpp"

#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace epycbench::cli {

auto run_terminal_menu(std::istream& input, std::ostream& output, std::ostream& error,
                       const MenuAction& help, const MenuAction& demo) -> int {
    try {
        std::string line;
        for (;;) {
            output << "\nEpycBench\n\n"
                      "1) Help\n"
                      "2) Demo test run\n"
                      "3) Exit\n\n"
                      "Choice: " << std::flush;
            if (!output) return 1;
            if (!std::getline(input, line)) return input.eof() && !input.bad() ? 0 : 1;

            std::string_view choice{line};
            constexpr std::string_view whitespace{" \t\r\n\v\f"};
            const auto first = choice.find_first_not_of(whitespace);
            if (first != std::string_view::npos)
                choice = choice.substr(first, choice.find_last_not_of(whitespace) - first + 1);

            if (choice == "3") return 0;
            if (choice == "1" || choice == "2") {
                const auto status = choice == "1" ? help() : demo();
                if (status != 0)
                    error << (choice == "1" ? "Help" : "Demo test run")
                          << " failed (exit code " << status << ").\n";
            } else {
                error << "Please enter 1, 2, or 3.\n";
            }
            if (!output || !error) return 1;
        }
    } catch (const std::ios_base::failure&) {
        return 1;
    }
}

} // namespace epycbench::cli
