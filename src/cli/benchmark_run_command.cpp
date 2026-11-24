#include "cli/benchmark_run_command.hpp"

#include <algorithm>
#include <charconv>

namespace epycbench::cli::benchmark_run_command {
auto parse_arguments(std::span<const std::string_view> arguments) noexcept
    -> std::expected<Arguments,ArgumentError> {
    const auto invalid=std::unexpected{ArgumentError::usage};
    if(arguments.size()==1 && (arguments[0]=="--help" || arguments[0]=="-h"))
        return Arguments{{},scenario::ScenarioFormat::yaml,true};
    if(arguments.empty() || arguments.size()>10)return invalid;
    Arguments result;
    std::optional<scenario::ScenarioFormat> explicit_format;
    bool positional_only{};
    for(std::size_t i=0;i<arguments.size();++i) {
        const auto argument=arguments[i];
        if(argument.empty() || argument.size()>4095 || argument.find('\0')!=std::string_view::npos)
            return invalid;
        if(!positional_only && argument=="--") {positional_only=true;continue;}
        if(!positional_only && argument=="--expect-plan") {
            if(!result.expected_plan_digest.empty() || ++i==arguments.size())return invalid;
            const auto digest=arguments[i];
            if(!digest.starts_with("sha256:") || digest.size()!=71U ||
               !std::ranges::all_of(digest.substr(7),[](char c) { return (c>='0'&&c<='9') || (c>='a'&&c<='f'); }))
                return invalid;
            result.expected_plan_digest=digest;
            continue;
        }
        if (!positional_only && argument == "--package-energy") {
            if (result.package_energy || ++i == arguments.size()) return invalid;
            const auto text = arguments[i];
            std::uint32_t number{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                (text.size() > 1U && text.front() == '0')) return invalid;
            result.package_energy = number;
            continue;
        }
        if (!positional_only && argument == "--package-energy-bound") {
            if (!result.package_increment_bound.empty() || ++i == arguments.size()) return invalid;
            const auto path = arguments[i];
            if (path.empty() || path.size() > 4095 || path.find('\0') != path.npos || path.starts_with('-')) return invalid;
            result.package_increment_bound = path;
            continue;
        }
        if(!positional_only && (argument=="--format" || argument.starts_with("--format="))) {
            if(explicit_format)return invalid;
            std::string_view value;
            if(argument=="--format") {
                if(++i==arguments.size())return invalid;
                value=arguments[i];
            } else value=argument.substr(9);
            if(value=="yaml")explicit_format=scenario::ScenarioFormat::yaml;
            else if(value=="json")explicit_format=scenario::ScenarioFormat::json;
            else return invalid;
            continue;
        }
        if(argument=="-" || (!positional_only && argument.front()=='-') || !result.scenario_path.empty())
            return invalid;
        result.scenario_path=argument;
    }
    if(result.scenario_path.empty() || (!result.package_increment_bound.empty() && !result.package_energy))return invalid;
    const auto leaf=result.scenario_path.substr(result.scenario_path.find_last_of('/')+1);
    const auto dot=leaf.find_last_of('.');
    const auto extension=dot==std::string_view::npos || dot==0?std::string_view{}:leaf.substr(dot);
    std::optional<scenario::ScenarioFormat> inferred;
    if(extension==".yaml" || extension==".yml")inferred=scenario::ScenarioFormat::yaml;
    else if(extension==".json")inferred=scenario::ScenarioFormat::json;
    if((!inferred && !explicit_format) || (inferred && explicit_format && inferred!=explicit_format))
        return invalid;
    result.format=explicit_format?*explicit_format:*inferred;
    return result;
}

} // namespace epycbench::cli::benchmark_run_command
