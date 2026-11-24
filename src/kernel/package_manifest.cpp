#include "kernel/package_manifest.hpp"
#include "core/binary_digest.hpp"

#include "core/json_ownership.hpp"
#include "epycbench/kernel/package.h"
#include "scenario/restricted_yaml_parser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

namespace epycbench::kernel::packages {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;
using Path = std::filesystem::path;

constexpr std::string_view implementation_suffix = ".kernel.yaml";
constexpr std::string_view task_suffix = ".task.yaml";
constexpr std::size_t manifest_limit = 1024U * 1024U;
constexpr std::size_t name_limit = 127U;

auto error(std::string_view code, std::string message, std::string_view field,
           const Path& path = {}, const std::error_code& os_error = {})
    -> std::unexpected<core::Error> {
    auto context = Json::object();
    if (!field.empty()) context["field"] = field;
    if (!path.empty()) context["path"] = path.string();
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::kernel,
        .code = core::StableErrorCode{"kernel.package." + std::string{code}},
        .message = std::move(message),
        .operation = "kernel_package_manifest",
        .os_error = os_error ? std::optional{core::OsError::capture(os_error)} : std::nullopt,
        .context = std::move(context),
    }};
}

auto file_error(std::string message, const Path& path, const std::error_code& os_error = {})
    -> std::unexpected<core::Error> {
    return error("manifest_io", std::move(message), {}, path, os_error);
}

auto manifest_error(std::string message, std::string_view field, const Path& path)
    -> std::unexpected<core::Error> {
    return error("invalid_manifest", std::move(message), field, path);
}

auto open_file(const Path& path) -> std::expected<std::ifstream, core::Error> {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)))
        return file_error("Expected a regular package file", path, ec);
    std::ifstream stream{path, std::ios::binary};
    if (!stream) return file_error("Cannot open package file", path);
    return stream;
}

auto read_manifest(const Path& path) -> std::expected<std::string, core::Error> {
    auto source = open_file(path);
    if (!source) return std::unexpected{std::move(source.error())};
    std::array<char, 16384> buffer{};
    std::string contents;
    do {
        source->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(source->gcount());
        if (count > manifest_limit - contents.size())
            return manifest_error("Package manifest exceeds the 1 MiB size limit", {}, path);
        contents.append(buffer.data(), count);
    } while (*source);
    if (source->bad() || !source->eof()) return file_error("Cannot read package manifest", path);
    return contents;
}

auto digest_file(const Path& path) -> std::expected<core::Sha256Digest, core::Error> {
    auto source = open_file(path);
    if (!source) return std::unexpected{std::move(source.error())};
    core::BinaryDigest digest;
    std::array<char, 65536> buffer{};
    std::uint64_t total{};
    do {
        source->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto block = std::span{buffer.data(), static_cast<std::size_t>(source->gcount())};
        total += block.size();
        if (total > maximum_binary_bytes || !digest.append(std::as_bytes(block)))
            return file_error("Package binary exceeds the size limit", path);
    } while (*source);
    if (source->bad() || !source->eof()) return file_error("Cannot read package binary", path);
    if (total == 0) return file_error("Package binary is empty", path);
    auto identity=digest.finish();
    if (!identity) return file_error("Cannot finish hashing package binary",path);
    return *identity;
}

auto plain_character(char value) -> bool {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '.' || value == '-' || value == '_';
}

auto valid_id(std::string_view value) -> bool {
    return !value.empty() && value.size() <= name_limit && value.front() != '/' &&
           value.back() != '/' && !value.contains("//") &&
           std::ranges::all_of(value, [](char c) { return plain_character(c) || c == '/'; });
}

auto valid_name(std::string_view value) -> bool {
    return !value.empty() && value.size() <= name_limit && std::ranges::all_of(value, plain_character);
}

auto parameter_type(std::string_view value) -> std::optional<ParameterType> {
    if (value == "uint64") return ParameterType::uint64;
    if (value == "int64") return ParameterType::int64;
    if (value == "float64") return ParameterType::float64;
    if (value == "bool") return ParameterType::boolean;
    if (value == "enum") return ParameterType::enumeration;
    return std::nullopt;
}

template <typename Value>
auto normalize_value(ParameterType type, const Value& value) -> std::optional<Json> {
    switch (type) {
    case ParameterType::uint64:
        if (value.is_number_unsigned()) return Json(value.template get<std::uint64_t>());
        if (value.is_number_integer() && value.template get<std::int64_t>() >= 0)
            return Json(static_cast<std::uint64_t>(value.template get<std::int64_t>()));
        break;
    case ParameterType::int64:
        if (value.is_number_unsigned() &&
            value.template get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) break;
        if (value.is_number_integer()) return Json(value.template get<std::int64_t>());
        break;
    case ParameterType::float64:
        if (value.is_number_float() && std::isfinite(value.template get<double>()))
            return Json(value.template get<double>());
        break;
    case ParameterType::boolean:
        if (value.is_boolean()) return Json(value.template get<bool>());
        break;
    case ParameterType::enumeration:
        if (value.is_string()) return Json(value.template get<std::string>());
        break;
    }
    return std::nullopt;
}

