#include "scenario/scenario_v1_structure.hpp"

#include "epycbench/scenario/identifier.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::scenario::detail {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::size_t substantive_diagnostic_limit = 63;

auto escape_pointer_token(std::string_view token) -> std::string {
    std::string result;
    result.reserve(token.size());
    for (const char value : token) {
        if (value == '~')
            result += "~0";
        else if (value == '/')
            result += "~1";
        else
            result += value;
    }
    return result;
}

auto child_pointer(std::string_view parent, std::string_view child) -> std::string {
    return std::string{parent} + "/" + escape_pointer_token(child);
}

auto index_pointer(std::string_view parent, std::size_t index) -> std::string {
    return std::string{parent} + "/" + std::to_string(index);
}

auto type_name(const Json& value) -> std::string {
    if (value.is_object())
        return "object";
    if (value.is_array())
        return "array";
    if (value.is_string())
        return "string";
    if (value.is_boolean())
        return "boolean";
    if (value.is_number_unsigned())
        return "unsigned_integer";
    if (value.is_number_integer())
        return "signed_integer";
    if (value.is_number_float())
        return "floating_point";
    if (value.is_null())
        return "null";
    return "discarded";
}

class Validator {
  public:
    explicit Validator(const AuthoredScenario& authored) : authored_(authored) {}

    auto run() -> std::vector<ScenarioDiagnostic> {
        const auto& root = authored_.value();
        if (!root.is_object()) {
            add("scenario.structure.root_not_object", "", false,
                {{"expected", "object"}, {"actual", type_name(root)}});
            return std::move(diagnostics_);
        }

        static constexpr std::array required{"schema", "metadata", "workloads", "experiments"};
        static constexpr std::array known{"schema",     "metadata",  "requirements", "defaults",
                                          "resources",  "workloads", "experiments",  "measurement",
                                          "monitoring", "output",    "random"};

        // Version diagnostics precede all other root diagnostics.
        if (!root.contains("schema")) {
            add("scenario.structure.missing_version", "", false, {{"field", "schema"}});
        } else {
            const auto& schema = root.at("schema");
            if (!schema.is_string()) {
                invalid_type("/schema", schema, "string");
            } else if (schema.get_ref<const std::string&>() != "epycbench.scenario/v1") {
                add("scenario.structure.unsupported_version", "/schema", false,
                    {{"expected", "epycbench.scenario/v1"}});
            }
        }

        object_members(root, "", required, known, "schema");
        if (stopped_)
            return std::move(diagnostics_);

        if (const auto* value = member(root, "metadata"))
            validate_metadata(*value, "/metadata");
        if (const auto* value = member(root, "requirements"))
            validate_requirements(*value, "/requirements");
        if (const auto* value = member(root, "defaults"))
            validate_defaults(*value, "/defaults");
        if (const auto* value = member(root, "resources"))
            validate_resources(*value, "/resources");
        if (const auto* value = member(root, "workloads"))
            validate_workloads(*value, "/workloads");
        if (const auto* value = member(root, "experiments"))
            validate_experiments(*value, "/experiments");
        if (const auto* value = member(root, "measurement"))
            validate_measurement(*value, "/measurement");
        if (const auto* value = member(root, "monitoring"))
            validate_monitoring(*value, "/monitoring");
        if (const auto* value = member(root, "output"))
            validate_output(*value, "/output");
        if (const auto* value = member(root, "random"))
            validate_random(*value, "/random");
        return std::move(diagnostics_);
    }

  private:
    const AuthoredScenario& authored_;
    std::vector<ScenarioDiagnostic> diagnostics_;
    bool stopped_{};

    auto location(std::string_view pointer, bool key) const -> std::optional<SourceLocation> {
        const auto found = authored_.locations().find(std::string{pointer});
        if (found == authored_.locations().end())
            return std::nullopt;
        if (key && found->second.mapping_key)
            return found->second.mapping_key;
        return found->second.value;
    }

    void add(std::string code, std::string pointer, bool key,
             std::map<std::string, std::string> context = {}) {
        if (stopped_)
            return;
        if (diagnostics_.size() == substantive_diagnostic_limit) {
            diagnostics_.push_back(ScenarioDiagnostic{
                .code = core::StableErrorCode{"scenario.structure.diagnostic_limit"},
                .json_pointer = std::move(pointer),
                .location = location(pointer, false),
                .safe_context = {{"limit", "64"}},
            });
            stopped_ = true;
            return;
        }
        diagnostics_.push_back(ScenarioDiagnostic{.code = core::StableErrorCode{std::move(code)},
                                                  .json_pointer = pointer,
                                                  .location = location(pointer, key),
                                                  .safe_context = std::move(context)});
    }

    static auto member(const Json& object, std::string_view name) -> const Json* {
        const auto found = object.find(std::string{name});
        return found == object.end() ? nullptr : &*found;
    }

