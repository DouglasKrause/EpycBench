#include "scenario/scenario_v1_defaults.hpp"
#include "core/json_ownership.hpp"
#include "scenario/json_cleanup.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace epycbench::scenario::detail {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::array bindings{
    DefaultBinding{"/experiments/*/variants/*/memory/policy", DefaultFamily::memory, 0},
    DefaultBinding{"/experiments/*/variants/*/memory/page_mode", DefaultFamily::memory, 1},
    DefaultBinding{"/experiments/*/variants/*/memory/placement_threshold", DefaultFamily::memory,
                   2},
    DefaultBinding{"/experiments/*/variants/*/memory/prefault", DefaultFamily::memory, 3},
    DefaultBinding{"/experiments/*/variants/*/memory/verify_before", DefaultFamily::memory, 4},
    DefaultBinding{"/experiments/*/variants/*/memory/verify_after", DefaultFamily::memory, 5},
    DefaultBinding{"/experiments/*/memory/policy", DefaultFamily::memory, 6},
    DefaultBinding{"/experiments/*/memory/page_mode", DefaultFamily::memory, 7},
    DefaultBinding{"/experiments/*/memory/placement_threshold", DefaultFamily::memory, 8},
    DefaultBinding{"/experiments/*/memory/prefault", DefaultFamily::memory, 9},
    DefaultBinding{"/experiments/*/memory/verify_before", DefaultFamily::memory, 10},
    DefaultBinding{"/experiments/*/memory/verify_after", DefaultFamily::memory, 11},
    DefaultBinding{"/resources/worker_sets/*/select/smt", DefaultFamily::worker_selector, 12},
    DefaultBinding{"/resources/worker_sets/*/select/ordering", DefaultFamily::worker_selector, 13},
    DefaultBinding{"/experiments/*/workers/select/smt", DefaultFamily::worker_selector, 14},
    DefaultBinding{"/experiments/*/workers/select/ordering", DefaultFamily::worker_selector, 15},
    DefaultBinding{"/experiments/*/workers_per_node/select/smt", DefaultFamily::worker_selector,
                   16},
    DefaultBinding{"/experiments/*/workers_per_node/select/ordering",
                   DefaultFamily::worker_selector, 17},
    DefaultBinding{"/experiments/*/compare/practical_effect", DefaultFamily::comparison, 18},
    DefaultBinding{"/experiments/*/compare/confidence", DefaultFamily::comparison, 19},
    DefaultBinding{"/experiments/*/compare/order/strategy", DefaultFamily::comparison, 20},
    DefaultBinding{"/experiments/*/comparison/practical_effect", DefaultFamily::comparison, 21},
    DefaultBinding{"/experiments/*/comparison/confidence", DefaultFamily::comparison, 22},
    DefaultBinding{"/experiments/*/comparison/order/strategy", DefaultFamily::comparison, 23},
    DefaultBinding{"/measurement/repetition/minimum_blocks", DefaultFamily::repetition, 24},
    DefaultBinding{"/measurement/repetition/maximum_blocks", DefaultFamily::repetition, 25},
    DefaultBinding{"/measurement/repetition/minimum_valid_observations_per_variant",
                   DefaultFamily::repetition, 26},
    DefaultBinding{"/measurement/observation/target_duration", DefaultFamily::observation, 27},
    DefaultBinding{"/measurement/observation/minimum_duration", DefaultFamily::observation, 28},
    DefaultBinding{"/measurement/observation/maximum_duration", DefaultFamily::observation, 29},
    DefaultBinding{"/measurement/observation/maximum_harness_overhead", DefaultFamily::observation,
                   30},
    DefaultBinding{"/measurement/calibration/enabled", DefaultFamily::calibration, 31},
    DefaultBinding{"/measurement/calibration/strategy", DefaultFamily::calibration, 32},
    DefaultBinding{"/measurement/warmup/minimum", DefaultFamily::warmup, 33},
    DefaultBinding{"/measurement/warmup/maximum", DefaultFamily::warmup, 34},
    DefaultBinding{"/measurement/warmup/stability_window", DefaultFamily::warmup, 35},
    DefaultBinding{"/measurement/warmup/stability_threshold", DefaultFamily::warmup, 36},
    DefaultBinding{"/measurement/stopping/enabled", DefaultFamily::stopping, 37},
    DefaultBinding{"/measurement/stopping/confidence", DefaultFamily::stopping, 38},
    DefaultBinding{"/measurement/stopping/ratio_interval_relative_half_width",
                   DefaultFamily::stopping, 39},
    DefaultBinding{"/measurement/statistics/bootstrap_samples", DefaultFamily::statistics, 40},
    DefaultBinding{"/measurement/statistics/practical_effect", DefaultFamily::statistics, 41},
    DefaultBinding{"/measurement/outliers/flagging", DefaultFamily::statistics, 42},
    DefaultBinding{"/random/seed", DefaultFamily::random_seed, 43},
};