auto in_domain(const ParameterDefinition& definition, const Json& value) -> bool {
    if (definition.type == ParameterType::enumeration)
        return std::ranges::contains(definition.values, value.get_ref<const std::string&>());
    return (!definition.minimum || value >= *definition.minimum) &&
           (!definition.maximum || value <= *definition.maximum);
}

auto normalize_accepted_values(AcceptedValues& accepts, const ParameterDefinitions& definitions,
                               const Path& path) -> std::expected<void, core::Error> {
    for (auto& [parameter, values] : accepts) {
        const auto field = "accepts." + parameter;
        const auto metadata = definitions.find(parameter);
        if (metadata == definitions.end())
            return manifest_error("Implementation restricts an unknown task parameter", field, path);
        const auto& definition = metadata->second;
        std::set<Json> seen;
        for (auto& value : values) {
            auto normalized = normalize_value(definition.type, value);
            if (!normalized || !in_domain(definition, *normalized))
                return manifest_error("Accepted value is outside its task parameter domain", field, path);
            if (!seen.insert(*normalized).second)
                return manifest_error("Accepted task values must be distinct", field, path);
            value = std::move(*normalized);
        }
    }
    return {};
}

template <typename Value>
auto check_members(const Value& value, std::initializer_list<std::string_view> allowed,
                   std::string_view field, const Path& path) -> std::expected<void, core::Error> {
    if (!value.is_object()) return manifest_error("Expected a mapping", field, path);
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!std::ranges::contains(allowed, it.key()))
            return manifest_error("Unknown manifest field: " + it.key(), field, path);
    }
    return {};
}

auto parse_definition(const OrderedJson& value, std::string_view field, const Path& path)
    -> std::expected<ParameterDefinition, core::Error> {
    auto members = check_members(value, {"type", "default", "min", "max", "values"}, field, path);
    if (!members) return std::unexpected{std::move(members.error())};
    const auto type_field = value.find("type");
    if (type_field == value.end() || !type_field->is_string())
        return manifest_error("Parameter requires a type", field, path);
    const auto type = parameter_type(type_field->get_ref<const std::string&>());
    if (!type) return manifest_error("Unknown parameter type", field, path);

    ParameterDefinition result{.type = *type, .default_value = {}, .minimum = {}, .maximum = {},
                               .values = {}};
    const auto values = value.find("values");
    if (*type == ParameterType::enumeration) {
        if (values == value.end() || !values->is_array() || values->empty())
            return manifest_error("Enum requires a non-empty values sequence", field, path);
        std::set<std::string_view> seen;
        for (const auto& item : *values) {
            if (!item.is_string()) return manifest_error("Enum values must be strings", field, path);
            const auto& text = item.get_ref<const std::string&>();
            if (text.empty() || text.contains('\0') || !seen.insert(text).second)
                return manifest_error("Enum values must be distinct non-empty strings without NUL", field, path);
            result.values.push_back(text);
        }
    } else if (values != value.end()) {
        return manifest_error("Only enum parameters support values", field, path);
    }
    for (const auto& [name, destination] : {std::pair{"min", &result.minimum},
                                            std::pair{"max", &result.maximum}}) {
        const auto bound = value.find(name);
        if (bound == value.end()) continue;
        if (*type == ParameterType::enumeration || *type == ParameterType::boolean)
            return manifest_error("Only numeric parameters support bounds", field, path);
        *destination = normalize_value(*type, *bound);
        if (!*destination) return manifest_error("Parameter bound has the wrong type or range", field, path);
    }
    if (result.minimum && result.maximum && *result.minimum > *result.maximum)
        return manifest_error("Parameter minimum exceeds maximum", field, path);
    if (const auto initial = value.find("default"); initial != value.end()) {
        result.default_value = normalize_value(*type, *initial);
        if (!result.default_value || !in_domain(result, *result.default_value))
            return manifest_error("Parameter default is outside its type or domain", field, path);
    }
    return result;
}

auto parse_ranks(const OrderedJson& document, const Path& path)
    -> std::expected<RankConstraints, core::Error> {
    const auto ranks = document.find("ranks");
    if (ranks == document.end()) return RankConstraints{};
    auto members = check_members(*ranks, {"min", "max", "distinct_cores"}, "ranks", path);
    if (!members) return std::unexpected{std::move(members.error())};
    const auto minimum = ranks->find("min");
    const auto maximum = ranks->find("max");
    if (minimum == ranks->end() || maximum == ranks->end() ||
        !minimum->is_number_unsigned() || !maximum->is_number_unsigned())
        return manifest_error("Rank bounds must be unsigned integers", "ranks", path);
    const auto low = minimum->get<std::uint64_t>();
    const auto high = maximum->get<std::uint64_t>();
    if (low == 0 || low > high || high > std::numeric_limits<std::uint32_t>::max())
        return manifest_error("Invalid minimum or maximum worker rank count", "ranks", path);
    const auto distinct = ranks->find("distinct_cores");
    if (distinct != ranks->end() && !distinct->is_boolean())
        return manifest_error("Expected a distinct_cores boolean", "ranks.distinct_cores", path);
    return RankConstraints{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(high),
                           distinct != ranks->end() && distinct->get<bool>()};
}

