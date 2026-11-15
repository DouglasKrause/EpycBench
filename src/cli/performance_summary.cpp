#include "cli/performance_summary.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <nlohmann/json.hpp>
#include <vector>

namespace epycbench::cli {
namespace {
using Json = nlohmann::json;

auto text(const Json& value) -> std::string {
    if(value.is_string()) return value.get<std::string>();
    if(value.is_number()) return value.dump();
    return "?";
}
auto number(const Json& value, double scale = 1.0, double offset = 0.0) -> std::string {
    if(!value.is_number()) return "unavailable";
    const auto scaled = (value.get<double>() + offset) * scale;
    if(!std::isfinite(scaled)) return "unavailable";
    std::array<char,64> bytes{};
    const auto converted = std::to_chars(bytes.data(),bytes.data()+bytes.size(),scaled,
        std::chars_format::general,4);
    if(converted.ec != std::errc{}) return "unavailable";
    return {bytes.data(),converted.ptr};
}
auto conclusion(std::string_view value) -> std::string_view {
    if(value == "candidate_faster") return "candidate faster";
    if(value == "baseline_faster") return "baseline faster";
    if(value == "detectable_below_practical_threshold") return "small difference";
    if(value == "inconclusive") return "inconclusive";
    return "not evaluated";
}
auto index(const Json& value) -> std::size_t {
    if(value.is_number_unsigned()) return value.get<std::size_t>();
    if(value.is_number_integer() && value.get<std::int64_t>()>=0) return value.get<std::size_t>();
    const auto encoded = text(value);
    std::size_t result{};
    const auto parsed = std::from_chars(encoded.data(),encoded.data()+encoded.size(),result);
    return parsed.ec == std::errc{} && parsed.ptr == encoded.data()+encoded.size() ? result : 0;
}
auto source_experiment(const Json& source, const Json& experiment) -> const Json* {
    if(!source.contains("experiments") || !source["experiments"].is_array()) return nullptr;
    const auto selected = index(experiment.at("source_experiment"));
    return selected < source["experiments"].size() ? &source["experiments"][selected] : nullptr;
}
auto placement(const Json* source, std::size_t selected) -> std::string {
    const auto fallback = "variant " + std::to_string(selected);
    if(!source || !source->contains("placement")) return fallback;
    const auto& resolved = source->at("placement").at("resolved");
    if(!resolved.contains("variants") || selected >= resolved["variants"].size()) return fallback;
    const auto& variant = resolved["variants"][selected];
    auto label = variant.contains("variant_id") ? text(variant["variant_id"]) :
        variant.contains("id") ? text(variant["id"]) : fallback;
    if(variant.contains("memory") && variant["memory"].contains("nodes")) {
        const auto& nodes = variant["memory"]["nodes"];
        if(nodes.is_array() && !nodes.empty()) {
            label += " (N";
            for(std::size_t i{}; i < std::min(nodes.size(),std::size_t{3}); ++i) {
                if(i) label += ',';
                label += nodes[i].is_object() && nodes[i].contains("linux_node") ?
                    text(nodes[i]["linux_node"]) : text(nodes[i]);
            }
            if(nodes.size() > 3) label += ",...";
            label += ')';
        }
    }
    return label;
}
auto pair_lines(const Json& pair, const Json& experiment, const Json* source)
    -> std::vector<std::string> {
    const auto label = source && source->contains("experiment_id") ? text(source->at("experiment_id")) :
        "Test " + std::to_string(index(experiment.at("source_experiment"))+1);
    const auto& roles = pair.at("roles").at("value");
    const auto locations = roles.is_null() ? "placements unavailable" :
        placement(source,index(roles.at("baseline_index"))) + " -> " +
        placement(source,index(roles.at("candidate_index")));
    const auto& statistics = pair.at("statistics").at("value");
    if(statistics.is_null()) return {label + ": " + locations,"  Results unavailable (not started)."};
    const auto& calculated = statistics.at("calculated").at("value");
    if(calculated.is_null()) return {label + ": " + locations,
        "  Results unavailable (" + text(statistics.at("validity")) + ")."};
    const auto& metric = statistics.at("metric").at("value");
    const bool bandwidth = !metric.is_null() && metric.at("family") == "bandwidth";
    const bool latency = !metric.is_null() && metric.at("family") == "dependent_load_latency";
    const double scale = bandwidth ? 1e-9 : latency ? 1e9 : 1.0;
    const auto unit = bandwidth ? " GB/s" : latency ? " ns" : "";
    const auto& first = calculated.at("first").at("descriptive");
    const auto& second = calculated.at("second").at("descriptive");
    std::vector<std::string> lines{label + ": " + locations,
        "  Mean: " + number(first.at("mean"),scale) + unit + " -> " +
        number(second.at("mean"),scale) + unit + " | samples " + text(first.at("count")) +
        "/" + text(second.at("count"))};
    if(statistics.at("scope") != "derived_analysis" || statistics.at("validity") != "valid") {
        lines.push_back("  Diagnostic measurements only; comparison not valid/evaluated.");
        return lines;
    }
    const auto& percentage = calculated.at("presentation").at("percentage_change").at("value");
    auto change = number(percentage);
    if(percentage.is_number() && percentage.get<double>() > 0) change.insert(0,"+");
    const auto& confidence = calculated.at("confidence_interval");
    const auto& interval = confidence.at("ratio_interval");
    lines.push_back("  Change: " + change + (percentage.is_null() ? "" : "%") +
        " | " + number(confidence.at("policy").at("confidence"),100.0) + "% range: " +
        number(interval.at("lower"),100.0,-1.0) + "%.." +
        number(interval.at("upper"),100.0,-1.0) + "% | " +
        std::string(conclusion(text(statistics.at("conclusion")))));
    return lines;
}
}

auto render_performance_summary(std::string_view summary_json,std::size_t columns,
    std::size_t rows,std::string_view source_json) -> std::string {
    columns = std::clamp(columns,std::size_t{1},std::size_t{80});
    rows = std::clamp(rows,std::size_t{1},std::size_t{24});
    std::vector<std::string> lines;
    try {
        Json summary = Json::parse(summary_json);
        core::detail::JsonCleanup summary_cleanup{summary};
        Json source;
        core::detail::JsonCleanup source_cleanup{source};
        if(!source_json.empty()) source = Json::parse(source_json);
        lines.push_back("Performance summary");
        lines.push_back("Run: " + text(summary.at("status").at("execution")) + " | validity: " +
            text(summary.at("status").at("validity")));
        std::string warnings = "unavailable";
        if(source.contains("preflight") && source["preflight"].contains("warnings")) {
            const auto& value = source["preflight"]["warnings"]["value"];
            if(value.is_array()) warnings = std::to_string(value.size());
        }
        lines.push_back("Preflight warnings: " + warnings + " | Positive change = candidate better");
        std::size_t total{}, shown{};
        for(const auto& experiment : summary.at("experiments")) total += experiment.at("pairs").size();
        for(const auto& experiment : summary.at("experiments")) {
            for(const auto& pair : experiment.at("pairs")) {
                auto rendered = pair_lines(pair,experiment,source_experiment(source,experiment));
                // Reserve a final line for omitted comparisons and saved-file guidance.
                if(lines.size()+rendered.size()+1 <= rows) {
                    lines.insert(lines.end(),rendered.begin(),rendered.end());
                    ++shown;
                }
            }
        }
        if(shown < total) lines.push_back(std::to_string(total-shown) + " more comparison(s); see saved summary.");
        else lines.push_back(total ? "Full details are in the saved summary." : "No comparisons available; see saved result.");
    } catch(const Json::exception&) {
        lines = {"Performance summary unavailable; see saved result."};
    }
    std::string output;
    for(std::size_t i{}; i < std::min(rows,lines.size()); ++i) {
        auto line = std::move(lines[i]);
        for(char& ch : line) if(static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126) ch = '?';
        if(line.size() >= columns) {
            line.resize(columns > 1 ? columns-1 : 1);
            if(columns > 4) line.replace(line.size()-3,3,"...");
        }
        output += line;
        output += '\n';
    }
    return output;
}
} // namespace epycbench::cli
