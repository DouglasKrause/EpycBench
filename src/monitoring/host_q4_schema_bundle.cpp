#include "monitoring/host_q4_schema_bundle.hpp"
#include "monitoring/host_q4_schema_bundle_test_access.hpp"

#include "epycbench/core/canonical_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Json = nlohmann::json;
constexpr std::size_t input_limit = 65'536;
constexpr std::size_t resolved_limit = 2U * 1024U * 1024U;
constexpr std::size_t node_limit = 100'000;
constexpr std::size_t depth_limit = 64;
thread_local auto fault = MonitoringQ4SchemaBundleTestAccess::Fault::none;
constexpr std::array filenames{
    std::string_view{"monitoring-q4-frequency-collection-plan-v1.schema.json"},
    std::string_view{"monitoring-host-q4-frequency-evidence-v1.schema.json"},
    std::string_view{"monitoring-host-q4-backend-result-v1.schema.json"},
    std::string_view{"monitoring-host-q4-backend-error-v1.schema.json"},
    std::string_view{"monitoring-host-q4-backend-authored-v1.schema.json"}};
constexpr std::array identities{
    std::string_view{"epycbench.monitoring-q4-frequency-collection-plan/v1"},
    std::string_view{"epycbench.monitoring-host-q4-frequency-evidence/v1"},
    std::string_view{"epycbench.monitoring-host-q4-backend-result/v1"},
    std::string_view{"epycbench.monitoring-host-q4-backend-error/v1"},
    std::string_view{"epycbench.monitoring-host-q4-backend-authored/v1"}};

struct Resolver final {
    const std::array<Json, 5>& documents;
    std::size_t& observed_nodes;
    std::size_t nodes{};
    std::size_t active_index{};
    MonitoringQ4SchemaBundleFailureKind failure{MonitoringQ4SchemaBundleFailureKind::reference};

    Resolver(const std::array<Json, 5>& values, std::size_t& observed) noexcept
        : documents{values}, observed_nodes{observed} {}