auto parse_definitions(const OrderedJson& document, std::string_view field, const Path& path)
    -> std::expected<ParameterDefinitions, core::Error> {
    const auto definitions = document.find(field);
    if (definitions == document.end() || !definitions->is_object())
        return manifest_error("Expected a parameter definitions mapping", field, path);
    ParameterDefinitions result;
    for (auto it = definitions->begin(); it != definitions->end(); ++it) {
        const auto member = std::string{field} + "." + it.key();
        if (!valid_name(it.key())) return manifest_error("Invalid parameter name", member, path);
        auto definition = parse_definition(it.value(), member, path);
        if (!definition) return std::unexpected{std::move(definition.error())};
        result.emplace(it.key(), std::move(*definition));
    }
    return result;
}

auto parse_accepted_values(const OrderedJson& document, const Path& path)
    -> std::expected<AcceptedValues, core::Error> {
    AcceptedValues result;
    const auto accepts = document.find("accepts");
    if (accepts == document.end()) return result;
    if (!accepts->is_object()) return manifest_error("Expected an accepted values mapping", "accepts", path);
    for (auto parameter = accepts->begin(); parameter != accepts->end(); ++parameter) {
        const auto field = "accepts." + parameter.key();
        if (!valid_name(parameter.key()) || !parameter->is_array() || parameter->empty())
            return manifest_error("Expected a parameter name and non-empty sequence of accepted values", field, path);
        std::vector<Json> values;
        values.reserve(parameter->size());
        for (const auto& value : *parameter) {
            if (!(value.is_number() || value.is_string() || value.is_boolean()))
                return manifest_error("Accepted values must be scalars", field, path);
            values.emplace_back(value);
        }
        result.emplace(parameter.key(), std::move(values));
    }
    return result;
}

auto parse_ports(const OrderedJson& document, const Path& path)
    -> std::expected<TaskPorts, core::Error> {
    const auto ports = document.find("ports");
    TaskPorts result;
    if (ports == document.end()) return result;
    auto members = check_members(*ports, {"inputs", "outputs"}, "ports", path);
    if (!members) return std::unexpected{std::move(members.error())};
    for (const auto& [direction, values] : {std::pair{"inputs", &result.inputs},
                                           std::pair{"outputs", &result.outputs}}) {
        const auto mapping = ports->find(direction);
        const auto field = "ports." + std::string{direction};
        if (mapping == ports->end() || !mapping->is_object())
            return manifest_error("Expected a port definitions mapping", field, path);
        for (auto it = mapping->begin(); it != mapping->end(); ++it) {
            const auto name = std::string_view{it.key()};
            if (name.size() >= EPYCBENCH_PACKAGE_REGION_NAME_CAPACITY || !valid_name(name) ||
                !it->is_string() || !valid_id(it->get_ref<const std::string&>()))
                return manifest_error("Expected a port name and format token", field + "." + it.key(), path);
            values->emplace(it.key(), it->get<std::string>());
        }
    }
    if (result.inputs.size() + result.outputs.size() > EPYCBENCH_PACKAGE_MAX_REGIONS)
        return manifest_error("Too many declared ports", "ports", path);
    for (const auto& [name, format] : result.outputs) {
        if (result.inputs.contains(name))
            return manifest_error("Input and output port names must be distinct", "ports.outputs." + name, path);
    }
    return result;
}

auto parse_control_inputs(const OrderedJson& document, const Path& path, bool collective)
    -> std::expected<std::vector<std::string>, core::Error> {
    const auto declared = document.find("control_inputs");
    std::vector<std::string> result;
    if (declared == document.end()) return result;
    if (!collective || !declared->is_array() || declared->size() > EPYCBENCH_PACKAGE_MAX_REGIONS)
        return manifest_error("Expected a bounded sequence of collective control input names", "control_inputs", path);
    for (const auto& item : *declared) {
        if (!item.is_string()) return manifest_error("Expected a control input name", "control_inputs", path);
        const auto& name = item.get_ref<const std::string&>();
        if (name.size() >= EPYCBENCH_PACKAGE_REGION_NAME_CAPACITY || !valid_name(name) ||
            std::ranges::contains(result, name))
            return manifest_error("Control input names must be valid and unique", "control_inputs", path);
        result.push_back(name);
    }
    return result;
}

