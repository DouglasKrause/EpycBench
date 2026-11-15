#include "cli/json_csv.hpp"
#include "core/json_ownership.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace epycbench::cli {
namespace {
using Json = nlohmann::json;
constexpr std::size_t output_limit = 256U * 1024U * 1024U;

void append(std::string& output, std::string_view text) {
    if (text.size() > output_limit - output.size())
        throw std::length_error{"CSV output limit"};
    output.append(text);
}

void cell(std::string& output, std::string_view text) {
    append(output, "\"");
    for (std::size_t quote; (quote = text.find('"')) != std::string_view::npos;) {
        append(output, text.substr(0, quote));
        append(output, "\"\"");
        text.remove_prefix(quote + 1);
    }
    append(output, text);
    append(output, "\"");
}

void rows(const Json& value, const Json::json_pointer& path, std::string& output) {
    if (value.is_structured() && !value.empty()) {
        for (const auto& [key, child] : value.items()) rows(child, path / key, output);
        return;
    }
    cell(output, path.to_string());
    append(output, ",");
    cell(output, value.type_name());
    append(output, ",");
    if (value.is_string()) cell(output, value.get_ref<const std::string&>());
    else cell(output, value.dump());
    append(output, "\r\n");
}
} // namespace

auto json_to_csv(std::string_view json) -> std::expected<std::string, CsvError> {
    try {
        auto value = Json::parse(json);
        core::detail::JsonCleanup cleanup{value};
        std::string output{"path,type,value\r\n"};
        rows(value, Json::json_pointer{}, output);
        return output;
    } catch (const std::bad_alloc&) {
        return std::unexpected{CsvError::allocation};
    } catch (const std::length_error&) {
        return std::unexpected{CsvError::limits};
    } catch (const Json::exception&) {
        return std::unexpected{CsvError::invalid_json};
    }
}
} // namespace epycbench::cli