    auto fail(const MonitoringQ4SchemaBundleFailureKind kind) -> std::optional<Json> {
        failure = kind;
        return std::nullopt;
    }
    auto cross_target(const std::size_t document, const std::string_view reference)
        -> std::optional<std::size_t> {
        if (document == 1 && reference == filenames[0])
            return 0;
        if (document == 2 && reference == filenames[1])
            return 1;
        return std::nullopt;
    }
    auto append(std::string& output, const std::string_view value) -> bool {
        if (value.size() > resolved_limit - output.size()) {
            failure = MonitoringQ4SchemaBundleFailureKind::resolved_byte_limit;
            return false;
        }
        output.append(value);
        return true;
    }
    auto write(const Json& node, const std::size_t document, const std::size_t depth,
               std::vector<std::pair<std::size_t, std::string>>& stack, std::string& output)
        -> bool {
        active_index = document;
        ++nodes;
        observed_nodes = nodes;
        if (nodes > node_limit) {
            failure = MonitoringQ4SchemaBundleFailureKind::node_limit;
            return false;
        }
        if (depth > depth_limit) {
            failure = MonitoringQ4SchemaBundleFailureKind::depth_limit;
            return false;
        }
        if (node.is_array()) {
            if (!append(output, "["))
                return false;
            for (std::size_t index = 0; index < node.size(); ++index) {
                if (index != 0 && !append(output, ","))
                    return false;
                if (!write(node[index], document, depth + 1, stack, output))
                    return false;
            }
            return append(output, "]");
        }
        if (!node.is_object()) {
            const auto encoded = core::canonicalize_json(node);
            if (!encoded) {
                failure = MonitoringQ4SchemaBundleFailureKind::canonical;
                return false;
            }
            return append(output, *encoded);
        }
        if (node.contains("$ref")) {
            if (node.size() != 1 || !node.at("$ref").is_string()) {
                failure = MonitoringQ4SchemaBundleFailureKind::reference;
                return false;
            }
            const auto reference = node.at("$ref").get<std::string>();
            constexpr std::string_view prefix{"#/$defs/"};
            if (reference.starts_with(prefix)) {
                const auto name = reference.substr(prefix.size());
                if (name.empty() || name.contains('/') || name.contains('~') ||
                    !documents[document].contains("$defs") ||
                    !documents[document].at("$defs").is_object() ||
                    !documents[document].at("$defs").contains(name)) {
                    failure = MonitoringQ4SchemaBundleFailureKind::reference;
                    return false;
                }
                const auto marker = std::pair{document, name};
                if (std::find(stack.begin(), stack.end(), marker) != stack.end()) {
                    failure = MonitoringQ4SchemaBundleFailureKind::cycle;
                    return false;
                }
                stack.push_back(marker);
                const auto result = write(documents[document].at("$defs").at(name), document,
                                          depth + 1, stack, output);
                stack.pop_back();
                return result;
            }
            const auto target = cross_target(document, reference);
            if (!target) {
                failure = MonitoringQ4SchemaBundleFailureKind::reference;
                return false;
            }
            const auto marker = std::pair{*target, std::string{"$"}};
            if (std::find(stack.begin(), stack.end(), marker) != stack.end()) {
                failure = MonitoringQ4SchemaBundleFailureKind::cycle;
                return false;
            }
            stack.push_back(marker);
            const auto result = write(documents[*target], *target, depth + 1, stack, output);
            stack.pop_back();
            return result;
        }
        if (!append(output, "{"))
            return false;
        bool first = true;
        for (auto iterator = node.begin(); iterator != node.end(); ++iterator) {
            if (iterator.key() == "$defs" || iterator.key() == "$id" || iterator.key() == "$schema")
                continue;
            if (!first && !append(output, ","))
                return false;
            first = false;
            const auto key = core::canonicalize_json(Json(iterator.key()));
            if (!key) {
                failure = MonitoringQ4SchemaBundleFailureKind::canonical;
                return false;
            }
            if (!append(output, *key) || !append(output, ":") ||
                !write(iterator.value(), document, depth + 1, stack, output))
                return false;
        }
        return append(output, "}");
    }
};

auto preflight(const Json& schema, const std::size_t depth = 0) -> bool {
    if (depth > depth_limit)
        return false;
    if (schema.is_boolean())
        return true;
    if (!schema.is_object())
        return false;
    static const std::set<std::string, std::less<>> allowed{"$schema",     "$id",
                                                            "title",       "description",
                                                            "default",     "examples",
                                                            "type",        "required",
                                                            "properties",  "additionalProperties",
                                                            "const",       "enum",
                                                            "pattern",     "minimum",
                                                            "maximum",     "exclusiveMinimum",
                                                            "minLength",   "maxLength",
                                                            "minItems",    "maxItems",
                                                            "uniqueItems", "items",
                                                            "prefixItems", "oneOf",
                                                            "anyOf",       "allOf"};
    for (auto iterator = schema.begin(); iterator != schema.end(); ++iterator)
        if (!allowed.contains(iterator.key()))
            return false;
    if (schema.contains("type")) {
        const auto valid = [](const Json& value) {
            static const std::set<std::string, std::less<>> types{
                "object", "array", "string", "integer", "number", "boolean", "null"};
            return value.is_string() && types.contains(value.get_ref<const std::string&>());
        };
        const auto& type = schema.at("type");
        if (type.is_array()) {
            if (type.empty() || !std::all_of(type.begin(), type.end(), valid))
                return false;
        } else if (!valid(type))
            return false;
    }
    if (schema.contains("required") &&
        (!schema.at("required").is_array() ||
         !std::all_of(schema.at("required").begin(), schema.at("required").end(),
                      [](const Json& value) { return value.is_string(); })))
        return false;
    if (schema.contains("enum") && (!schema.at("enum").is_array() || schema.at("enum").empty()))
        return false;
    if (schema.contains("uniqueItems") && !schema.at("uniqueItems").is_boolean())
        return false;
    if (schema.contains("properties")) {
        if (!schema.at("properties").is_object())
            return false;
        for (const auto& child : schema.at("properties"))
            if (!preflight(child, depth + 1))
                return false;
    }
    if (schema.contains("additionalProperties") &&
        !schema.at("additionalProperties").is_boolean() &&
        !preflight(schema.at("additionalProperties"), depth + 1))
        return false;
    if (schema.contains("items") && !preflight(schema.at("items"), depth + 1))
        return false;
    for (const auto keyword : {"prefixItems", "oneOf", "anyOf", "allOf"}) {
        if (!schema.contains(keyword))
            continue;
        const auto& branches = schema.at(keyword);
        if (!branches.is_array() ||
            (std::string_view{keyword} != "prefixItems" && branches.empty()))
            return false;
        for (const auto& branch : branches)
            if (!preflight(branch, depth + 1))
                return false;
    }
    return true;
}
} // namespace