auto parse_partition_extent_parameters(const OrderedJson& document, const ParameterDefinitions& parameters,
                                       const Path& path) -> std::expected<std::vector<std::string>, core::Error> {
    constexpr std::string_view field = "partition_extent_parameters";
    const auto declared = document.find(field);
    std::vector<std::string> result;
    if (declared == document.end()) return result;
    if (!declared->is_array() || declared->empty() || declared->size() > parameters.size())
        return manifest_error("Expected a nonempty sequence of declared task parameter names", field, path);
    for (const auto& item : *declared) {
        if (!item.is_string())
            return manifest_error("Expected a task parameter name", field, path);
        const auto& name = item.get_ref<const std::string&>();
        if (!parameters.contains(name) || std::ranges::contains(result, name))
            return manifest_error("Partition extent parameter names must be declared and unique", field, path);
        result.push_back(name);
    }
    return result;
}

auto parse_document(std::string_view contents, const Path& path)
    -> std::expected<scenario::detail::ParsedDocument, core::Error> {
    if (contents.size() > manifest_limit)
        return manifest_error("Package manifest exceeds the 1 MiB size limit", {}, path);
    auto parsed = scenario::detail::parse_restricted_yaml(contents);
    if (!parsed) {
        auto failure = std::move(parsed.error());
        failure.category = core::ErrorCategory::kernel;
        failure.context["parser_code"] = failure.code.value();
        failure.context["path"] = path.string();
        failure.code = core::StableErrorCode{"kernel.package.invalid_yaml"};
        failure.message = "Package manifest violates the restricted YAML grammar";
        failure.operation = "kernel_package_manifest";
        return std::unexpected{std::move(failure)};
    }
    return parsed;
}

auto parse_identity(const OrderedJson& document, std::string_view schema, const Path& path)
    -> std::expected<std::string, core::Error> {
    const auto schema_value = document.find("schema");
    if (schema_value == document.end() || !core::detail::json_string_equal(*schema_value, schema))
        return manifest_error("Expected manifest schema " + std::string{schema}, "schema", path);
    const auto id = document.find("id");
    if (id == document.end() || !id->is_string() || !valid_id(id->get_ref<const std::string&>()))
        return manifest_error("Expected a valid package ID", "id", path);
    return id->get<std::string>();
}

auto sibling_path(const OrderedJson& document, std::string_view field, std::string_view suffix,
                  const Path& path) -> std::expected<Path, core::Error> {
    const auto entry = document.find(field);
    if (entry != document.end() && entry->is_string()) {
        const auto& filename = entry->get_ref<const std::string&>();
        if (filename.size() > suffix.size() && filename.ends_with(suffix) &&
            !filename.contains('/') && !filename.contains('\\') && !filename.contains('\0'))
            return path.parent_path() / filename;
    }
    return manifest_error("Expected a sibling filename ending in " + std::string{suffix}, field, path);
}

struct TaskDeclaration {
    std::string id;
    ParameterDefinitions parameters;
    TaskPorts ports;
    RankConstraints ranks;
    bool collective;
    bool collective_finalizer;
    bool shared_cpu_local;
    bool per_worker_input;
    MappingResource mapping;
    std::vector<std::string> control_inputs;
    std::vector<std::string> partition_extent_parameters;
    std::string work_unit, work_metric;
    Path oracle;
};

auto parse_work(const OrderedJson& document, const Path& path)
    -> std::expected<std::pair<std::string, std::string>, core::Error> {
    const auto work = document.find("work");
    if (work == document.end()) return std::pair{"work_unit", "units_per_second"};
    auto members = check_members(*work, {"unit", "metric"}, "work", path);
    if (!members) return std::unexpected{std::move(members.error())};
    const auto unit = work->find("unit");
    const auto metric = work->find("metric");
    if (unit == work->end() || !unit->is_string() || !valid_id(unit->get_ref<const std::string&>()))
        return manifest_error("Expected a logical work unit token", "work.unit", path);
    if (metric == work->end() || (!core::detail::json_string_equal(*metric, "units_per_second") &&
                                 !core::detail::json_string_equal(*metric, "seconds_per_unit")))
        return manifest_error("Expected units_per_second or seconds_per_unit", "work.metric", path);
    return std::pair{unit->get<std::string>(), metric->get<std::string>()};
}