auto duration(std::uint64_t value) -> JsonOwner {
    return json_object([&](Json& json_target_0) {
        set_json(json_target_0, "unit", "nanosecond");
        set_json(json_target_0, "value", value);
    });
}

auto ratio(std::uint64_t numerator, std::uint64_t denominator) -> JsonOwner {
    return json_object([&](Json& json_target_1) {
        set_json(json_target_1, "unit", "ratio");
        set_json(json_target_1, "numerator", numerator);
        set_json(json_target_1, "denominator", denominator);
    });
}

auto child_pointer(std::string_view parent, std::string_view child) -> std::string {
    return std::string{parent} + "/" + std::string{child};
}

auto index_pointer(std::string_view parent, std::size_t index) -> std::string {
    return std::string{parent} + "/" + std::to_string(index);
}

auto require_object(Json& parent, std::string_view field, std::string_view binding)
    -> std::expected<Json*, core::Error> {
    if (!parent.contains(field)) {
        set_json(parent, field, Json::object());
    }
    auto& value = parent.at(field);
    if (!value.is_object()) {
        return std::unexpected{
            make_defaults_internal_error("expected_object", std::string{binding})};
    }
    return &value;
}

auto existing_object(Json& parent, std::string_view field, std::string_view binding)
    -> std::expected<Json*, core::Error> {
    if (!parent.contains(field))
        return static_cast<Json*>(nullptr);
    auto& value = parent.at(field);
    if (!value.is_object()) {
        return std::unexpected{
            make_defaults_internal_error("expected_object", std::string{binding})};
    }
    return &value;
}

void record(std::map<std::string, ValueOrigin>& origins, std::string target, ValueOriginKind kind,
            std::optional<std::string> source = std::nullopt) {
    origins.insert_or_assign(std::move(target),
                             ValueOrigin{.kind = kind, .source_pointer = std::move(source)});
}

void apply(Json& object, std::string_view field, const Json& fallback, ValueOrigin fallback_origin,
           ValueOriginKind explicit_origin, const std::string& parent,
           std::map<std::string, ValueOrigin>& origins) {
    auto target = child_pointer(parent, field);
    if (object.contains(field)) {
        record(origins, std::move(target), explicit_origin, child_pointer(parent, field));
        return;
    }
    set_json(object, field, fallback);
    record(origins, std::move(target), fallback_origin.kind, fallback_origin.source_pointer);
}

template <typename Field, std::size_t Size>
void reorder(Json& object, const std::array<Field, Size>& fields) {
    Json ordered = Json::object();
    JsonCleanup ordered_cleanup{ordered};
    for (const auto field : fields) {
        if (object.contains(field))
            set_json(ordered, field, std::move(object.at(field)));
    }
    for (auto& item : object.items()) {
        if (!ordered.contains(item.key()))
            set_json(ordered, item.key(), std::move(item.value()));
    }
    object.swap(ordered);
}

class Expander {
  public:
    Expander(Json& effective, std::map<std::string, ValueOrigin>& origins)
        : effective_(effective), origins_(origins) {}

    ~Expander() {
        if (defaults_)
            retire_json(*defaults_);
    }