auto resolve_monitoring_q4_schema_bundle(
    const std::array<MonitoringQ4SchemaInput, MonitoringQ4ResolvedSchemaBundle::schema_count>&
        inputs)
    -> std::expected<MonitoringQ4ResolvedSchemaBundle, MonitoringQ4SchemaBundleFailure> {
    std::size_t active_index{};
    std::size_t observed_nodes{};
    try {
        std::array<Json, 5> documents;
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            active_index = index;
            if (fault == MonitoringQ4SchemaBundleTestAccess::Fault::parse_allocation)
                throw std::bad_alloc{};
            if (fault == MonitoringQ4SchemaBundleTestAccess::Fault::contained)
                throw 1;
            if (inputs[index].filename != filenames[index])
                return std::unexpected{MonitoringQ4SchemaBundleFailure{
                    MonitoringQ4SchemaBundleFailureKind::association, index}};
            const auto bytes = inputs[index].bytes;
            if (bytes.empty() || bytes.size() > input_limit || bytes.back() != '\n')
                return std::unexpected{MonitoringQ4SchemaBundleFailure{
                    MonitoringQ4SchemaBundleFailureKind::byte_limit, index}};
            documents[index] = Json::parse(bytes.begin(), bytes.end());
            const auto canonical = core::canonicalize_json(documents[index]);
            if (!canonical || *canonical + "\n" != bytes)
                return std::unexpected{MonitoringQ4SchemaBundleFailure{
                    MonitoringQ4SchemaBundleFailureKind::canonical, index}};
            if (!documents[index].is_object() || !documents[index].contains("$schema") ||
                documents[index].at("$schema") != "https://json-schema.org/draft/2020-12/schema" ||
                !documents[index].contains("$id") ||
                documents[index].at("$id") != identities[index])
                return std::unexpected{MonitoringQ4SchemaBundleFailure{
                    MonitoringQ4SchemaBundleFailureKind::identity, index}};
        }
        std::array<std::string, 5> resolved;
        std::array<std::size_t, 5> visited_nodes{};
        for (std::size_t index = 0; index < documents.size(); ++index) {
            observed_nodes = 0;
            Resolver resolver{documents, observed_nodes};
            active_index = index;
            if (fault == MonitoringQ4SchemaBundleTestAccess::Fault::resolve_allocation)
                throw std::bad_alloc{};
            resolver.active_index = index;
            if (documents[index].contains("$defs")) {
                if (!documents[index].at("$defs").is_object())
                    return std::unexpected{MonitoringQ4SchemaBundleFailure{
                        MonitoringQ4SchemaBundleFailureKind::preflight, index}};
                for (auto iterator = documents[index].at("$defs").begin();
                     iterator != documents[index].at("$defs").end(); ++iterator) {
                    if (iterator.key().empty())
                        return std::unexpected{MonitoringQ4SchemaBundleFailure{
                            MonitoringQ4SchemaBundleFailureKind::preflight, index}};
                    std::vector<std::pair<std::size_t, std::string>> stack{{index, iterator.key()}};
                    std::string bytes;
                    if (!resolver.write(iterator.value(), index, 1, stack, bytes))
                        return std::unexpected{MonitoringQ4SchemaBundleFailure{
                            resolver.failure, resolver.active_index, resolver.nodes}};
                    const auto value = Json::parse(bytes);
                    if (!preflight(value))
                        return std::unexpected{MonitoringQ4SchemaBundleFailure{
                            MonitoringQ4SchemaBundleFailureKind::preflight, index}};
                }
            }
            std::vector<std::pair<std::size_t, std::string>> stack{{index, "$"}};
            std::string bytes;
            if (!resolver.write(documents[index], index, 0, stack, bytes))
                return std::unexpected{MonitoringQ4SchemaBundleFailure{
                    resolver.failure, resolver.active_index, resolver.nodes}};
            const auto value = Json::parse(bytes);
            if (!preflight(value))
                return std::unexpected{MonitoringQ4SchemaBundleFailure{
                    MonitoringQ4SchemaBundleFailureKind::preflight, index}};
            if (fault == MonitoringQ4SchemaBundleTestAccess::Fault::encode_allocation)
                throw std::bad_alloc{};
            resolved[index] = std::move(bytes);
            visited_nodes[index] = resolver.nodes;
        }
        return MonitoringQ4ResolvedSchemaBundle{std::move(resolved), visited_nodes};
    } catch (const nlohmann::json::parse_error&) {
        return std::unexpected{MonitoringQ4SchemaBundleFailure{
            MonitoringQ4SchemaBundleFailureKind::canonical, active_index, observed_nodes}};
    } catch (const std::bad_alloc&) {
        return std::unexpected{MonitoringQ4SchemaBundleFailure{
            MonitoringQ4SchemaBundleFailureKind::allocation, active_index, observed_nodes}};
    } catch (...) {
        return std::unexpected{MonitoringQ4SchemaBundleFailure{
            MonitoringQ4SchemaBundleFailureKind::contained, active_index, observed_nodes}};
    }
}