auto parse_task(std::string_view contents, const Path& path)
    -> std::expected<TaskDeclaration, core::Error> {
    auto parsed = parse_document(contents, path);
    if (!parsed) return std::unexpected{std::move(parsed.error())};
    const core::detail::JsonCleanup cleanup{parsed->value};
    auto members = check_members(parsed->value,
        {"schema", "id", "oracle", "parameters", "ports", "ranks", "collective", "collective_finalizer",
         "shared_placement", "work", "input_scaling", "control_inputs", "mapping",
         "partition_extent_parameters"}, {}, path);
    if (!members) return std::unexpected{std::move(members.error())};
    auto id = parse_identity(parsed->value, "epycbench.task/v1", path);
    if (!id) return std::unexpected{std::move(id.error())};
    auto parameters = parse_definitions(parsed->value, "parameters", path);
    if (!parameters) return std::unexpected{std::move(parameters.error())};
    auto extent_parameters = parse_partition_extent_parameters(parsed->value, *parameters, path);
    if (!extent_parameters) return std::unexpected{std::move(extent_parameters.error())};
    auto ports = parse_ports(parsed->value, path);
    if (!ports) return std::unexpected{std::move(ports.error())};
    auto ranks = parse_ranks(parsed->value, path);
    if (!ranks) return std::unexpected{std::move(ranks.error())};
    const auto collective = parsed->value.find("collective");
    if (collective != parsed->value.end() && !collective->is_boolean())
        return manifest_error("Expected a collective boolean", "collective", path);
    const bool collective_task = collective != parsed->value.end() && collective->get<bool>();
    auto control_inputs = parse_control_inputs(parsed->value, path, collective_task);
    if (!control_inputs) return std::unexpected{std::move(control_inputs.error())};
    const auto finalizer = parsed->value.find("collective_finalizer");
    if (finalizer != parsed->value.end() && !finalizer->is_boolean())
        return manifest_error("Expected a collective_finalizer boolean", "collective_finalizer", path);
    const bool collective_finalizer = finalizer != parsed->value.end() && finalizer->get<bool>();
    if (collective_finalizer && !collective_task)
        return manifest_error("A collective finalizer requires a collective task", "collective_finalizer", path);
    const auto placement = parsed->value.find("shared_placement");
    const bool cpu_local = placement != parsed->value.end() && core::detail::json_string_equal(*placement, "owner_cpu_local");
    if (placement != parsed->value.end() &&
        (!collective_task || (!cpu_local && !core::detail::json_string_equal(*placement, "owner_memory"))))
        return manifest_error("Expected a collective shared placement of owner_memory or owner_cpu_local", "shared_placement", path);
    const auto scaling = parsed->value.find("input_scaling");
    const bool per_worker = scaling != parsed->value.end() && core::detail::json_string_equal(*scaling, "per_worker");
    if (scaling != parsed->value.end() && !per_worker && !core::detail::json_string_equal(*scaling, "fixed"))
        return manifest_error("Expected fixed or per_worker task input scaling", "input_scaling", path);
    MappingResource mapping{};
    if (const auto resource = parsed->value.find("mapping"); resource != parsed->value.end()) {
        if (core::detail::json_string_equal(*resource, "first_touch"))
            mapping = MappingResource::first_touch;
        else if (core::detail::json_string_equal(*resource, "allocation"))
            mapping = MappingResource::allocation;
        else
            return manifest_error("Expected first_touch or allocation mapping resource", "mapping", path);
        if (collective_task || per_worker || ranks->minimum != 1U || ranks->maximum != 1U ||
            !ports->inputs.empty() || !ports->outputs.empty())
            return manifest_error("Mapping tasks require one rank and no prepared ports or collective", "mapping", path);
        const auto has_type = [&](std::string_view name, ParameterType type) {
            const auto parameter = parameters->find(name);
            return parameter != parameters->end() && parameter->second.type == type;
        };
        if (mapping == MappingResource::first_touch &&
            (parameters->size() != 2U || !has_type("pages", ParameterType::uint64) ||
             !has_type("page_mode", ParameterType::enumeration)))
            return manifest_error("First-touch mapping requires pages and page_mode parameters", "parameters", path);
        if (mapping == MappingResource::allocation &&
            (parameters->size() != 1U || !has_type("requested_bytes", ParameterType::uint64)))
            return manifest_error("Allocation mapping requires a requested_bytes parameter", "parameters", path);
    }
    auto work = parse_work(parsed->value, path);
    if (!work) return std::unexpected{std::move(work.error())};
    auto oracle = sibling_path(parsed->value, "oracle", ".so", path);
    if (!oracle) return std::unexpected{std::move(oracle.error())};
    return TaskDeclaration{std::move(*id), std::move(*parameters), std::move(*ports), *ranks,
        collective_task, collective_finalizer, cpu_local, per_worker, mapping, std::move(*control_inputs),
        std::move(*extent_parameters),
        std::move(work->first), std::move(work->second), std::move(*oracle)};
}

auto load_task(const Path& path) -> std::expected<TaskManifest, core::Error> {
    auto contents = read_manifest(path);
    if (!contents) return std::unexpected{std::move(contents.error())};
    auto task = parse_task(*contents, path);
    if (!task) return std::unexpected{std::move(task.error())};
    auto oracle_digest = digest_file(task->oracle);
    if (!oracle_digest) return std::unexpected{std::move(oracle_digest.error())};
    const auto manifest_digest = core::sha256(*contents);
    return TaskManifest{.id = std::move(task->id), .parameters = std::move(task->parameters),
        .ports = std::move(task->ports), .ranks = task->ranks, .collective = task->collective,
        .collective_finalizer = task->collective_finalizer, .shared_cpu_local = task->shared_cpu_local,
        .per_worker_input = task->per_worker_input, .mapping = task->mapping,
        .control_inputs = std::move(task->control_inputs),
        .partition_extent_parameters = std::move(task->partition_extent_parameters),
        .work_unit = std::move(task->work_unit),
        .work_metric = std::move(task->work_metric), .path = path, .oracle_path = std::move(task->oracle),
        .contents = std::move(*contents), .sha256 = manifest_digest, .oracle_sha256 = *oracle_digest};
}

