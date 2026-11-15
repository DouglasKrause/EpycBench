#include "cli/terminal_menu.hpp"

#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace epycbench::cli {

auto run_terminal_menu(std::istream& input, std::ostream& output, std::ostream& error,
                       const MenuAction& help, const MenuAction& demo,
                       const MenuAction& inspect, const MenuAction& view) -> int {
    try {
        std::string line;
        for (;;) {
            output << "\nEpycBench\n\n"
                      "1) Help\n"
                      "2) Demo test run\n"
                      "3) Inspect this machine\n"
                      "4) View machine architecture\n"
                      "5) Exit\n\n"
                      "Choice: " << std::flush;
            if (!output) return 1;
            if (!std::getline(input, line)) return input.eof() && !input.bad() ? 0 : 1;

            std::string_view choice{line};
            constexpr std::string_view whitespace{" \t\r\n\v\f"};
            const auto first = choice.find_first_not_of(whitespace);
            if (first != std::string_view::npos)
                choice = choice.substr(first, choice.find_last_not_of(whitespace) - first + 1);

            if (choice == "5") return 0;
            const MenuAction* action=nullptr;
            std::string_view label;
            if(choice=="1"){action=&help;label="Help";}
            if(choice=="2"){action=&demo;label="Demo test run";}
            if(choice=="3"){action=&inspect;label="Machine inspection";}
            if(choice=="4"){action=&view;label="Machine architecture";}
            if(action&&*action) {
                const auto status=(*action)();
                if(status!=0)error << label << " failed (exit code " << status << ").\n";
                if(choice=="2"||choice=="4") {
                    output << "Press Enter to return to the menu." << std::flush;
                    if(!std::getline(input,line))return input.eof()&&!input.bad()?0:1;
                }
            } else error << "Please enter a number from 1 to 5.\n";
            if (!output || !error) return 1;
        }
    } catch (const std::ios_base::failure&) {
        return 1;
    }
}

} // namespace epycbench::cli
