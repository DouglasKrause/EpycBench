#include "cli/terminal_menu.hpp"

#include <charconv>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace epycbench::cli {

auto run_terminal_menu(std::istream& input, std::ostream& output, std::ostream& error,
                       const MenuAction& help, const MenuAction& demo,
                       const MenuAction& inspect, const MenuAction& view,
                       std::span<const MenuEntry> additional) -> int {
    try {
        std::string line;
        for (;;) {
            output << "\nEpycBench\n\n"
                      "1) Help\n"
                      "2) Demo test run\n"
                      "3) Inspect this machine\n"
                      "4) View machine architecture\n"
                      "5) Exit\n";
            for (std::size_t index = 0; index < additional.size(); ++index)
                output << index + 6U << ") " << additional[index].label << '\n';
            output << "\nChoice: " << std::flush;
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
            if (!action && !additional.empty()) {
                std::size_t number{};
                const auto* end = choice.data() + choice.size();
                const auto parsed = std::from_chars(choice.data(), end, number);
                if (parsed.ec == std::errc{} && parsed.ptr == end && number >= 6U
                    && number - 6U < additional.size()) {
                    const auto& entry = additional[number - 6U];
                    action = &entry.action;
                    label = entry.label;
                }
            }
            if(action&&*action) {
                const auto status=(*action)();
                if(status!=0)error << label << " failed (exit code " << status << ").\n";
                if(choice=="2"||choice=="4") {
                    output << "Press Enter to return to the menu." << std::flush;
                    if(!std::getline(input,line))return input.eof()&&!input.bad()?0:1;
                }
            } else error << "Please enter a number from 1 to " << 5U + additional.size() << ".\n";
            if (!output || !error) return 1;
        }
    } catch (const std::ios_base::failure&) {
        return 1;
    }
}

} // namespace epycbench::cli