    template <std::size_t Required, std::size_t Known>
    void object_members(const Json& object, std::string_view path,
                        const std::array<const char*, Required>& required,
                        const std::array<const char*, Known>& known,
                        std::string_view skip_missing = {}) {
        for (const auto* name : required) {
            if (name == skip_missing)
                continue;
            if (!object.contains(name)) {
                add("scenario.structure.missing_field", std::string{path}, false,
                    {{"field", name}});
            }
        }
        for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
            const auto recognized = std::ranges::any_of(
                known, [&](const char* candidate) { return iterator.key() == candidate; });
            if (!recognized) {
                add("scenario.structure.unknown_field", child_pointer(path, iterator.key()), true,
                    {});
            }
        }
    }

    void invalid_type(std::string path, const Json& value, std::string expected) {
        add("scenario.structure.invalid_type", std::move(path), false,
            {{"expected", std::move(expected)}, {"actual", type_name(value)}});
    }

    auto require_object(const Json& value, std::string_view path) -> bool {
        if (value.is_object())
            return true;
        invalid_type(std::string{path}, value, "object");
        return false;
    }

    void require_string(const Json& value, std::string path, bool nonempty = false) {
        if (!value.is_string()) {
            invalid_type(std::move(path), value, "string");
        } else if (nonempty && value.get_ref<const std::string&>().empty()) {
            add("scenario.structure.invalid_object_shape", std::move(path), false,
                {{"classification", "empty_token"}});
        }
    }

    void require_boolean(const Json& value, std::string path) {
        if (!value.is_boolean())
            invalid_type(std::move(path), value, "boolean");
    }

    void require_unsigned(const Json& value, std::string path) {
        if (!value.is_number_unsigned())
            invalid_type(std::move(path), value, "unsigned_integer");
    }

    void token(const Json& value, std::string path, std::span<const std::string_view> tokens) {
        if (!value.is_string()) {
            invalid_type(std::move(path), value, "string");
            return;
        }
        const auto& text = value.get_ref<const std::string&>();
        if (std::ranges::find(tokens, text) == tokens.end()) {
            add("scenario.structure.invalid_object_shape", std::move(path), false,
                {{"classification", "unsupported_token"}});
        }
    }

    void identifier(const Json& value, std::string path, bool key = false) {
        if (!value.is_string()) {
            invalid_type(std::move(path), value, "string");
            return;
        }
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() > 64) {
            add("scenario.structure.identifier_too_long", std::move(path), key, {{"limit", "64"}});
        } else if (!ScenarioIdentifier::parse(text)) {
            add("scenario.structure.invalid_identifier", std::move(path), key,
                {{"grammar", "[a-z][a-z0-9-]{0,63}"}});
        }
    }

    void string_array(const Json& value, std::string path, bool identifiers = false,
                      bool nonempty = false, bool unique = false, bool element_nonempty = true) {
        if (!value.is_array()) {
            invalid_type(std::move(path), value, "array");
            return;
        }
        if (nonempty && value.empty()) {
            add("scenario.structure.empty_collection", path, false,
                {{"limit_name", "minimum_items"}, {"limit", "1"}});
        }
        std::set<std::string> seen;
        for (std::size_t index = 0; index < value.size() && !stopped_; ++index) {
            auto item_path = index_pointer(path, index);
            if (identifiers)
                identifier(value[index], std::move(item_path));
            else
                require_string(value[index], std::move(item_path), element_nonempty);
            if (unique && value[index].is_string() &&
                !seen.insert(value[index].get_ref<const std::string&>()).second) {
                add("scenario.structure.invalid_object_shape", index_pointer(path, index), false,
                    {{"classification", "duplicate_list_entry"}});
            }
        }
    }

    void unsigned_array(const Json& value, std::string path, bool nonempty, bool unique) {
        if (!value.is_array()) {
            invalid_type(std::move(path), value, "array");
            return;
        }
        if (nonempty && value.empty()) {
            add("scenario.structure.empty_collection", path, false,
                {{"limit_name", "minimum_items"}, {"limit", "1"}});
        }
        std::set<std::uint64_t> seen;
        for (std::size_t index = 0; index < value.size() && !stopped_; ++index) {
            auto item_path = index_pointer(path, index);
            if (!value[index].is_number_unsigned()) {
                invalid_type(std::move(item_path), value[index], "unsigned_integer");
                continue;
            }
            if (unique && !seen.insert(value[index].get<std::uint64_t>()).second) {
                add("scenario.structure.invalid_object_shape", std::move(item_path), false,
                    {{"classification", "duplicate_list_entry"}});
            }
        }
    }

    void validate_metadata(const Json& value, std::string_view path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array required{"id"};
        static constexpr std::array known{"id", "name", "description", "tags", "user"};
        object_members(value, path, required, known);
        if (const auto* item = member(value, "id"))
            identifier(*item, child_pointer(path, "id"));
        if (const auto* item = member(value, "name"))
            require_string(*item, child_pointer(path, "name"));
        if (const auto* item = member(value, "description"))
            require_string(*item, child_pointer(path, "description"));
        if (const auto* item = member(value, "tags"))
            string_array(*item, child_pointer(path, "tags"), false, false, false, false);
        if (const auto* item = member(value, "user"))
            require_object(*item, child_pointer(path, "user"));
    }

    void validate_random(const Json& value, std::string_view path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> required{};
        static constexpr std::array known{"seed"};
        object_members(value, path, required, known);
        if (const auto* item = member(value, "seed"))
            require_unsigned(*item, child_pointer(path, "seed"));
    }

    void validate_requirements(const Json& value, std::string_view path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{
            "operating_system", "architecture", "cpu_vendor", "numa", "smt", "features"};
        object_members(value, path, none, known);
        static constexpr std::array linux_token{std::string_view{"linux"}};
        if (const auto* item = member(value, "operating_system"))
            token(*item, child_pointer(path, "operating_system"), linux_token);
        validate_any_of(member(value, "architecture"), child_pointer(path, "architecture"));
        validate_any_of(member(value, "cpu_vendor"), child_pointer(path, "cpu_vendor"));
        if (const auto* object = member(value, "numa")) {
            const auto object_path = child_pointer(path, "numa");
            if (require_object(*object, object_path)) {
                static constexpr std::array fields{"minimum_cpu_nodes", "minimum_memory_nodes"};
                object_members(*object, object_path, none, fields);
                for (const auto* field : fields)
                    if (const auto* item = member(*object, field))
                        require_unsigned(*item, child_pointer(object_path, field));
            }
        }
        if (const auto* object = member(value, "smt")) {
            const auto object_path = child_pointer(path, "smt");
            if (require_object(*object, object_path)) {
                static constexpr std::array fields{"permitted"};
                object_members(*object, object_path, none, fields);
                if (const auto* item = member(*object, "permitted"))
                    require_boolean(*item, child_pointer(object_path, "permitted"));
            }
        }
        if (const auto* object = member(value, "features")) {
            const auto object_path = child_pointer(path, "features");
            if (require_object(*object, object_path)) {
                static constexpr std::array fields{"required", "preferred"};
                object_members(*object, object_path, none, fields);
                for (const auto* field : fields)
                    if (const auto* item = member(*object, field))
                        string_array(*item, child_pointer(object_path, field));
            }
        }
    }

    void validate_any_of(const Json* value, const std::string& path) {
        if (!value || !require_object(*value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"any_of"};
        object_members(*value, path, none, known);
        if (const auto* item = member(*value, "any_of"))
            string_array(*item, child_pointer(path, "any_of"));
    }

    void validate_defaults(const Json& value, std::string_view path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"memory", "workers", "comparison", "observation"};
        object_members(value, path, none, known);
        if (const auto* item = member(value, "memory"))
            validate_memory_defaults(*item, child_pointer(path, "memory"));
        if (const auto* item = member(value, "workers"))
            validate_workers_defaults(*item, child_pointer(path, "workers"));
        if (const auto* item = member(value, "comparison"))
            validate_comparison_defaults(*item, child_pointer(path, "comparison"));
        if (const auto* item = member(value, "observation")) {
            const auto item_path = child_pointer(path, "observation");
            if (require_object(*item, item_path)) {
                static constexpr std::array fields{"target_duration"};
                object_members(*item, item_path, none, fields);
                if (const auto* field = member(*item, "target_duration"))
                    require_string(*field, child_pointer(item_path, "target_duration"), true);
            }
        }
    }

    void validate_memory_defaults(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array fields{"page_mode", "placement_threshold", "prefault",
                                           "verify_before", "verify_after"};
        object_members(value, path, none, fields);
        static constexpr std::array base{std::string_view{"base"}};
        if (const auto* item = member(value, "page_mode"))
            token(*item, child_pointer(path, "page_mode"), base);
        if (const auto* item = member(value, "placement_threshold"))
            require_string(*item, child_pointer(path, "placement_threshold"), true);
        for (const auto* field : {"prefault", "verify_before", "verify_after"})
            if (const auto* item = member(value, field))
                require_boolean(*item, child_pointer(path, field));
    }

    void validate_workers_defaults(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array fields{"smt", "ordering"};
        object_members(value, path, none, fields);
        static constexpr std::array smt{std::string_view{"one-per-core"}, std::string_view{"all"}};
        if (const auto* item = member(value, "smt"))
            token(*item, child_pointer(path, "smt"), smt);
        if (const auto* item = member(value, "ordering"))
            ordering_token(*item, child_pointer(path, "ordering"));
    }

    void validate_comparison_defaults(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array fields{"order", "minimum_blocks", "maximum_blocks"};
        object_members(value, path, none, fields);
        if (const auto* item = member(value, "order"))
            comparison_strategy(*item, child_pointer(path, "order"));
        for (const auto* field : {"minimum_blocks", "maximum_blocks"})
            if (const auto* item = member(value, field))
                require_unsigned(*item, child_pointer(path, field));
    }

    void ordering_token(const Json& value, std::string path) {
        static constexpr std::array tokens{
            std::string_view{"ascending-pu"},  std::string_view{"descending-pu"},
            std::string_view{"compact-cache"}, std::string_view{"spread-cache"},
            std::string_view{"compact-numa"},  std::string_view{"spread-numa"},
            std::string_view{"random"}};
        token(value, std::move(path), tokens);
    }

    void comparison_strategy(const Json& value, std::string path) {
        static constexpr std::array tokens{std::string_view{"randomized-abba"},
                                           std::string_view{"randomized-baab"},
                                           std::string_view{"randomized-balanced"}};
        token(value, std::move(path), tokens);
    }

    void validate_resources(const Json& value, std::string_view path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"worker_sets", "service_cpus"};
        object_members(value, path, none, known);
        for (const auto* category : known) {
            const auto* map = member(value, category);
            if (!map)
                continue;
            const auto map_path = child_pointer(path, category);
            if (!require_object(*map, map_path))
                continue;
            for (auto iterator = map->begin(); iterator != map->end() && !stopped_; ++iterator) {
                const auto resource_path = child_pointer(map_path, iterator.key());
                Json key_value = iterator.key();
                identifier(key_value, resource_path, true);
                validate_resource_selection(*iterator, resource_path);
            }
        }
    }

    void validate_resource_selection(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"pus", "select"};
        object_members(value, path, none, known);
        const bool pus = value.contains("pus");
        const bool select = value.contains("select");
        if (pus && select)
            add("scenario.structure.mutually_exclusive_fields", path, false,
                {{"fields", "pus,select"}});
        else if (!pus && !select)
            add("scenario.structure.missing_alternative", path, false, {{"fields", "pus,select"}});
        if (const auto* item = member(value, "pus"))
            unsigned_array(*item, child_pointer(path, "pus"), true, true);
        if (const auto* item = member(value, "select"))
            validate_selector(*item, child_pointer(path, "select"));
    }

    void validate_selector(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array required{"unit"};
        static constexpr std::array known{"unit", "where", "exclude", "smt", "count", "ordering"};
        object_members(value, path, required, known);
        static constexpr std::array units{std::string_view{"pu"}, std::string_view{"core"}};
        if (const auto* item = member(value, "unit"))
            token(*item, child_pointer(path, "unit"), units);
        if (const auto* item = member(value, "where"))
            validate_where(*item, child_pointer(path, "where"));
        if (const auto* item = member(value, "exclude"))
            validate_exclude(*item, child_pointer(path, "exclude"));
        if (const auto* item = member(value, "smt"))
            validate_smt(*item, child_pointer(path, "smt"));
        if (const auto* item = member(value, "count")) {
            const auto item_path = child_pointer(path, "count");
            if (item->is_number_unsigned()) {
                if (item->get<std::uint64_t>() == 0)
                    add("scenario.structure.invalid_object_shape", item_path, false,
                        {{"classification", "nonpositive_count"}});
            } else {
                static constexpr std::array all{std::string_view{"all"}};
                token(*item, item_path, all);
            }
        }
        if (const auto* item = member(value, "ordering"))
            ordering_token(*item, child_pointer(path, "ordering"));
    }

    void validate_where(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"packages",      "numa_nodes", "dies",
                                          "cache_domains", "allowed",    "online"};
        object_members(value, path, none, known);
        for (const auto* field : {"packages", "numa_nodes", "dies", "cache_domains"})
            if (const auto* item = member(value, field))
                unsigned_array(*item, child_pointer(path, field), false, false);
        for (const auto* field : {"allowed", "online"})
            if (const auto* item = member(value, field))
                require_boolean(*item, child_pointer(path, field));
    }

    void validate_exclude(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"pus", "cores", "worker_sets"};
        object_members(value, path, none, known);
        for (const auto* field : {"pus", "cores"})
            if (const auto* item = member(value, field))
                unsigned_array(*item, child_pointer(path, field), false, false);
        if (const auto* item = member(value, "worker_sets"))
            string_array(*item, child_pointer(path, "worker_sets"), true);
    }

    void validate_smt(const Json& value, const std::string& path) {
        if (value.is_string()) {
            static constexpr std::array modes{std::string_view{"one-per-core"},
                                              std::string_view{"all"}};
            token(value, path, modes);
            return;
        }
        if (!require_object(value, path))
            return;
        static constexpr std::array required{"indices"};
        static constexpr std::array known{"indices"};
        object_members(value, path, required, known);
        if (const auto* item = member(value, "indices"))
            unsigned_array(*item, child_pointer(path, "indices"), true, true);
    }

    void validate_workloads(const Json& value, std::string_view path) {
        if (!require_object(value, path))
            return;
        if (value.empty())
            add("scenario.structure.empty_collection", std::string{path}, false,
                {{"limit_name", "minimum_properties"}, {"limit", "1"}});
        for (auto iterator = value.begin(); iterator != value.end() && !stopped_; ++iterator) {
            const auto workload_path = child_pointer(path, iterator.key());
            Json key_value = iterator.key();
            identifier(key_value, workload_path, true);
            if (!require_object(*iterator, workload_path))
                continue;
            static constexpr std::array required{"kernel"};
            static constexpr std::array known{"kernel", "parameters"};
            object_members(*iterator, workload_path, required, known);
            if (const auto* item = member(*iterator, "kernel"))
                require_string(*item, child_pointer(workload_path, "kernel"), true);
            if (const auto* item = member(*iterator, "parameters"))
                require_object(*item, child_pointer(workload_path, "parameters"));
        }
    }

    void validate_experiments(const Json& value, std::string_view path) {
        if (!value.is_array()) {
            invalid_type(std::string{path}, value, "array");
            return;
        }
        if (value.empty())
            add("scenario.structure.empty_collection", std::string{path}, false,
                {{"limit_name", "minimum_items"}, {"limit", "1"}});
        std::map<std::string, SourceLocation> ids;
        for (std::size_t index = 0; index < value.size() && !stopped_; ++index) {
            const auto experiment_path = index_pointer(path, index);
            validate_experiment(value[index], experiment_path, ids);
        }
    }

    void validate_experiment(const Json& value, const std::string& path,
                             std::map<std::string, SourceLocation>& ids) {
        if (!require_object(value, path))
            return;
        static constexpr std::array common_required{"id", "type", "workload"};
        static constexpr std::array union_known{
            "id",           "type",         "workload",         "workers", "variants",  "compare",
            "worker_nodes", "memory_nodes", "workers_per_node", "memory",  "comparison"};
        const auto* type = member(value, "type");
        const bool comparison =
            type && type->is_string() && type->get_ref<const std::string&>() == "comparison";
        const bool matrix =
            type && type->is_string() && type->get_ref<const std::string&>() == "node-pair-matrix";
        if (comparison) {
            static constexpr std::array required{"id",      "type",     "workload",
                                                 "workers", "variants", "compare"};
            static constexpr std::array known{"id",      "type",     "workload",
                                              "workers", "variants", "compare"};
            object_members(value, path, required, known);
        } else if (matrix) {
            static constexpr std::array required{"id",           "type",         "workload",
                                                 "worker_nodes", "memory_nodes", "workers_per_node",
                                                 "comparison"};
            static constexpr std::array known{"id",           "type",         "workload",
                                              "worker_nodes", "memory_nodes", "workers_per_node",
                                              "memory",       "comparison"};
            object_members(value, path, required, known);
        } else {
            object_members(value, path, common_required, union_known);
        }

        if (const auto* id = member(value, "id")) {
            const auto id_path = child_pointer(path, "id");
            identifier(*id, id_path);
            if (id->is_string() && ScenarioIdentifier::parse(id->get_ref<const std::string&>())) {
                const auto id_location = location(id_path, false);
                const auto [first, inserted] = ids.emplace(id->get_ref<const std::string&>(),
                                                           id_location.value_or(SourceLocation{}));
                if (!inserted) {
                    std::map<std::string, std::string> context;
                    context.emplace("first_line", std::to_string(first->second.line));
                    context.emplace("first_column", std::to_string(first->second.column));
                    if (id_location) {
                        context.emplace("duplicate_line", std::to_string(id_location->line));
                        context.emplace("duplicate_column", std::to_string(id_location->column));
                    }
                    add("scenario.structure.duplicate_identifier", id_path, false,
                        std::move(context));
                }
            }
        }
        if (const auto* workload = member(value, "workload"))
            identifier(*workload, child_pointer(path, "workload"));
        if (!type)
            return;
        if (!type->is_string()) {
            invalid_type(child_pointer(path, "type"), *type, "string");
            return;
        }
        if (!comparison && !matrix) {
            add("scenario.structure.invalid_object_shape", child_pointer(path, "type"), false,
                {{"classification", "unsupported_experiment_type"}});
            return;
        }
        if (comparison)
            validate_comparison_experiment(value, path);
        else
            validate_matrix_experiment(value, path);
    }

    void validate_comparison_experiment(const Json& value, const std::string& path) {
        if (const auto* item = member(value, "workers"))
            identifier(*item, child_pointer(path, "workers"));
        if (const auto* variants = member(value, "variants")) {
            const auto variants_path = child_pointer(path, "variants");
            if (!variants->is_array())
                invalid_type(variants_path, *variants, "array");
            else {
                if (variants->size() < 2)
                    add("scenario.structure.collection_limit", variants_path, false,
                        {{"limit_name", "minimum_items"}, {"limit", "2"}});
                std::map<std::string, SourceLocation> ids;
                for (std::size_t index = 0; index < variants->size() && !stopped_; ++index)
                    validate_variant((*variants)[index], index_pointer(variants_path, index), ids);
            }
        }
        if (const auto* item = member(value, "compare"))
            validate_compare(*item, child_pointer(path, "compare"));
    }

    void validate_variant(const Json& value, const std::string& path,
                          std::map<std::string, SourceLocation>& ids) {
        if (!require_object(value, path))
            return;
        static constexpr std::array required{"id", "memory"};
        static constexpr std::array known{"id", "memory"};
        object_members(value, path, required, known);
        if (const auto* id = member(value, "id")) {
            const auto id_path = child_pointer(path, "id");
            identifier(*id, id_path);
            if (id->is_string() && ScenarioIdentifier::parse(id->get_ref<const std::string&>())) {
                const auto id_location = location(id_path, false);
                const auto [first, inserted] = ids.emplace(id->get_ref<const std::string&>(),
                                                           id_location.value_or(SourceLocation{}));
                if (!inserted)
                    add("scenario.structure.duplicate_identifier", id_path, false,
                        {{"first_line", std::to_string(first->second.line)},
                         {"first_column", std::to_string(first->second.column)},
                         {"duplicate_line", std::to_string(id_location ? id_location->line : 0)},
                         {"duplicate_column",
                          std::to_string(id_location ? id_location->column : 0)}});
            }
        }
        if (const auto* item = member(value, "memory"))
            validate_comparison_memory(*item, child_pointer(path, "memory"));
    }

    void validate_comparison_memory(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array required{"nodes"};
        static constexpr std::array known{
            "policy",        "nodes",       "page_mode", "prefault", "placement_threshold",
            "verify_before", "verify_after"};
        object_members(value, path, required, known);
        validate_memory_fields(value, path);
        if (const auto* nodes = member(value, "nodes")) {
            const auto nodes_path = child_pointer(path, "nodes");
            if (nodes->is_array())
                unsigned_array(*nodes, nodes_path, true, true);
            else if (nodes->is_object()) {
                const auto per_worker = nodes->contains("local-to-each-worker");
                const auto member_name = per_worker ? "local-to-each-worker" : "local-to-workers";
                const std::array local_required{member_name};
                static constexpr std::array local_known{"local-to-workers",
                                                        "local-to-each-worker"};
                object_members(*nodes, nodes_path, local_required, local_known);
                if (nodes->contains("local-to-workers") && per_worker)
                    add("scenario.structure.invalid_object_shape", nodes_path, false,
                        {{"classification", "mutually_exclusive"}});
                if (const auto* local = member(*nodes, member_name)) {
                    if (!local->is_boolean())
                        invalid_type(child_pointer(nodes_path, member_name), *local,
                                     "boolean");
                    else if (!local->get<bool>())
                        add("scenario.structure.invalid_object_shape",
                            child_pointer(nodes_path, member_name), false,
                            {{"classification", "required_true"}});
                }
            } else
                invalid_type(nodes_path, *nodes, "array_or_object");
        }
    }

    void validate_memory_fields(const Json& value, const std::string& path) {
        static constexpr std::array bind{std::string_view{"bind"}};
        static constexpr std::array base{std::string_view{"base"}};
        if (const auto* item = member(value, "policy"))
            token(*item, child_pointer(path, "policy"), bind);
        if (const auto* item = member(value, "page_mode"))
            token(*item, child_pointer(path, "page_mode"), base);
        if (const auto* item = member(value, "placement_threshold"))
            require_string(*item, child_pointer(path, "placement_threshold"), true);
        for (const auto* field : {"prefault", "verify_before", "verify_after"})
            if (const auto* item = member(value, field))
                require_boolean(*item, child_pointer(path, field));
    }

    void validate_compare(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array required{"baseline", "candidates"};
        static constexpr std::array known{"baseline", "candidates", "practical_effect",
                                          "confidence", "order"};
        object_members(value, path, required, known);
        if (const auto* item = member(value, "baseline"))
            identifier(*item, child_pointer(path, "baseline"));
        if (const auto* item = member(value, "candidates"))
            string_array(*item, child_pointer(path, "candidates"), true, true, true);
        for (const auto* field : {"practical_effect", "confidence"})
            if (const auto* item = member(value, field))
                require_string(*item, child_pointer(path, field), true);
        if (const auto* item = member(value, "order"))
            validate_order(*item, child_pointer(path, "order"));
    }

    void validate_order(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"strategy", "seed"};
        object_members(value, path, none, known);
        if (const auto* item = member(value, "strategy"))
            comparison_strategy(*item, child_pointer(path, "strategy"));
        if (const auto* item = member(value, "seed"))
            require_unsigned(*item, child_pointer(path, "seed"));
    }

    void validate_matrix_experiment(const Json& value, const std::string& path) {
        validate_matrix_axis(member(value, "worker_nodes"), child_pointer(path, "worker_nodes"),
                             "all-cpu-nodes");
        validate_matrix_axis(member(value, "memory_nodes"), child_pointer(path, "memory_nodes"),
                             "all-memory-nodes");
        if (const auto* item = member(value, "workers_per_node")) {
            const auto item_path = child_pointer(path, "workers_per_node");
            if (require_object(*item, item_path)) {
                static constexpr std::array required{"select"};
                static constexpr std::array known{"select"};
                object_members(*item, item_path, required, known);
                if (const auto* selector = member(*item, "select"))
                    validate_selector(*selector, child_pointer(item_path, "select"));
            }
        }
        if (const auto* item = member(value, "memory"))
            validate_matrix_memory(*item, child_pointer(path, "memory"));
        if (const auto* item = member(value, "comparison"))
            validate_matrix_comparison(*item, child_pointer(path, "comparison"));
    }

    void validate_matrix_axis(const Json* value, const std::string& path,
                              std::string_view expected) {
        if (!value || !require_object(*value, path))
            return;
        static constexpr std::array required{"select"};
        static constexpr std::array known{"select"};
        object_members(*value, path, required, known);
        if (const auto* item = member(*value, "select")) {
            const std::array tokens{expected};
            token(*item, child_pointer(path, "select"), tokens);
        }
    }

    void validate_matrix_memory(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"policy",        "page_mode",
                                          "prefault",      "placement_threshold",
                                          "verify_before", "verify_after"};
        object_members(value, path, none, known);
        validate_memory_fields(value, path);
    }

    void validate_matrix_comparison(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array required{"baseline"};
        static constexpr std::array known{"baseline", "practical_effect", "confidence", "order"};
        object_members(value, path, required, known);
        static constexpr std::array baseline{std::string_view{"local-memory"}};
        if (const auto* item = member(value, "baseline"))
            token(*item, child_pointer(path, "baseline"), baseline);
        for (const auto* field : {"practical_effect", "confidence"})
            if (const auto* item = member(value, field))
                require_string(*item, child_pointer(path, field), true);
        if (const auto* item = member(value, "order"))
            validate_order(*item, child_pointer(path, "order"));
    }

    void validate_measurement(const Json& value, std::string_view path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"clock",      "observation", "calibration", "warmup",
                                          "repetition", "stopping",    "statistics",  "outliers"};
        object_members(value, path, none, known);
        static constexpr std::array clock{std::string_view{"monotonic-raw"}};
        if (const auto* item = member(value, "clock"))
            token(*item, child_pointer(path, "clock"), clock);
        validate_simple_object(
            member(value, "observation"), child_pointer(path, "observation"),
            {"target_duration", "minimum_duration", "maximum_duration", "maximum_harness_overhead"},
            {},
            {"target_duration", "minimum_duration", "maximum_duration",
             "maximum_harness_overhead"});
        validate_calibration(member(value, "calibration"), child_pointer(path, "calibration"));
        validate_simple_object(member(value, "warmup"), child_pointer(path, "warmup"),
                               {"minimum", "maximum", "stability_window", "stability_threshold"},
                               {"minimum", "maximum", "stability_window"}, {"stability_threshold"});
        validate_simple_object(
            member(value, "repetition"), child_pointer(path, "repetition"),
            {"minimum_blocks", "maximum_blocks", "minimum_valid_observations_per_variant"},
            {"minimum_blocks", "maximum_blocks", "minimum_valid_observations_per_variant"}, {});
        validate_stopping(member(value, "stopping"), child_pointer(path, "stopping"));
        validate_simple_object(member(value, "statistics"), child_pointer(path, "statistics"),
                               {"bootstrap_samples", "practical_effect"}, {"bootstrap_samples"},
                               {"practical_effect"});
        validate_outliers(member(value, "outliers"), child_pointer(path, "outliers"));
    }

    void validate_simple_object(const Json* value, const std::string& path,
                                std::initializer_list<const char*> known,
                                std::initializer_list<const char*> unsigned_fields,
                                std::initializer_list<const char*> string_fields) {
        if (!value || !require_object(*value, path))
            return;
        for (auto iterator = value->begin(); iterator != value->end(); ++iterator) {
            if (std::ranges::find(known, iterator.key()) == known.end())
                add("scenario.structure.unknown_field", child_pointer(path, iterator.key()), true);
        }
        for (const auto* field : known) {
            const auto* item = member(*value, field);
            if (!item)
                continue;
            if (std::ranges::find(unsigned_fields, field) != unsigned_fields.end())
                require_unsigned(*item, child_pointer(path, field));
            else if (std::ranges::find(string_fields, field) != string_fields.end())
                require_string(*item, child_pointer(path, field), true);
        }
    }

    void validate_calibration(const Json* value, const std::string& path) {
        if (!value || !require_object(*value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"enabled", "strategy"};
        object_members(*value, path, none, known);
        if (const auto* item = member(*value, "enabled"))
            require_boolean(*item, child_pointer(path, "enabled"));
        static constexpr std::array strategies{std::string_view{"geometric-refine"}};
        if (const auto* item = member(*value, "strategy"))
            token(*item, child_pointer(path, "strategy"), strategies);
    }

    void validate_stopping(const Json* value, const std::string& path) {
        if (!value || !require_object(*value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"enabled", "confidence",
                                          "ratio_interval_relative_half_width"};
        object_members(*value, path, none, known);
        if (const auto* item = member(*value, "enabled"))
            require_boolean(*item, child_pointer(path, "enabled"));
        for (const auto* field : {"confidence", "ratio_interval_relative_half_width"})
            if (const auto* item = member(*value, field))
                require_string(*item, child_pointer(path, field), true);
    }

    void validate_outliers(const Json* value, const std::string& path) {
        if (!value || !require_object(*value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"flagging", "automatic_exclusion"};
        object_members(*value, path, none, known);
        static constexpr std::array flagging{std::string_view{"median-absolute-deviation"}};
        if (const auto* item = member(*value, "flagging"))
            token(*item, child_pointer(path, "flagging"), flagging);
        if (const auto* item = member(*value, "automatic_exclusion")) {
            const auto pointer = child_pointer(path, "automatic_exclusion");
            require_boolean(*item, pointer);
            if (item->is_boolean() && item->get<bool>()) {
                add("scenario.structure.invalid_object_shape", pointer, false,
                    {{"classification", "unsupported_automatic_exclusion"}});
            }
        }
    }

    void validate_monitoring(const Json& value, std::string_view path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"required", "optional", "sampling",
                                          "unavailable_optional_source"};
        object_members(value, path, none, known);
        for (const auto* field : {"required", "optional"})
            if (const auto* item = member(value, field))
                string_array(*item, child_pointer(path, field));
        static constexpr std::array policy{std::string_view{"fail"}, std::string_view{"warn"},
                                           std::string_view{"disable"}};
        if (const auto* item = member(value, "unavailable_optional_source"))
            token(*item, child_pointer(path, "unavailable_optional_source"), policy);
        if (const auto* sampling = member(value, "sampling"))
            validate_sampling(*sampling, child_pointer(path, "sampling"));
    }

    void validate_sampling(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"before_observation", "after_observation",
                                          "during_observation"};
        object_members(value, path, none, known);
        for (const auto* field : {"before_observation", "after_observation"})
            if (const auto* item = member(value, field))
                require_boolean(*item, child_pointer(path, field));
        if (const auto* during = member(value, "during_observation")) {
            const auto during_path = child_pointer(path, "during_observation");
            if (require_object(*during, during_path)) {
                static constexpr std::array fields{"enabled", "interval"};
                object_members(*during, during_path, none, fields);
                if (const auto* item = member(*during, "enabled"))
                    require_boolean(*item, child_pointer(during_path, "enabled"));
                if (const auto* item = member(*during, "interval"))
                    require_string(*item, child_pointer(during_path, "interval"), true);
            }
        }
    }

    void validate_output(const Json& value, std::string_view path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{"directory", "format", "retain", "console"};
        object_members(value, path, none, known);
        if (const auto* item = member(value, "directory"))
            require_string(*item, child_pointer(path, "directory"));
        static constexpr std::array format{std::string_view{"json"}};
        if (const auto* item = member(value, "format"))
            token(*item, child_pointer(path, "format"), format);
        if (const auto* retain = member(value, "retain"))
            validate_retain(*retain, child_pointer(path, "retain"));
        if (const auto* console = member(value, "console")) {
            const auto console_path = child_pointer(path, "console");
            if (require_object(*console, console_path)) {
                static constexpr std::array fields{"level"};
                object_members(*console, console_path, none, fields);
                static constexpr std::array levels{std::string_view{"summary"}};
                if (const auto* item = member(*console, "level"))
                    token(*item, child_pointer(console_path, "level"), levels);
            }
        }
    }

    void validate_retain(const Json& value, const std::string& path) {
        if (!require_object(value, path))
            return;
        static constexpr std::array<const char*, 0> none{};
        static constexpr std::array known{
            "authored_scenario", "normalized_scenario", "resolved_plan",     "topology_snapshot",
            "warmups",           "raw_observations",    "monitoring_samples"};
        object_members(value, path, none, known);
        for (const auto* field : known)
            if (const auto* item = member(value, field))
                require_boolean(*item, child_pointer(path, field));
    }
};

} // namespace

auto validate_v1_structure(const AuthoredScenario& authored) -> std::vector<ScenarioDiagnostic> {
    return Validator{authored}.run();
}

} // namespace epycbench::scenario::detail