struct Tasks {
    std::map<Path, std::shared_ptr<const TaskManifest>> by_path;
    std::map<std::string, std::shared_ptr<const TaskManifest>, std::less<>> by_id;

    auto at(const Path& path) -> std::expected<std::shared_ptr<const TaskManifest>, core::Error> {
        if (const auto existing = by_path.find(path); existing != by_path.end()) return existing->second;
        auto task = load_task(path);
        if (!task) return std::unexpected{std::move(task.error())};
        const auto existing = by_id.find(task->id);
        if (existing != by_id.end() && (existing->second->contents != task->contents ||
                                       existing->second->oracle_sha256 != task->oracle_sha256))
            return error("conflicting_task", "Task ID has conflicting definitions: " + task->id, "id", path);
        auto shared = std::make_shared<const TaskManifest>(std::move(*task));
        by_id.try_emplace(shared->id, shared);
        by_path.emplace(path, shared);
        return shared;
    }
};

struct ImplementationDeclaration {
    std::string id;
    ParameterDefinitions tuning;
    AcceptedValues accepts;
    std::optional<core::Sha256Digest> declared_module_digest;
    Path task, module;
};

auto parse_declared_module_digest(const OrderedJson& document, const Path& path)
    -> std::expected<std::optional<core::Sha256Digest>, core::Error> {
    const auto build = document.find("build");
    if (build == document.end()) return std::nullopt;
    if (!build->is_object()) return manifest_error("Expected a build provenance mapping", "build", path);
    std::optional<core::Sha256Digest> module;
    for (const auto field : {"module_sha256", "receipt_sha256"}) {
        const auto entry = build->find(field);
        if (entry == build->end() || !entry->is_string())
            return manifest_error("Expected a build SHA-256 digest", std::string{"build."} + field, path);
        const auto& value = entry->get_ref<const std::string&>();
        const auto parsed = core::Sha256Digest::parse(value);
        if (!parsed || parsed->to_hex() != value)
            return manifest_error("Expected a lowercase build SHA-256 digest", std::string{"build."} + field, path);
        if (std::string_view{field} == "module_sha256") module = *parsed;
    }
    return module;
}

auto parse_implementation(std::string_view contents, const Path& path)
    -> std::expected<ImplementationDeclaration, core::Error> {
    auto parsed = parse_document(contents, path);
    if (!parsed) return std::unexpected{std::move(parsed.error())};
    const core::detail::JsonCleanup cleanup{parsed->value};
    auto members = check_members(parsed->value, {"schema", "id", "task", "module", "tuning", "accepts", "build"}, {}, path);
    if (!members) return std::unexpected{std::move(members.error())};
    auto id = parse_identity(parsed->value, "epycbench.kernel/v1", path);
    if (!id) return std::unexpected{std::move(id.error())};
    auto task_path = sibling_path(parsed->value, "task", task_suffix, path);
    if (!task_path) return std::unexpected{std::move(task_path.error())};
    auto tuning = parse_definitions(parsed->value, "tuning", path);
    if (!tuning) return std::unexpected{std::move(tuning.error())};
    auto accepts = parse_accepted_values(parsed->value, path);
    if (!accepts) return std::unexpected{std::move(accepts.error())};
    auto declared_module_digest = parse_declared_module_digest(parsed->value, path);
    if (!declared_module_digest) return std::unexpected{std::move(declared_module_digest.error())};
    auto module = sibling_path(parsed->value, "module", ".so", path);
    if (!module) return std::unexpected{std::move(module.error())};
    return ImplementationDeclaration{std::move(*id), std::move(*tuning), std::move(*accepts),
                                     *declared_module_digest, std::move(*task_path), std::move(*module)};
}

auto load_implementation(const Path& path, Tasks& tasks)
    -> std::expected<ImplementationManifest, core::Error> {
    auto contents = read_manifest(path);
    if (!contents) return std::unexpected{std::move(contents.error())};
    auto implementation = parse_implementation(*contents, path);
    if (!implementation) return std::unexpected{std::move(implementation.error())};
    auto task = tasks.at(implementation->task);
    if (!task) return std::unexpected{std::move(task.error())};
    if ((*task)->mapping != MappingResource::none && !implementation->tuning.empty())
        return manifest_error("Mapping implementations have fixed callbacks and no tuning fields", "tuning", path);
    auto constraints = normalize_accepted_values(implementation->accepts, (*task)->parameters, path);
    if (!constraints) return std::unexpected{std::move(constraints.error())};
    auto module_digest = digest_file(implementation->module);
    if (!module_digest) return std::unexpected{std::move(module_digest.error())};
    if (implementation->declared_module_digest && *implementation->declared_module_digest != *module_digest)
        return manifest_error("Build provenance names a different implementation binary", "build.module_sha256", path);
    const auto manifest_digest = core::sha256(*contents);
    return ImplementationManifest{std::move(implementation->id), std::move(*task),
        std::move(implementation->tuning), std::move(implementation->accepts),
        path, std::move(implementation->module),
        std::move(*contents), manifest_digest, *module_digest};
}

} // namespace

