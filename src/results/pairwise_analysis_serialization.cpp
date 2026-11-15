#include "results/pairwise_analysis_serialization.hpp"
#include "results/calculation_failure_identity.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace epycbench::results::analysis {
namespace {
using Json = nlohmann::json;
struct Rejection {
    std::string_view kind;
};
[[noreturn]] void reject(std::string_view kind = "invalid_result") { throw Rejection{kind}; }
void require(bool value) {
    if (!value)
        reject();
}
void finite(double value) {
    if (!std::isfinite(value))
        reject("nonfinite");
}
auto failure(std::string_view kind) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.analysis." + std::string{kind}},
            .message = "Pairwise analysis serialization rejected",
            .operation = "serialize_pairwise_analysis",
            .os_error = std::nullopt,
            .context = Json::object(),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
void retire(Json& value) noexcept {
    if (value.is_structured()) {
        for (auto& child : value)
            retire(child);
        value.clear();
    }
}
struct Guard {
    Json& value;
    ~Guard() { retire(value); }
};
// Every container is attached to the guarded root before receiving children.
// Temporaries are scalar or empty containers only, including attachment failure.
struct Budget {
    core::JcsLimits limits;
    std::size_t nodes{}, text{};
    void node(std::size_t depth) {
        if (depth > limits.depth || nodes == limits.nodes)
            reject("capacity");
        ++nodes;
    }
    void bytes(std::size_t count) {
        if (count > limits.output_bytes - text)
            reject("capacity");
        text += count;
    }
};
struct Node {
    Json& value;
    Budget& budget;
    std::size_t depth;
    auto child(std::string_view key) -> Node {
        budget.node(depth + 1);
        budget.bytes(key.size());
        auto [it, inserted] = value.emplace(std::string{key}, nullptr);
        require(inserted);
        return {*it, budget, depth + 1};
    }
    auto element() -> Node {
        budget.node(depth + 1);
        value.push_back(nullptr);
        return {value.back(), budget, depth + 1};
    }
    void object() { value = Json::object(); }
    void array() { value = Json::array(); }
    auto object(std::string_view key) -> Node {
        auto n = child(key);
        n.object();
        return n;
    }
    auto array(std::string_view key) -> Node {
        auto n = child(key);
        n.array();
        return n;
    }
    void text(std::string_view input) {
        budget.bytes(input.size());
        value = std::string{input};
    }
    void text(std::string_view key, std::string_view input) { child(key).text(input); }
    void number(double input) {
        finite(input);
        value = input;
    }
    void number(std::string_view key, double input) { child(key).number(input); }
    void boolean(std::string_view key, bool input) { child(key).value = input; }
    void null(std::string_view key) { (void)child(key); }
    void u(std::uint64_t input) {
        char buffer[20];
        const auto result = std::to_chars(buffer, buffer + 20, input);
        require(result.ec == std::errc{});
        text({buffer, static_cast<std::size_t>(result.ptr - buffer)});
    }
    void u(std::string_view key, std::uint64_t input) { child(key).u(input); }
    auto present(std::string_view key) -> Node {
        auto n = object(key);
        n.null("absent_reason");
        return n.child("value");
    }
    void absent(std::string_view key, std::string_view reason) {
        auto n = object(key);
        n.null("value");
        n.text("absent_reason", reason);
    }
};
template <class Enum, std::size_t N>
auto token(Enum input, const std::array<std::string_view, N>& names) -> std::string_view {
    const auto index = static_cast<std::underlying_type_t<Enum>>(input);
    if constexpr (std::is_signed_v<decltype(index)>)
        if (index < 0)
            reject();
    if (static_cast<std::uintmax_t>(index) >= N)
        reject();
    return names[static_cast<std::size_t>(index)];
}
using namespace std::literals;
auto validity(measurement::ObservationValidityDisposition v) -> std::string_view {
    return token(v, std::array{"valid"sv, "invalid"sv});
}
auto completion(runtime::BlockAttemptCompletion v) -> std::string_view {
    return token(
        v, std::array{"complete_valid"sv, "complete_invalid"sv, "operationally_incomplete"sv});
}
auto role(measurement::ComparisonVariantSlot v) -> std::string_view {
    return token(v, std::array{"baseline"sv, "candidate"sv});
}
auto decision(measurement::PrecisionStoppingDecision v) -> std::string_view {
    return token(v, std::array{"target_met"sv, "target_not_met"sv, "unavailable"sv});
}
void method(Node n, std::string_view name) {
    n.object();
    n.text("name", name);
    n.text("version", "1");
}
void policy(Node n, const statistics::BootstrapPolicy& p) {
    n.object();
    require(p.replicate_count().value() > 0);
    finite(p.confidence().value());
    require(p.confidence().value() > 0 && p.confidence().value() < 1);
    n.u("replicate_count", p.replicate_count().value());
    n.number("confidence", p.confidence().value());
    n.u("seed", p.seed().value());
}
void repetition(Node n, const measurement::RepetitionPolicy& p) {
    n.object();
    n.u("minimum_blocks", p.minimum_blocks());
    n.u("maximum_blocks", p.maximum_blocks());
    n.u("minimum_valid_observations_per_variant", p.minimum_valid_observations_per_variant());
    n.boolean("stopping_enabled", p.stopping_enabled());
}
void bootstrap(Node n, const statistics::BlockBootstrapResult& b) {
    n.object();
    method(n.child("method"), "block-bootstrap-percentile");
    policy(n.child("policy"), b.policy());
    auto log = n.object("log_interval");
    log.number("lower", b.log_effect_interval().lower().value());
    log.number("upper", b.log_effect_interval().upper().value());
    auto ratio = n.object("ratio_interval");
    ratio.number("lower", b.oriented_geometric_ratio_interval().lower().value());
    ratio.number("upper", b.oriented_geometric_ratio_interval().upper().value());
    require(b.log_effect_interval().lower().value() <= b.log_effect_interval().upper().value());
    require(b.oriented_geometric_ratio_interval().lower().value() > 0 &&
            b.oriented_geometric_ratio_interval().lower().value() <=
                b.oriented_geometric_ratio_interval().upper().value());
}
void policies(Node n, const PairwiseRunResult& result) {
    const auto& p = result.statistics_policy();
    auto stats = n.object("statistics");
    policy(stats.child("comparison_bootstrap"), p.comparison_bootstrap_policy());
    policy(stats.child("stopping_bootstrap"), p.stopping_bootstrap_policy());
    stats.number("effective_practical_effect_fraction", p.effective_practical_effect_fraction());
    stats.number("practical_effect_delta", p.practical_effect_threshold().delta());
    stats.number("relative_half_width_target", p.relative_half_width_target().value());
    require(p.effective_practical_effect_fraction() >= 0 &&
            p.practical_effect_threshold().delta() >= 0 &&
            p.relative_half_width_target().value() >= 0 &&
            p.relative_half_width_target().value() <= 1 &&
            p.effective_practical_effect_fraction() <= 1);
    repetition(n.child("repetition"), result.repetition_policy());
    auto order = n.object("comparison_order");
    order.text("strategy", token(result.comparison_order().strategy(),
                                 std::array{"randomized_abba"sv, "randomized_baab"sv,
                                            "randomized_balanced"sv}));
    order.u("seed", result.comparison_order().comparison_order_seed());
    const auto& m = result.monitoring_policy();
    auto monitor = n.object("monitoring");
    for (const auto [name, channel] : std::array{
             std::pair{"page_faults"sv, monitoring::MonitoringChannel::page_faults},
             std::pair{"context_switches"sv, monitoring::MonitoringChannel::context_switches},
             std::pair{"temperature"sv, monitoring::MonitoringChannel::temperature},
             std::pair{"frequency"sv, monitoring::MonitoringChannel::frequency}})
        monitor.text(name, token(m.disposition(channel),
                                 std::array{"disabled"sv, "optional"sv, "required"sv}));
    monitor.boolean("before_observation", m.before_observation());
    monitor.boolean("after_observation", m.after_observation());
    monitor.boolean("during_observation", m.during_observation());
    if (m.during_interval())
        monitor.present("during_interval_ns").u(m.during_interval()->value());
    else
        monitor.absent("during_interval_ns", "not_configured");
    monitor.text("optional_unsupported",
                 token(m.optional_unsupported(), std::array{"fail"sv, "warn"sv, "disable"sv}));
}
void provenance(Node n, const runtime::PairwiseRunProvenance& p) {
    n.u("experiment_id", p.experiment_id.value());
    n.text("scenario_profile", p.scenario_profile);
    n.text("scenario_digest", p.scenario_digest);
    n.text("resolved_plan_digest", p.resolved_plan_digest);
    n.text("workload_id", p.workload_id);
    n.text("first_variant_id", p.first_variant_id);
    n.text("second_variant_id", p.second_variant_id);
}
void terminal(Node n, const PairwiseRunResult& result) {
    if (const auto* normal = std::get_if<runtime::NormalRunTerminalEvidence>(&result.terminal())) {
        n.text("kind", "normal");
        auto o = n.present("normal_outcome");
        o.object();
        const auto& v = normal->outcome;
        require(v.experiment_id() == result.provenance().experiment_id &&
                v.policy() == result.repetition_policy());
        o.u("experiment_id", v.experiment_id().value());
        repetition(o.child("policy"), v.policy());
        o.text("disposition", token(v.disposition(), std::array{"precision_target_reached"sv,
                                                                "maximum_blocks_reached"sv,
                                                                "selected_work_unavailable"sv}));
        o.u("completed_blocks", v.completed_blocks());
        o.u("complete_valid_blocks", v.complete_valid_blocks());
        o.u("valid_first_variant_observations", v.valid_first_variant_observations());
        o.u("valid_second_variant_observations", v.valid_second_variant_observations());
        n.absent("active_block_id", "normal_terminal");
    } else {
        n.text("kind", "operational");
        n.absent("normal_outcome", "operational_terminal");
        const auto& o = std::get<runtime::OperationalRunTerminalEvidence>(result.terminal());
        if (o.active_block_id)
            n.present("active_block_id").u(o.active_block_id->value());
        else
            n.absent("active_block_id", "no_active_block");
    }
}
struct Counts {
    std::uint64_t completed{}, valid{}, first{}, second{};
};
void checked_increment(std::uint64_t& value) {
    if (value == UINT64_MAX)
        reject("capacity");
    ++value;
}
auto prefix(const PairwiseRunResult& result, std::uint64_t boundary) -> Counts {
    Counts counts;
    bool found = false;
    for (const auto& block : result.raw_blocks()) {
        if (block.plan.block_id().value() > boundary)
            break;
        if (block.plan.block_id().value() == boundary)
            found = true;
        if (block.completion != runtime::BlockAttemptCompletion::operationally_incomplete)
            checked_increment(counts.completed);
        if (block.completion == runtime::BlockAttemptCompletion::complete_valid) {
            checked_increment(counts.valid);
            for (const auto& observation : block.observations) {
                if (observation.slot.comparison_variant() ==
                    measurement::ComparisonVariantSlot::first)
                    checked_increment(counts.first);
                else
                    checked_increment(counts.second);
            }
        }
    }
    require(found && counts.valid > 0);
    return counts;
}
void counts(Node n, Counts c) {
    n.u("completed_blocks", c.completed);
    n.u("complete_valid_blocks", c.valid);
    n.u("valid_first_observations", c.first);
    n.u("valid_second_observations", c.second);
}
void metric(Node n, std::size_t family) {
    n.object();
    require(family < 2);
    n.text("family", family == 0 ? "bandwidth" : "dependent_load_latency");
    n.text("unit", family == 0 ? "bytes_per_second" : "seconds_per_completed_load");
    n.text("better", family == 0 ? "higher" : "lower");
    n.text("orientation", family == 0 ? "candidate_over_baseline" : "baseline_over_candidate");
}
struct Population {
    std::optional<std::size_t> family;
    std::size_t eligible{};
};
auto population(Node output, const PairwiseRunResult& result) -> Population {
    Population population;
    std::optional<std::uint64_t> previous_block;
    std::vector<std::uint64_t> identities;
    for (const auto& block : result.raw_blocks()) {
        const auto id = block.plan.block_id().value();
        require(!previous_block || *previous_block < id);
        previous_block = id;
        const auto complete = completion(block.completion);
        std::array<unsigned, 2> roles{};
        std::optional<std::uint8_t> previous_slot;
        if (block.completion == runtime::BlockAttemptCompletion::complete_valid)
            require(block.observations.size() == 4 && !block.failed_observation);
        const auto append = [&](const auto& slot, core::ObservationId observation_id,
                                std::optional<measurement::ObservationValidityDisposition> state) {
            require(slot.block_id() == block.plan.block_id() && slot.slot_index() < 4);
            require(slot == block.plan.observations()[slot.slot_index()]);
            require(!previous_slot || *previous_slot < slot.slot_index());
            previous_slot = slot.slot_index();
            if (identities.size() == output.budget.limits.nodes)
                reject("capacity");
            identities.push_back(observation_id.value());
            const auto variant = role(slot.comparison_variant());
            const auto r = variant == "baseline" ? 0U : 1U;
            if (state && block.completion == runtime::BlockAttemptCompletion::complete_valid) {
                require(*state == measurement::ObservationValidityDisposition::valid);
                ++roles[r];
            }
            auto n = output.element();
            n.object();
            n.u("block_id", id);
            n.u("observation_id", observation_id.value());
            n.text("role", variant);
            n.child("slot_index").value = slot.slot_index();
            n.text("block_completion", complete);
            if (state)
                n.present("observation_validity").text(validity(*state));
            else
                n.absent("observation_validity", "no_factual_observation");
            const bool eligible =
                state.has_value() &&
                block.completion == runtime::BlockAttemptCompletion::complete_valid;
            n.boolean("eligible", eligible);
            if (eligible) {
                ++population.eligible;
                n.absent("exclusion", "eligible_paired_population");
            } else {
                auto e = n.present("exclusion");
                e.object();
                e.boolean("automatic", true);
                if (!state) {
                    e.text("category", "operational_failure");
                    e.text("reason_code", "no_factual_observation");
                    e.text("authority", "runtime.failed_observation");
                } else if (*state == measurement::ObservationValidityDisposition::invalid) {
                    e.text("category", "measurement_validity");
                    e.text("reason_code", "observation_invalid");
                    e.text("authority", "measurement.validity");
                } else {
                    e.text("category", "paired_population");
                    e.text("reason_code",
                           block.completion == runtime::BlockAttemptCompletion::complete_invalid
                               ? "paired_block_invalid"
                               : "paired_block_incomplete");
                    e.text("authority", "runtime.block_completion");
                }
            }
        };
        for (const auto& observation : block.observations) {
            require(!population.family || *population.family == observation.observation.index());
            population.family = observation.observation.index();
            require(observation.validity.observation_id() == observation.observation_id);
            require(std::visit(
                [&](const auto& measured) {
                    return measured.observation_id() == observation.observation_id;
                },
                observation.observation));
            append(observation.slot, observation.observation_id,
                   observation.validity.disposition());
        }
        if (block.completion == runtime::BlockAttemptCompletion::complete_valid)
            require(roles[0] == 2 && roles[1] == 2);
        if (block.failed_observation)
            append(block.failed_observation->slot, block.failed_observation->observation_id,
                   std::nullopt);
    }
    std::ranges::sort(identities);
    require(std::adjacent_find(identities.begin(), identities.end()) == identities.end());
    return population;
}
template <class Metric> auto scalar(Metric metric) -> double {
    if constexpr (std::same_as<Metric, statistics::Bandwidth>)
        return metric.bytes_per_second();
    else
        return metric.seconds_per_completed_load();
}
template <class Metric> void variant(Node n, const VariantStatisticsEvidence<Metric>& v) {
    n.object();
    require(!v.observation_ids.empty() && v.observation_ids.size() == v.descriptive.count() &&
            v.outliers.flags().size() == v.observation_ids.size());
    auto ids = n.array("observation_ids");
    for (auto id : v.observation_ids)
        ids.element().u(id.value());
    const auto& d = v.descriptive;
    auto desc = n.object("descriptive");
    desc.u("count", d.count());
    desc.number("minimum", scalar(d.minimum()));
    desc.number("maximum", scalar(d.maximum()));
    desc.number("mean", scalar(d.mean()));
    desc.number("median", scalar(d.median()));
    desc.number("population_standard_deviation", scalar(d.population_standard_deviation()));
    desc.number("coefficient_of_variation", d.coefficient_of_variation());
    desc.number("percentile_5", scalar(d.percentile_5()));
    desc.number("percentile_95", scalar(d.percentile_95()));
    require(scalar(d.minimum()) > 0 && scalar(d.minimum()) <= scalar(d.maximum()) &&
            scalar(d.mean()) > 0 && scalar(d.median()) > 0 &&
            scalar(d.population_standard_deviation()) >= 0 && d.coefficient_of_variation() >= 0 &&
            scalar(d.percentile_5()) > 0 && scalar(d.percentile_5()) <= scalar(d.percentile_95()));
    const auto& o = v.outliers;
    auto out = n.object("outliers");
    method(out.child("method"), token(o.method(), std::array{"median_absolute_deviation"sv}));
    out.number("median", scalar(o.median()));
    out.number("median_absolute_deviation", scalar(o.median_absolute_deviation()));
    require(scalar(o.median()) > 0 && scalar(o.median_absolute_deviation()) >= 0);
    auto flags = out.array("flags");
    std::uint64_t flagged{};
    for (auto flag : o.flags()) {
        flags.element().text(
            token(flag, std::array{"not_flagged"sv, "flagged_low"sv, "flagged_high"sv}));
        if (flag != statistics::OutlierFlagState::not_flagged)
            checked_increment(flagged);
    }
    out.u("flagged_count", flagged);
}
template <class Metric>
void statistics_value(Node n, const AuthoritativePairwiseStatistics<Metric>& s,
                      std::span<const runtime::PairwiseBlockAttemptEvidence> blocks_input,
                      const runtime::StatisticsProductionPolicy& statistics_policy) {
    n.object();
    require(s.block_ids.size() == s.ordered_effects.size() && !s.block_ids.empty());
    std::size_t blocks{}, first{}, second{};
    for (const auto& block : blocks_input) {
        if (block.completion != runtime::BlockAttemptCompletion::complete_valid)
            continue;
        require(blocks < s.block_ids.size() && s.block_ids[blocks++] == block.plan.block_id());
        for (const auto& observation : block.observations) {
            if (observation.slot.comparison_variant() ==
                measurement::ComparisonVariantSlot::first) {
                require(first < s.first.observation_ids.size() &&
                        s.first.observation_ids[first++] == observation.observation_id);
            } else {
                require(second < s.second.observation_ids.size() &&
                        s.second.observation_ids[second++] == observation.observation_id);
            }
        }
    }
    require(blocks == s.block_ids.size() && first == s.first.observation_ids.size() &&
            second == s.second.observation_ids.size());
    variant(n.child("first"), s.first);
    variant(n.child("second"), s.second);
    method(n.child("comparison_method"), "paired-log-ratio");
    auto definition = n.object("descriptive_definition");
    definition.text("dispersion", "population");
    definition.text("quantile", "type_7");
    definition.text("coefficient_of_variation", "dimensionless_ratio");
    auto ids = n.array("block_ids");
    for (auto id : s.block_ids)
        ids.element().u(id.value());
    auto effects = n.array("ordered_effects");
    for (auto effect : s.ordered_effects)
        effects.element().number(effect.value());
    auto comparison = n.object("comparison");
    comparison.number("mean_log_effect", s.comparison.mean_log_effect().value());
    const auto point = s.comparison.oriented_geometric_ratio().value();
    comparison.number("oriented_geometric_ratio", point);
    require(point > 0);
    require(s.confidence_interval.policy() ==
            statistics_policy.comparison_bootstrap_policy());
    bootstrap(n.child("confidence_interval"), s.confidence_interval);
    n.text("conclusion", token(s.conclusion, std::array{"candidate_faster"sv, "baseline_faster"sv,
                                                        "detectable_below_practical_threshold"sv,
                                                        "inconclusive"sv}));
    auto presentation = n.object("presentation");
    const auto percentage = (point - 1.0) * 100.0;
    if (std::isfinite(percentage))
        presentation.present("percentage_change").number(percentage);
    else
        presentation.absent("percentage_change", "numeric_overflow");
    auto precision = statistics::evaluate_adaptive_stopping(
        s.comparison.oriented_geometric_ratio(),
        s.confidence_interval.oriented_geometric_ratio_interval(),
        statistics_policy.relative_half_width_target());
    if (precision)
        presentation.present("relative_half_width").number(precision->observed_precision().value());
    else if (precision.error().code.value() == "statistics.stopping.numeric_failure")
        presentation.absent("relative_half_width", "numeric_failure");
    else
        reject();
    presentation.boolean("excludes_equality",
                         s.confidence_interval.log_effect_interval().lower().value() > 0 ||
                             s.confidence_interval.log_effect_interval().upper().value() < 0);
}
void validate_failure(FinalCalculationFailure value, bool historical) {
    require(static_cast<unsigned>(value.stage) < 12 && static_cast<unsigned>(value.identity) < 7);
    const auto stage = static_cast<unsigned>(value.stage);
    require(historical ? (stage < 3 || stage >= 9) : stage < 9);
    require(value.identity == detail::calculation_identity(value.stage));
}
void histories(Node replay_array, Node diagnostics, const PairwiseRunResult& result) {
    std::optional<std::uint64_t> previous;
    for (const auto& replay : result.stopping_replays()) {
        const auto id = replay.boundary_block_id.value();
        require(!previous || *previous < id);
        previous = id;
        require(replay.bootstrap.policy() ==
                result.statistics_policy().stopping_bootstrap_policy());
        auto n = replay_array.element();
        n.object();
        n.u("boundary_block_id", id);
        counts(n.object("counts"), prefix(result, id));
        bootstrap(n.child("bootstrap"), replay.bootstrap);
        auto e = n.object("evaluation");
        e.number("observed_relative_half_width", replay.evaluation.observed_precision().value());
        require(replay.evaluation.observed_precision().value() >= 0);
        e.text("attainment", token(replay.evaluation.attainment(),
                                   std::array{"target_met"sv, "target_not_met"sv}));
        e.number("target", result.statistics_policy().relative_half_width_target().value());
        n.text("decision", decision(replay.decision));
        n.boolean("exact_agreement", replay.exact_agreement);
    }
    previous.reset();
    for (const auto& diagnostic : result.diagnostics()) {
        const auto id = diagnostic.boundary_block_id.value();
        require(!previous || *previous < id);
        previous = id;
        const auto replay = std::ranges::find_if(result.stopping_replays(), [&](const auto& v) {
            return v.boundary_block_id == diagnostic.boundary_block_id;
        });
        if (diagnostic.code == "results.recalculation_mismatch") {
            require(diagnostic.stage == "stopping_replay" &&
                    diagnostic.field == "exact_typed_evidence" &&
                    replay != result.stopping_replays().end() && !replay->exact_agreement);
        } else {
            bool matched = false;
            for (unsigned i = 0; i < 12; ++i) {
                if (i >= 3 && i < 9)
                    continue;
                const auto stage = static_cast<CalculationStage>(i);
                const FinalCalculationFailure fact{stage, detail::calculation_identity(stage)};
                if (diagnostic.code == fact.stable_code() && diagnostic.stage == fact.stage_name())
                    matched = true;
            }
            require(matched && diagnostic.field == "calculation_unavailable" &&
                    replay == result.stopping_replays().end() && !diagnostic.runner_value &&
                    !diagnostic.controller_value);
        }
        auto n = diagnostics.element();
        n.object();
        n.text("code", diagnostic.code);
        n.u("boundary_block_id", id);
        n.text("stage", diagnostic.stage);
        n.text("field", diagnostic.field);
        counts(n.object("counts"), prefix(result, id));
        if (diagnostic.runner_value)
            n.present("runner_value").number(*diagnostic.runner_value);
        else
            n.absent("runner_value", "not_retained");
        if (diagnostic.controller_value)
            n.present("controller_value").number(*diagnostic.controller_value);
        else
            n.absent("controller_value", "not_retained");
    }
    for (const auto& replay : result.stopping_replays())
        if (!replay.exact_agreement)
            require(std::ranges::any_of(result.diagnostics(), [&](const auto& d) {
                return d.boundary_block_id == replay.boundary_block_id &&
                       d.code == "results.recalculation_mismatch";
            }));
}
void project(Node root, const PairwiseRunResult& result) {
    root.object();
    root.text("profile", "epycbench.pairwise-derived-analysis/v1");
    root.text("scope", "derived_analysis");
    provenance(root.object("provenance"), result.provenance());
    policies(root.object("policies"), result);
    terminal(root.object("terminal"), result);
    root.text("validity",
              token(result.validity(), std::array{"valid"sv, "invalid"sv, "not_evaluated"sv}));
    const auto input = population(root.array("population"), result);
    const bool normal =
        std::holds_alternative<runtime::NormalRunTerminalEvidence>(result.terminal());
    const bool final = result.authoritative_statistics().has_value();
    const auto failure = result.final_calculation_failure();
    require(!(final && failure) && (!final || normal));
    require(!normal || input.eligible == 0 || final || failure);
    require(result.validity() != PairwiseResultValidity::valid ||
            (normal && final && !failure && result.diagnostics().empty()));
    require(result.validity() != PairwiseResultValidity::not_evaluated ||
            (!final && !failure && result.diagnostics().empty()));
    require((!failure && result.diagnostics().empty()) ||
            result.validity() == PairwiseResultValidity::invalid);
    if (final) {
        require(input.family && *input.family == result.authoritative_statistics()->index());
        metric(root.present("metric"), *input.family);
        std::visit([&](const auto& s) { statistics_value(root.present("analysis"), s, result.raw_blocks(), result.statistics_policy()); },
                   *result.authoritative_statistics());
    } else {
        if (input.family)
            metric(root.present("metric"), *input.family);
        else
            root.absent("metric", "no_measurement_family");
        root.absent("analysis", failure  ? "numerical_failure"
                                : normal ? "no_complete_valid_blocks"
                                         : "operational_terminal");
    }
    if (failure) {
        validate_failure(*failure, false);
        auto n = root.present("final_calculation_failure");
        n.object();
        n.text("stage", failure->stage_name());
        n.text("code", failure->stable_code());
    } else
        root.absent("final_calculation_failure", "no_final_calculation_failure");
    histories(root.array("stopping_replays"), root.array("diagnostics"), result);
}
} // namespace