    auto run() -> std::expected<void, core::Error> {
        if (!effective_.is_object())
            return std::unexpected{make_defaults_internal_error("root_not_object", "root")};
        if (effective_.contains("defaults"))
            defaults_ = clone_json(effective_.at("defaults"));
        if (defaults_ && !defaults_->is_object())
            return std::unexpected{make_defaults_internal_error("expected_object", "defaults")};

        if (auto result = expand_workers(); !result)
            return result;
        if (auto result = expand_experiments(); !result)
            return result;
        if (auto result = expand_measurement(); !result)
            return result;
        if (auto result = expand_seed(); !result)
            return result;

        if (effective_.contains("defaults"))
            retire_json(effective_.at("defaults"));
        effective_.erase("defaults");
        reorder_root();
        return {};
    }

  private:
    Json& effective_;
    std::map<std::string, ValueOrigin>& origins_;
    std::optional<Json> defaults_;

    auto default_object(std::string_view name) -> std::expected<Json*, core::Error> {
        if (!defaults_)
            return static_cast<Json*>(nullptr);
        return existing_object(*defaults_, name, std::string{"defaults."} + std::string{name});
    }

    auto inherited(Json* declarations, std::string_view field, JsonOwner schema,
                   std::string_view declaration_parent) const -> std::pair<JsonOwner, ValueOrigin> {
        if (declarations != nullptr && declarations->contains(field)) {
            const auto source = child_pointer(declaration_parent, field);
            return {
                JsonOwner{clone_json(declarations->at(field))},
                ValueOrigin{.kind = ValueOriginKind::scenario_default, .source_pointer = source}};
        }
        return {std::move(schema), ValueOrigin{.kind = ValueOriginKind::schema_default,
                                               .source_pointer = std::nullopt}};
    }

