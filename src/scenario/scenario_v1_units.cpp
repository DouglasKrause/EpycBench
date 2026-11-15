#include "scenario/scenario_v1_units.hpp"
#include "scenario/json_cleanup.hpp"

#include "scenario/quantity_parser.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace epycbench::scenario::detail {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::array bindings{
    UnitBinding{"/defaults/memory/placement_threshold", QuantityFamily::percentage, 0},
    UnitBinding{"/defaults/observation/target_duration", QuantityFamily::duration, 1},
    UnitBinding{"/experiments/*/variants/*/memory/placement_threshold", QuantityFamily::percentage,
                2},
    UnitBinding{"/experiments/*/memory/placement_threshold", QuantityFamily::percentage, 3},
    UnitBinding{"/experiments/*/compare/practical_effect", QuantityFamily::percentage, 4},
    UnitBinding{"/experiments/*/compare/confidence", QuantityFamily::percentage, 5},
    UnitBinding{"/experiments/*/comparison/practical_effect", QuantityFamily::percentage, 6},
    UnitBinding{"/experiments/*/comparison/confidence", QuantityFamily::percentage, 7},
    UnitBinding{"/measurement/observation/target_duration", QuantityFamily::duration, 8},
    UnitBinding{"/measurement/observation/minimum_duration", QuantityFamily::duration, 9},
    UnitBinding{"/measurement/observation/maximum_duration", QuantityFamily::duration, 10},
    UnitBinding{"/measurement/observation/maximum_harness_overhead", QuantityFamily::percentage,
                11},
    UnitBinding{"/measurement/warmup/stability_threshold", QuantityFamily::percentage, 12},
    UnitBinding{"/measurement/stopping/confidence", QuantityFamily::percentage, 13},
    UnitBinding{"/measurement/stopping/ratio_interval_relative_half_width",
                QuantityFamily::percentage, 14},
    UnitBinding{"/measurement/statistics/practical_effect", QuantityFamily::percentage, 15},
    UnitBinding{"/monitoring/sampling/during_observation/interval", QuantityFamily::duration, 16},
};

auto child_pointer(std::string_view parent, std::string_view child) -> std::string {
    return std::string{parent} + "/" + std::string{child};
}

auto index_pointer(std::string_view parent, std::size_t index) -> std::string {
    return std::string{parent} + "/" + std::to_string(index);
}

auto failure_code(QuantityParseFailure failure) -> std::string_view {
    switch (failure) {
    case QuantityParseFailure::invalid_quantity:
        return "scenario.units.invalid_quantity";
    case QuantityParseFailure::unsupported_unit:
        return "scenario.units.unsupported_unit";
    case QuantityParseFailure::negative_not_allowed:
        return "scenario.units.negative_not_allowed";
    case QuantityParseFailure::inexact_conversion:
        return "scenario.units.inexact_conversion";
    case QuantityParseFailure::out_of_range:
        return "scenario.units.out_of_range";
    case QuantityParseFailure::overflow:
        return "scenario.units.overflow";
    }
    return "scenario.units.internal_failure";
}

auto family_name(QuantityFamily family) -> std::string_view {
    return family == QuantityFamily::duration ? "duration" : "percentage";
}

auto canonical_unit(QuantityFamily family) -> std::string_view {
    return family == QuantityFamily::duration ? "nanosecond" : "ratio";
}

class Normalizer {
  public:
    Normalizer(const StructurallyValidScenario& scenario, Json& normalized)
        : scenario_(scenario), normalized_(normalized) {}

    void run() {
        normalize_defaults();
        normalize_experiments();
        normalize_measurement();
        normalize_monitoring();
    }

  private:
    const StructurallyValidScenario& scenario_;
    Json& normalized_;
    std::vector<ScenarioDiagnostic> diagnostics_;
    bool stopped_{};

    auto location(std::string_view pointer) const -> std::optional<SourceLocation> {
        const auto& locations = scenario_.authored().locations();
        const auto found = locations.find(std::string{pointer});
        if (found == locations.end())
            return std::nullopt;
        return found->second.value;
    }

    void add_diagnostic(std::string_view pointer, QuantityFamily family,
                        const QuantityParseError& parse_error) {
        if (stopped_)
            return;
        if (diagnostics_.size() == 63) {
            diagnostics_.push_back(ScenarioDiagnostic{
                .code = core::StableErrorCode{"scenario.units.diagnostic_limit"},
                .json_pointer = std::string{pointer},
                .location = location(pointer),
                .safe_context = {{"limit_name", "diagnostic_count"}},
            });
            stopped_ = true;
            return;
        }
        diagnostics_.push_back(ScenarioDiagnostic{
            .code = core::StableErrorCode{std::string{failure_code(parse_error.failure)}},
            .json_pointer = std::string{pointer},
            .location = location(pointer),
            .safe_context = {{"quantity_family", std::string{family_name(family)}},
                             {"classification", std::string{parse_error.classification}},
                             {"canonical_unit", std::string{canonical_unit(family)}}},
        });
    }

    auto normalize_field(Json& object, std::string_view field, std::string path,
                         QuantityFamily family) -> std::expected<void, core::Error> {
        if (stopped_ || !object.contains(field))
            return {};
        auto& value = object.at(field);
        if (!value.is_string()) {
            return std::unexpected{make_units_internal_error("bound_field_type_mismatch")};
        }
        const auto& text = value.get_ref<const std::string&>();
        if (family == QuantityFamily::duration) {
            const auto parsed = parse_duration(text);
            if (!parsed) {
                add_diagnostic(path, family, parsed.error());
                return {};
            }
            value = json_object([&](Json& target) {
                set_json(target, "unit", "nanosecond");
                set_json(target, "value", parsed->value());
            });
            return {};
        }
        const auto parsed = parse_percentage(text);
        if (!parsed) {
            add_diagnostic(path, family, parsed.error());
            return {};
        }
        value = json_object([&](Json& target) {
            set_json(target, "unit", "ratio");
            set_json(target, "numerator", parsed->numerator());
            set_json(target, "denominator", parsed->denominator());
        });
        return {};
    }