auto Catalog::discover(const Path& directory) -> std::expected<Catalog, core::Error> {
    Catalog result;
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(directory, ec);
    if (ec) return file_error("Cannot resolve kernel package directory", directory, ec);
    std::filesystem::directory_iterator entry{absolute, ec};
    if (ec == std::errc::no_such_file_or_directory) return result;
    if (ec) return file_error("Cannot open kernel package directory", absolute, ec);
    std::vector<Path> paths;
    for (; !ec && entry != std::filesystem::directory_iterator{}; entry.increment(ec)) {
        if (entry->path().filename().string().ends_with(implementation_suffix))
            paths.push_back(entry->path());
    }
    if (ec) return file_error("Cannot read kernel package directory", absolute, ec);
    std::ranges::sort(paths);
    return load_manifests(paths);
}

auto Catalog::load_manifests(std::span<const Path> paths) -> std::expected<Catalog,core::Error> {
    Catalog result;
    Tasks tasks;
    std::set<std::pair<std::string, std::string>> ids;
    result.implementations_.reserve(paths.size());
    for (const auto& path : paths) {
        auto implementation = load_implementation(path, tasks);
        if (!implementation) return std::unexpected{std::move(implementation.error())};
        if (!ids.emplace(implementation->task->id, implementation->id).second)
            return error("duplicate_implementation", "Duplicate implementation for task " +
                         implementation->task->id + ": " + implementation->id, "id", path);
        result.implementations_.push_back(std::move(*implementation));
    }
    return result;
}

auto Catalog::from_bindings(const Json& bindings) -> std::expected<Catalog, core::Error> {
    if (!bindings.is_object() || bindings.empty())
        return error("saved_bindings_invalid", "Expected saved package bindings by role", "bindings");
    constexpr std::array<std::string_view, 6> fields{
        "task_manifest", "task_manifest_sha256", "oracle_sha256",
        "implementation_manifest", "implementation_manifest_sha256", "module_sha256"};
    Catalog result;
    std::map<std::string, std::shared_ptr<const TaskManifest>, std::less<>> tasks;
    for (auto role = bindings.begin(); role != bindings.end(); ++role) {
        const auto& binding = role.value();
        if (!binding.is_object() || binding.size() != fields.size() ||
            !std::ranges::all_of(fields, [&](std::string_view name) {
                const auto member = binding.find(name);
                return member != binding.end() && member->is_string();
            }))
            return error("saved_bindings_invalid", "Invalid saved package binding", role.key());
        const auto text = [&](std::string_view name) -> const std::string& {
            return binding.at(name).get_ref<const std::string&>();
        };
        const auto& task_yaml = text("task_manifest");
        const auto& implementation_yaml = text("implementation_manifest");
        if (task_yaml.size() > manifest_limit || implementation_yaml.size() > manifest_limit)
            return error("saved_bindings_invalid", "Saved package manifest exceeds the size limit", role.key());
        const auto task_digest = core::sha256(task_yaml);
        const auto implementation_digest = core::sha256(implementation_yaml);
        const auto oracle_digest = core::Sha256Digest::parse(text("oracle_sha256"));
        const auto module_digest = core::Sha256Digest::parse(text("module_sha256"));
        if (task_digest.to_hex() != text("task_manifest_sha256") ||
            implementation_digest.to_hex() != text("implementation_manifest_sha256") ||
            !oracle_digest || !module_digest || oracle_digest->to_hex() != text("oracle_sha256") ||
            module_digest->to_hex() != text("module_sha256"))
            return error("saved_bindings_invalid", "Saved package digests are invalid or inconsistent", role.key());

        auto task = parse_task(task_yaml, {});
        if (!task) return std::unexpected{std::move(task.error())};
        auto implementation = parse_implementation(implementation_yaml, {});
        if (!implementation) return std::unexpected{std::move(implementation.error())};
        if (task->mapping != MappingResource::none && !implementation->tuning.empty())
            return manifest_error("Mapping implementations have fixed callbacks and no tuning fields", "tuning", {});
        if (implementation->declared_module_digest && *implementation->declared_module_digest != *module_digest)
            return error("saved_bindings_invalid", "Saved build provenance disagrees with its implementation binary", role.key());
        auto constraints = normalize_accepted_values(implementation->accepts, task->parameters, {});
        if (!constraints) return std::unexpected{std::move(constraints.error())};
        auto& shared = tasks[task->id];
        if (shared) {
            if (shared->contents != task_yaml || shared->oracle_sha256 != *oracle_digest)
                return error("conflicting_task", "Saved task ID has conflicting definitions: " + task->id,
                             role.key());
        } else {
            shared = std::make_shared<const TaskManifest>(TaskManifest{.id = std::move(task->id),
                .parameters = std::move(task->parameters), .ports = std::move(task->ports), .ranks = task->ranks,
                .collective = task->collective, .collective_finalizer = task->collective_finalizer,
                .shared_cpu_local = task->shared_cpu_local, .per_worker_input = task->per_worker_input,
                .mapping = task->mapping,
                .control_inputs = std::move(task->control_inputs),
                .partition_extent_parameters = std::move(task->partition_extent_parameters),
                .work_unit = std::move(task->work_unit), .work_metric = std::move(task->work_metric),
                .path = {}, .oracle_path = {}, .contents = task_yaml, .sha256 = task_digest,
                .oracle_sha256 = *oracle_digest});
        }
        if (const auto* existing = result.find(shared->id, implementation->id)) {
            if (existing->contents != implementation_yaml || existing->module_sha256 != *module_digest)
                return error("duplicate_implementation", "Saved implementation ID has conflicting definitions: " +
                             implementation->id, role.key());
            continue;
        }
        result.implementations_.push_back(ImplementationManifest{
            std::move(implementation->id), shared, std::move(implementation->tuning),
            std::move(implementation->accepts), {}, {},
            implementation_yaml, implementation_digest, *module_digest});
    }
    return result;
}