auto serialize_diagnostic_statistics(const PairwiseStatistics& input,
    std::span<const runtime::PairwiseBlockAttemptEvidence> blocks,
    const runtime::StatisticsProductionPolicy& policy_input, core::JcsLimits limits)
    -> std::expected<std::string, core::Error> {
    try {
        if (limits.depth > 64 || limits.output_bytes > std::string{}.max_size())
            return std::unexpected{failure("invalid_limits")};
        Budget budget{limits};
        budget.node(1);
        Json tree;
        Guard guard{tree};
        Node root{tree, budget, 1};
        root.object();
        root.text("profile", "epycbench.pairwise-diagnostic-statistics/v1");
        root.text("scope", "diagnostic_partial");
        root.text("conclusion", "not_evaluated");
        std::visit([&](const auto& value) {
            statistics_value(root.child("diagnostic"), value, blocks, policy_input);
        }, input);
        return core::canonicalize_json_jcs(tree, limits);
    } catch (const Rejection& error) {
        return std::unexpected{failure(error.kind)};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::length_error&) {
        return std::unexpected{failure("resource_failure")};
    } catch (...) {
        return std::unexpected{failure("internal_failure")};
    }
}

auto serialize_pairwise_analysis(const PairwiseRunResult& result, core::JcsLimits limits)
    -> std::expected<std::string, core::Error> {
    try {
        if (limits.depth > 64 || limits.output_bytes > std::string{}.max_size())
            return std::unexpected{failure("invalid_limits")};
        if (limits.depth == 0 || limits.nodes == 0 || limits.output_bytes == 0)
            return std::unexpected{failure("capacity")};
        Budget budget{limits};
        budget.node(1);
        Json tree;
        Guard guard{tree};
        project({tree, budget, 1}, result);
        auto output = core::canonicalize_json_jcs(tree, limits);
        if (output)
            return output;
        const auto& code = output.error().code.value();
        if (code.find("utf8") != std::string::npos || code.find("unicode") != std::string::npos)
            return std::unexpected{failure("invalid_utf8")};
        if (code.find("limit") != std::string::npos || code.find("capacity") != std::string::npos)
            return std::unexpected{failure("capacity")};
        if (code.find("nonfinite") != std::string::npos)
            return std::unexpected{failure("nonfinite")};
        return std::unexpected{failure("internal_failure")};
    } catch (const Rejection& error) {
        return std::unexpected{failure(error.kind)};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::length_error&) {
        return std::unexpected{failure("resource_failure")};
    } catch (...) {
        return std::unexpected{failure("internal_failure")};
    }
}
} // namespace epycbench::results::analysis