    auto apply(Json& object, std::string_view field, const std::string& parent,
               QuantityFamily family) -> bool {
        const auto result = normalize_field(object, field, child_pointer(parent, field), family);
        if (!result) {
            internal_error_ = result.error();
            stopped_ = true;
            return false;
        }
        return true;
    }

    std::optional<core::Error> internal_error_;

    void normalize_defaults() {
        if (!normalized_.contains("defaults"))
            return;
        auto& defaults = normalized_.at("defaults");
        if (defaults.contains("memory")) {
            apply(defaults.at("memory"), "placement_threshold", "/defaults/memory",
                  QuantityFamily::percentage);
        }
        if (defaults.contains("observation")) {
            apply(defaults.at("observation"), "target_duration", "/defaults/observation",
                  QuantityFamily::duration);
        }
    }

    void normalize_experiments() {
        if (stopped_ && internal_error_)
            return;
        auto& experiments = normalized_.at("experiments");
        for (std::size_t experiment_index = 0; experiment_index < experiments.size() && !stopped_;
             ++experiment_index) {
            auto& experiment = experiments[experiment_index];
            const auto experiment_path = index_pointer("/experiments", experiment_index);
            const auto& type = experiment.at("type").get_ref<const std::string&>();
            if (type == "comparison") {
                auto& variants = experiment.at("variants");
                for (std::size_t variant_index = 0; variant_index < variants.size() && !stopped_;
                     ++variant_index) {
                    auto& memory = variants[variant_index].at("memory");
                    const auto memory_path = child_pointer(
                        index_pointer(child_pointer(experiment_path, "variants"), variant_index),
                        "memory");
                    apply(memory, "placement_threshold", memory_path, QuantityFamily::percentage);
                }
                auto& compare = experiment.at("compare");
                const auto compare_path = child_pointer(experiment_path, "compare");
                apply(compare, "practical_effect", compare_path, QuantityFamily::percentage);
                apply(compare, "confidence", compare_path, QuantityFamily::percentage);
            } else if (type == "node-pair-matrix") {
                if (experiment.contains("memory")) {
                    apply(experiment.at("memory"), "placement_threshold",
                          child_pointer(experiment_path, "memory"), QuantityFamily::percentage);
                }
                auto& comparison = experiment.at("comparison");
                const auto comparison_path = child_pointer(experiment_path, "comparison");
                apply(comparison, "practical_effect", comparison_path, QuantityFamily::percentage);
                apply(comparison, "confidence", comparison_path, QuantityFamily::percentage);
            } else {
                internal_error_ = make_units_internal_error("experiment_discriminator_mismatch");
                stopped_ = true;
            }
        }
    }

    void normalize_measurement() {
        if (stopped_ && internal_error_)
            return;
        if (!normalized_.contains("measurement"))
            return;
        auto& measurement = normalized_.at("measurement");
        if (measurement.contains("observation")) {
            auto& observation = measurement.at("observation");
            constexpr std::array duration_fields{"target_duration", "minimum_duration",
                                                 "maximum_duration"};
            for (const auto* field : duration_fields) {
                apply(observation, field, "/measurement/observation", QuantityFamily::duration);
            }
            apply(observation, "maximum_harness_overhead", "/measurement/observation",
                  QuantityFamily::percentage);
        }
        if (measurement.contains("warmup")) {
            apply(measurement.at("warmup"), "stability_threshold", "/measurement/warmup",
                  QuantityFamily::percentage);
        }
        if (measurement.contains("stopping")) {
            auto& stopping = measurement.at("stopping");
            apply(stopping, "confidence", "/measurement/stopping", QuantityFamily::percentage);
            apply(stopping, "ratio_interval_relative_half_width", "/measurement/stopping",
                  QuantityFamily::percentage);
        }
        if (measurement.contains("statistics")) {
            apply(measurement.at("statistics"), "practical_effect", "/measurement/statistics",
                  QuantityFamily::percentage);
        }
    }

    void normalize_monitoring() {
        if (stopped_ && internal_error_)
            return;
        if (!normalized_.contains("monitoring"))
            return;
        auto& monitoring = normalized_.at("monitoring");
        if (!monitoring.contains("sampling"))
            return;
        auto& sampling = monitoring.at("sampling");
        if (!sampling.contains("during_observation"))
            return;
        apply(sampling.at("during_observation"), "interval",
              "/monitoring/sampling/during_observation", QuantityFamily::duration);
    }

  public:
    auto finish() -> std::expected<std::vector<ScenarioDiagnostic>, core::Error> {
        if (internal_error_)
            return std::unexpected{std::move(*internal_error_)};
        return std::move(diagnostics_);
    }
};

} // namespace

auto scenario_v1_unit_bindings() noexcept -> std::span<const UnitBinding> { return bindings; }

auto normalize_v1_units(const StructurallyValidScenario& scenario, Json& normalized)
    -> std::expected<std::vector<ScenarioDiagnostic>, core::Error> {
    Normalizer normalizer{scenario, normalized};
    normalizer.run();
    return normalizer.finish();
}

} // namespace epycbench::scenario::detail