auto Catalog::find(std::string_view task_id, std::string_view implementation_id) const noexcept
    -> const ImplementationManifest* {
    const auto found = std::ranges::find_if(implementations_, [&](const auto& implementation) {
        return implementation.task->id == task_id && implementation.id == implementation_id;
    });
    return found == implementations_.end() ? nullptr : &*found;
}

auto package_identity(const ImplementationManifest& impl) -> Json {
    return {{"task",impl.task->id},{"implementation",impl.id},{"task_manifest_sha256",impl.task->sha256.to_hex()},
        {"oracle_sha256",impl.task->oracle_sha256.to_hex()},{"implementation_manifest_sha256",impl.sha256.to_hex()},
        {"module_sha256",impl.module_sha256.to_hex()}};
}

auto resolve_parameters(const ParameterDefinitions& definitions, const Json& supplied,
                        std::string_view field) -> std::expected<Json, core::Error> {
    if (!supplied.is_object()) return error("invalid_values", "Expected a parameter mapping", field);
    auto result = Json::object();
    for (auto it = supplied.begin(); it != supplied.end(); ++it) {
        const auto member = std::string{field} + "." + it.key();
        const auto definition = definitions.find(it.key());
        if (definition == definitions.end()) return error("invalid_values", "Unknown parameter", member);
        auto value = normalize_value(definition->second.type, it.value());
        if (!value || !in_domain(definition->second, *value))
            return error("invalid_values", "Parameter value is outside its type or domain", member);
        result.emplace(it.key(), std::move(*value));
    }
    for (const auto& [name, definition] : definitions) {
        if (result.contains(name)) continue;
        if (!definition.default_value)
            return error("invalid_values", "Required parameter is missing", std::string{field} + "." + name);
        result.emplace(name, *definition.default_value);
    }
    return result;
}

auto validate_implementation_inputs(const AcceptedValues& accepts, const Json& resolved,
                                    std::string_view field, std::string* rejected_parameter)
    -> std::expected<void, core::Error> {
    for (const auto& [name, allowed] : accepts) {
        const auto selected = resolved.find(name);
        if (selected == resolved.end() || !std::ranges::contains(allowed, *selected)) {
            if (rejected_parameter) *rejected_parameter = name;
            return error("task_input_unsupported", "Implementation does not accept task parameter value: " + name, field);
        }
    }
    return {};
}

auto validate_saved_values(std::string_view task_yaml, std::string_view implementation_yaml,
    std::string_view task_id, std::string_view implementation_id, const Json& effective_parameters,
    const Json& effective_tuning) -> std::expected<void, core::Error> {
    auto task = parse_task(task_yaml, {});
    if (!task) return std::unexpected{std::move(task.error())};
    auto implementation = parse_implementation(implementation_yaml, {});
    if (!implementation) return std::unexpected{std::move(implementation.error())};
    if (task->id != task_id || implementation->id != implementation_id)
        return error("saved_identity_mismatch", "Saved package IDs disagree with the manifest IDs", "id");
    auto parameters = resolve_parameters(task->parameters, effective_parameters, "parameters");
    if (!parameters) return std::unexpected{std::move(parameters.error())};
    auto tuning = resolve_parameters(implementation->tuning, effective_tuning, "tuning");
    if (!tuning) return std::unexpected{std::move(tuning.error())};
    if (parameters->size() != effective_parameters.size() || tuning->size() != effective_tuning.size())
        return error("invalid_values", "Saved package settings omit resolved values", {});
    auto constraints = normalize_accepted_values(implementation->accepts, task->parameters, {});
    if (!constraints) return std::unexpected{std::move(constraints.error())};
    return validate_implementation_inputs(implementation->accepts, *parameters, "accepts");
}

} // namespace epycbench::kernel::packages