    auto expand_memory(Json& memory, const std::string& path, ValueOriginKind explicit_kind,
                       Json* memory_defaults) -> void {
        const auto [page_mode, page_origin] =
            inherited(memory_defaults, "page_mode", Json("base"), "/defaults/memory");
        const auto [threshold, threshold_origin] = inherited(memory_defaults, "placement_threshold",
                                                             ratio(999, 1'000), "/defaults/memory");
        const auto [prefault, prefault_origin] =
            inherited(memory_defaults, "prefault", Json(true), "/defaults/memory");
        const auto [verify_before, before_origin] =
            inherited(memory_defaults, "verify_before", Json(true), "/defaults/memory");
        const auto [verify_after, after_origin] =
            inherited(memory_defaults, "verify_after", Json(true), "/defaults/memory");

        apply(memory, "policy", Json("bind"), ValueOrigin{.kind = ValueOriginKind::schema_default},
              explicit_kind, path, origins_);
        apply(memory, "page_mode", page_mode, page_origin, explicit_kind, path, origins_);
        apply(memory, "placement_threshold", threshold, threshold_origin, explicit_kind, path,
              origins_);
        apply(memory, "prefault", prefault, prefault_origin, explicit_kind, path, origins_);
        apply(memory, "verify_before", verify_before, before_origin, explicit_kind, path, origins_);
        apply(memory, "verify_after", verify_after, after_origin, explicit_kind, path, origins_);
        constexpr std::array order{
            "policy",        "nodes",       "page_mode", "prefault", "placement_threshold",
            "verify_before", "verify_after"};
        reorder(memory, order);
    }

    auto expand_selector(Json& selector, const std::string& path, Json* worker_defaults) -> void {
        const auto [smt, smt_origin] =
            inherited(worker_defaults, "smt", Json("one-per-core"), "/defaults/workers");
        const auto [ordering, ordering_origin] =
            inherited(worker_defaults, "ordering", Json("ascending-pu"), "/defaults/workers");
        apply(selector, "smt", smt, smt_origin, ValueOriginKind::explicit_value, path, origins_);
        apply(selector, "ordering", ordering, ordering_origin, ValueOriginKind::explicit_value,
              path, origins_);
        constexpr std::array order{"unit", "where", "exclude", "smt", "count", "ordering"};
        reorder(selector, order);
    }

    auto expand_workers() -> std::expected<void, core::Error> {
        auto worker_defaults = default_object("workers");
        if (!worker_defaults)
            return std::unexpected{std::move(worker_defaults.error())};
        if (!effective_.contains("resources"))
            return {};
        auto& resources = effective_.at("resources");
        if (!resources.is_object())
            return std::unexpected{make_defaults_internal_error("expected_object", "resources")};
        if (!resources.contains("worker_sets"))
            return {};
        auto& sets = resources.at("worker_sets");
        if (!sets.is_object())
            return std::unexpected{make_defaults_internal_error("expected_object", "worker_sets")};
        for (auto& item : sets.items()) {
            auto& selection = item.value();
            if (!selection.is_object())
                return std::unexpected{
                    make_defaults_internal_error("expected_object", "worker_selection")};
            if (!selection.contains("select"))
                continue;
            auto& selector = selection.at("select");
            if (!selector.is_object())
                return std::unexpected{
                    make_defaults_internal_error("expected_object", "worker_selector")};
            expand_selector(selector, "/resources/worker_sets/" + item.key() + "/select",
                            *worker_defaults);
        }
        return {};
    }

    auto expand_comparison(Json& experiment, std::size_t experiment_index, Json* memory_defaults,
                           Json* comparison_defaults) -> std::expected<void, core::Error> {
        const auto experiment_path = index_pointer("/experiments", experiment_index);
        auto& variants = experiment.at("variants");
        if (!variants.is_array())
            return std::unexpected{
                make_defaults_internal_error("expected_array", "comparison_variants")};
        for (std::size_t index = 0; index < variants.size(); ++index) {
            auto& memory = variants[index].at("memory");
            if (!memory.is_object())
                return std::unexpected{
                    make_defaults_internal_error("expected_object", "variant_memory")};
            expand_memory(memory,
                          index_pointer(child_pointer(experiment_path, "variants"), index) +
                              "/memory",
                          ValueOriginKind::variant_override, memory_defaults);
        }
        auto& compare = experiment.at("compare");
        return expand_comparison_settings(compare, child_pointer(experiment_path, "compare"),
                                          ValueOriginKind::experiment_override,
                                          comparison_defaults);
    }

    auto expand_matrix(Json& experiment, std::size_t experiment_index, Json* memory_defaults,
                       Json* worker_defaults, Json* comparison_defaults)
        -> std::expected<void, core::Error> {
        const auto experiment_path = index_pointer("/experiments", experiment_index);
        if (experiment.contains("memory")) {
            auto& memory = experiment.at("memory");
            if (!memory.is_object())
                return std::unexpected{
                    make_defaults_internal_error("expected_object", "matrix_memory")};
            expand_memory(memory, child_pointer(experiment_path, "memory"),
                          ValueOriginKind::matrix_override, memory_defaults);
        }
        auto& workers = experiment.at("workers_per_node");
        if (!workers.is_object() || !workers.contains("select") ||
            !workers.at("select").is_object()) {
            return std::unexpected{
                make_defaults_internal_error("expected_object", "matrix_worker_selector")};
        }
        expand_selector(workers.at("select"),
                        child_pointer(experiment_path, "workers_per_node/select"), worker_defaults);
        auto& comparison = experiment.at("comparison");
        return expand_comparison_settings(comparison, child_pointer(experiment_path, "comparison"),
                                          ValueOriginKind::matrix_override, comparison_defaults);
    }

    auto expand_comparison_settings(Json& object, const std::string& path,
                                    ValueOriginKind explicit_kind, Json* comparison_defaults)
        -> std::expected<void, core::Error> {
        if (!object.is_object())
            return std::unexpected{
                make_defaults_internal_error("expected_object", "comparison_settings")};
        apply(object, "practical_effect", ratio(1, 100),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, explicit_kind, path, origins_);
        apply(object, "confidence", ratio(19, 20),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, explicit_kind, path, origins_);

        auto order = require_object(object, "order", "comparison_order");
        if (!order)
            return std::unexpected{std::move(order.error())};
        const auto [strategy, strategy_origin] = inherited(
            comparison_defaults, "order", Json("randomized-abba"), "/defaults/comparison");
        apply(**order, "strategy", strategy, strategy_origin, explicit_kind,
              child_pointer(path, "order"), origins_);
        constexpr std::array order_fields{"strategy", "seed"};
        reorder(**order, order_fields);
        constexpr std::array compare_fields{"baseline", "candidates", "practical_effect",
                                            "confidence", "order"};
        reorder(object, compare_fields);
        return {};
    }

    auto expand_experiments() -> std::expected<void, core::Error> {
        if (!effective_.contains("experiments") || !effective_.at("experiments").is_array())
            return std::unexpected{make_defaults_internal_error("expected_array", "experiments")};
        auto memory_defaults = default_object("memory");
        if (!memory_defaults)
            return std::unexpected{std::move(memory_defaults.error())};
        auto worker_defaults = default_object("workers");
        if (!worker_defaults)
            return std::unexpected{std::move(worker_defaults.error())};
        auto comparison_defaults = default_object("comparison");
        if (!comparison_defaults)
            return std::unexpected{std::move(comparison_defaults.error())};

        auto& experiments = effective_.at("experiments");
        for (std::size_t index = 0; index < experiments.size(); ++index) {
            auto& experiment = experiments[index];
            if (!experiment.is_object() || !experiment.contains("type") ||
                !experiment.at("type").is_string()) {
                return std::unexpected{
                    make_defaults_internal_error("invalid_experiment", "experiment")};
            }
            const auto& type = experiment.at("type").get_ref<const std::string&>();
            if (type == "comparison") {
                if (auto result = expand_comparison(experiment, index, *memory_defaults,
                                                    *comparison_defaults);
                    !result)
                    return result;
            } else if (type == "node-pair-matrix") {
                if (auto result = expand_matrix(experiment, index, *memory_defaults,
                                                *worker_defaults, *comparison_defaults);
                    !result)
                    return result;
            } else {
                return std::unexpected{
                    make_defaults_internal_error("invalid_experiment_discriminator", "experiment")};
            }
        }
        return {};
    }

    auto expand_measurement() -> std::expected<void, core::Error> {
        auto measurement = require_object(effective_, "measurement", "measurement");
        if (!measurement)
            return std::unexpected{std::move(measurement.error())};
        constexpr std::array measurement_children{"observation", "calibration", "warmup",
                                                  "repetition",  "stopping",    "statistics",
                                                  "outliers"};
        for (const auto* child : measurement_children) {
            if (auto ensured = require_object(**measurement, child, child); !ensured)
                return std::unexpected{std::move(ensured.error())};
        }
        auto& measurement_object = **measurement;
        auto& observation = measurement_object.at("observation");
        auto& calibration = measurement_object.at("calibration");
        auto& warmup = measurement_object.at("warmup");
        auto& repetition = measurement_object.at("repetition");
        auto& stopping = measurement_object.at("stopping");
        auto& statistics = measurement_object.at("statistics");
        auto& outliers = measurement_object.at("outliers");

        auto observation_defaults = default_object("observation");
        auto comparison_defaults = default_object("comparison");
        if (!observation_defaults || !comparison_defaults)
            return std::unexpected{
                make_defaults_internal_error("defaults_child_type", "measurement")};

        const auto [target, target_origin] =
            inherited(*observation_defaults, "target_duration", duration(500'000'000),
                      "/defaults/observation");
        apply(observation, "target_duration", target, target_origin,
              ValueOriginKind::explicit_value, "/measurement/observation", origins_);
        apply(observation, "minimum_duration", duration(250'000'000),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/observation", origins_);
        apply(observation, "maximum_duration", duration(1'000'000'000),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/observation", origins_);
        apply(observation, "maximum_harness_overhead", ratio(1, 100),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/observation", origins_);

        apply(calibration, "enabled", Json(true),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/calibration", origins_);
        apply(calibration, "strategy", Json("geometric-refine"),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/calibration", origins_);

        apply(warmup, "minimum", Json(std::uint64_t{3}),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/warmup", origins_);
        apply(warmup, "maximum", Json(std::uint64_t{10}),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/warmup", origins_);
        apply(warmup, "stability_window", Json(std::uint64_t{3}),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/warmup", origins_);
        apply(warmup, "stability_threshold", ratio(3, 100),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/warmup", origins_);

        const auto [minimum_blocks, minimum_origin] = inherited(
            *comparison_defaults, "minimum_blocks", Json(std::uint64_t{8}), "/defaults/comparison");
        const auto [maximum_blocks, maximum_origin] =
            inherited(*comparison_defaults, "maximum_blocks", Json(std::uint64_t{20}),
                      "/defaults/comparison");
        apply(repetition, "minimum_blocks", minimum_blocks, minimum_origin,
              ValueOriginKind::explicit_value, "/measurement/repetition", origins_);
        apply(repetition, "maximum_blocks", maximum_blocks, maximum_origin,
              ValueOriginKind::explicit_value, "/measurement/repetition", origins_);
        apply(repetition, "minimum_valid_observations_per_variant", Json(std::uint64_t{16}),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/repetition", origins_);

        apply(stopping, "enabled", Json(true), ValueOrigin{.kind = ValueOriginKind::schema_default},
              ValueOriginKind::explicit_value, "/measurement/stopping", origins_);
        apply(stopping, "confidence", ratio(19, 20),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/stopping", origins_);
        apply(stopping, "ratio_interval_relative_half_width", ratio(1, 50),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/stopping", origins_);

        apply(statistics, "bootstrap_samples", Json(std::uint64_t{10'000}),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/statistics", origins_);
        apply(statistics, "practical_effect", ratio(1, 100),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/statistics", origins_);
        apply(outliers, "flagging", Json("median-absolute-deviation"),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/measurement/outliers", origins_);

        constexpr std::array observation_order{"target_duration", "minimum_duration",
                                               "maximum_duration", "maximum_harness_overhead"};
        constexpr std::array calibration_order{"enabled", "strategy"};
        constexpr std::array warmup_order{"minimum", "maximum", "stability_window",
                                          "stability_threshold"};
        constexpr std::array repetition_order{"minimum_blocks", "maximum_blocks",
                                              "minimum_valid_observations_per_variant"};
        constexpr std::array stopping_order{"enabled", "confidence",
                                            "ratio_interval_relative_half_width"};
        constexpr std::array statistics_order{"bootstrap_samples", "practical_effect"};
        constexpr std::array outlier_order{"flagging", "automatic_exclusion"};
        reorder(observation, observation_order);
        reorder(calibration, calibration_order);
        reorder(warmup, warmup_order);
        reorder(repetition, repetition_order);
        reorder(stopping, stopping_order);
        reorder(statistics, statistics_order);
        reorder(outliers, outlier_order);
        constexpr std::array measurement_order{"clock",      "observation", "calibration",
                                               "warmup",     "repetition",  "stopping",
                                               "statistics", "outliers"};
        reorder(**measurement, measurement_order);
        return {};
    }

    auto expand_seed() -> std::expected<void, core::Error> {
        auto random = require_object(effective_, "random", "random");
        if (!random)
            return std::unexpected{std::move(random.error())};
        apply(**random, "seed", Json(std::uint64_t{0}),
              ValueOrigin{.kind = ValueOriginKind::schema_default}, ValueOriginKind::explicit_value,
              "/random", origins_);
        constexpr std::array random_order{"seed"};
        reorder(**random, random_order);
        return {};
    }

    void reorder_root() {
        constexpr std::array root_order{"schema",    "metadata",    "requirements", "resources",
                                        "workloads", "experiments", "measurement",  "monitoring",
                                        "output",    "random"};
        reorder(effective_, root_order);
    }
};

} // namespace

auto scenario_v1_default_bindings() noexcept -> std::span<const DefaultBinding> { return bindings; }

auto make_defaults_internal_error(std::string classification, std::string binding) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};
    context["classification"] = std::move(classification);
    if (!binding.empty())
        context["binding"] = std::move(binding);
    return core::Error{
        .category = core::ErrorCategory::scenario,
        .code = core::StableErrorCode{"scenario.defaults.internal_failure"},
        .message = "Scenario default expansion failed internally",
        .operation = "expand_scenario_defaults",
        .os_error = std::nullopt,
        .context = std::move(context),
    };
}

auto expand_v1_defaults(nlohmann::ordered_json& effective,
                        std::map<std::string, ValueOrigin>& origins)
    -> std::expected<void, core::Error> {
    return Expander{effective, origins}.run();
}

} // namespace epycbench::scenario::detail