void MonitoringQ4SchemaBundleTestAccess::set_fault(const Fault value) noexcept { fault = value; }

auto MonitoringQ4SchemaBundleTestAccess::resolved_bytes(
    const MonitoringQ4ResolvedSchemaBundle& bundle, const MonitoringQ4SchemaKind kind) noexcept
    -> std::optional<std::string_view> {
    switch (kind) {
    case MonitoringQ4SchemaKind::plan:
        return bundle.plan_schema();
    case MonitoringQ4SchemaKind::evidence:
        return bundle.evidence_schema();
    case MonitoringQ4SchemaKind::backend_result:
        return bundle.backend_result_schema();
    case MonitoringQ4SchemaKind::backend_error:
        return bundle.backend_error_schema();
    case MonitoringQ4SchemaKind::backend_authored:
        return bundle.backend_authored_schema();
    }
    return std::nullopt;
}

auto MonitoringQ4SchemaBundleTestAccess::visited_nodes(
    const MonitoringQ4ResolvedSchemaBundle& bundle, const MonitoringQ4SchemaKind kind) noexcept
    -> std::optional<std::size_t> {
    switch (kind) {
    case MonitoringQ4SchemaKind::plan:
        return bundle.visited_nodes_[0];
    case MonitoringQ4SchemaKind::evidence:
        return bundle.visited_nodes_[1];
    case MonitoringQ4SchemaKind::backend_result:
        return bundle.visited_nodes_[2];
    case MonitoringQ4SchemaKind::backend_error:
        return bundle.visited_nodes_[3];
    case MonitoringQ4SchemaKind::backend_authored:
        return bundle.visited_nodes_[4];
    }
    return std::nullopt;
}

} // namespace epycbench::monitoring::qualification::detail
