#include "results/journal_snapshot.hpp"
#include "results/expected_pair_evidence_context.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "runtime/finalized_evidence_wire_typed_reconstruction.hpp"

#include <limits>
#include <utility>

namespace epycbench::results::snapshot {
namespace {
using Json = nlohmann::json;
namespace wire = runtime::wire_v1;
constexpr auto profile = "epycbench.finalized-pairwise-journal-snapshot/v1";
auto error(std::string_view kind) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.snapshot." + std::string{kind}},
            .message = "Snapshot rejected",
            .operation = "journal_snapshot",
            .os_error = std::nullopt,
            .context = Json::object(),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
// Only pre-bounded, exclusively owned trees are retired here.
void retire(Json& value) noexcept {
    if (value.is_structured()) {
        for (auto& child : value) {
            retire(child);
        }
        value.clear();
    }
}
struct Guard {
    Json& value;
    ~Guard() { retire(value); }
};
auto journal_error(const core::Error& source) -> core::Error {
    const auto& code = source.code.value();
    if (code == "results.journal.capacity")
        return error("capacity");
    if (code == "results.journal.identity")
        return error("identity");
    if (code == "results.journal.sequence")
        return error("sequence");
    if (code == "results.journal.chain" || code == "results.journal.digest")
        return error("digest");
    return error("framing");
}
auto codec_error(const wire::LogicalCodecError& source) -> core::Error {
    using enum wire::LogicalCodecErrorCode;
    return error(source.code == byte_limit || source.code == node_limit ||
                         source.code == depth_limit
                     ? "capacity"
                     : "reconstruction");
}
auto provenance_json(const runtime::PairwiseRunProvenance& p) -> Json {
    Json value = Json::object();
    Guard guard{value};
    value["experiment_id"] = std::to_string(p.experiment_id.value());
    value["scenario_profile"] = p.scenario_profile;
    value["scenario_digest"] = p.scenario_digest;
    value["resolved_plan_digest"] = p.resolved_plan_digest;
    value["workload_id"] = p.workload_id;
    value["first_variant_id"] = p.first_variant_id;
    value["second_variant_id"] = p.second_variant_id;
    return Json(std::move(value));
}
auto receipt(const PairwiseRunResult& result, std::string_view record_digest) -> Json {
    const auto validity = result.validity() == PairwiseResultValidity::valid     ? "valid"
                          : result.validity() == PairwiseResultValidity::invalid ? "invalid"
                                                                                 : "not_evaluated";
    Json value = Json::object();
    Guard guard{value};
    value["profile"] = profile;
    value["evidence_record_digest"] = record_digest;
    value["projection_profile"] = "epycbench.pairwise-results-replay-receipt/v1";
    value["terminal_kind"] =
        std::holds_alternative<runtime::NormalRunTerminalEvidence>(result.terminal())
            ? "normal"
            : "operational";
    value["validity"] = validity;
    value["authoritative_statistics_present"] = result.authoritative_statistics().has_value();
    value["stopping_replay_count"] = std::to_string(result.stopping_replays().size());
    value["diagnostic_count"] = std::to_string(result.diagnostics().size());
    return Json(std::move(value));
}
} // namespace
struct Context::State {
    State(journal::RunId supplied_run, Limits supplied_limits,
          block_evidence::ExpectedPairContext supplied_expected)
        : run{std::move(supplied_run)}, limits{supplied_limits}, expected{std::move(supplied_expected)},
          creation(Json::object()), provenance(Json::object()) {}
    journal::RunId run;
    Limits limits;
    block_evidence::ExpectedPairContext expected;
    Json creation;
    Json provenance;
    ~State() { retire(creation); retire(provenance); }
};
Context::Context(std::unique_ptr<State> state) : state_{std::move(state)} {}
Context::Context(Context&&) noexcept = default;
Context::~Context() = default;
auto required_retained_bytes(std::size_t s, std::size_t p, std::size_t l, std::size_t b) noexcept
    -> std::optional<std::size_t> {
    // Six artifact-text slots, eight logical-body slots, twelve outer body/frame
    // slots; framing allowance covers three RS/LF pairs in each outer slot.
    std::size_t total = 72;
    for (auto [size, copies] :
         std::array<std::pair<std::size_t, std::size_t>, 4>{{{s, 6}, {p, 6}, {l, 8}, {b, 12}}}) {
        if (size > (std::numeric_limits<std::size_t>::max() - total) / copies) {
            return std::nullopt;
        }
        total += size * copies;
    }
    return total;
}
auto Context::prepare(journal::RunId run, const scenario::ScenarioDigestedPlan& plan,
                      const runtime::AdmittedPairwiseProductionRequest& pair,
                      const wire::CalculatedEvidenceCapacity& capacity, Limits limits)
    -> std::expected<Context, core::Error> {
    if (capacity.resource_plan.controller_receive_bytes.value > SIZE_MAX) {
        return std::unexpected{error("capacity")};
    }
    auto need = required_retained_bytes(
        plan.canonical_scenario_json().size(), plan.canonical_resolved_scenario_plan_json().size(),
        static_cast<std::size_t>(capacity.resource_plan.controller_receive_bytes.value),
        limits.outer.body_bytes);
    if (!need || *need > limits.retained_bytes || limits.outer.records < 3 ||
        limits.outer.depth > 64 || limits.outer.body_bytes == 0 || limits.outer.nodes == 0 ||
        limits.outer.depth == 0) {
        return std::unexpected{error("capacity")};
    }
    auto validated_run = journal::RunId::parse(run.text());
    if (!validated_run) {
        return std::unexpected{error("identity")};
    }
    // The shared sealed helper performs the real Scenario/request association.
    // Existing snapshot outer/text limits remain its public resource policy;
    // the helper separately checks every arithmetic/shape bound before allocating.
    auto expected = block_evidence::ExpectedPairContext::prepare(run, plan, pair, capacity,
        {plan.canonical_scenario_json().size(), plan.canonical_resolved_scenario_plan_json().size(),
         SIZE_MAX, SIZE_MAX, 64U, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX});
    if (!expected) return std::unexpected{error(expected.error().code == block_evidence::ErrorCode::capacity
                                               ? "capacity" : "identity")};
    auto state = std::make_unique<State>(std::move(run), limits, std::move(*expected));
    state->provenance = provenance_json(state->expected.provenance());
    Json cpus = Json::array();
    Guard cpu_guard{cpus};
    for (const auto cpu : state->expected.cpus()) cpus.push_back(cpu.value());
    auto& creation = state->creation;
    creation["profile"] = profile;
    creation["canonical_scenario"] = plan.canonical_scenario_json();
    creation["scenario_digest"] = plan.scenario_digest().text();
    creation["canonical_resolved_plan"] = plan.canonical_resolved_scenario_plan_json();
    creation["resolved_plan_digest"] = plan.resolved_scenario_plan_digest().text();
    creation["provenance"] = state->provenance;
    creation["worker_cpu_ids"] = std::move(cpus);
    return Context{std::move(state)};
}
namespace {
auto reconstruct(std::string_view body, const Context::State& context)
    -> std::expected<runtime::FinalizedPairwiseRunEvidence, core::Error> {
    auto logical = wire::decode_evidence_body(body, context.expected.capacity());
    if (!logical) {
        return std::unexpected{codec_error(logical.error())};
    }
    if (logical->value().at("provenance") != context.provenance) {
        return std::unexpected{error("identity")};
    }
    auto evidence = wire::detail::reconstruct_finalized_evidence(*logical, context.expected.cpus());
    if (!evidence) {
        return std::unexpected{error("reconstruction")};
    }
    auto reencoded = wire::detail::encode_finalized_evidence(*evidence, context.expected.capacity());
    if (!reencoded || *reencoded != body) {
        return std::unexpected{error("reconstruction")};
    }
    return std::move(*evidence);
}
auto projection(const runtime::FinalizedPairwiseRunEvidence& evidence)
    -> std::expected<PairwiseRunResult, core::Error> {
    auto result = project_pairwise_run_result(evidence);
    if (!result) {
        return std::unexpected{error("projection")};
    }
    return std::move(*result);
}
} // namespace
Capture::Capture(Context context) : context_{std::move(context)} {}
Capture::Capture(Capture&&) noexcept = default;
Capture::~Capture() = default;
auto Capture::finish(const runtime::FinalizedPairwiseRunEvidence& evidence,
                     const std::array<journal::Timestamp, 3>& times)
    -> std::expected<std::array<std::string, 3>, core::Error> {
    auto context = std::move(context_.state_);
    if (!context) {
        return std::unexpected{error("lifecycle")};
    }
    auto supplied_provenance = provenance_json(evidence.provenance());
    Guard pg{supplied_provenance};
    if (supplied_provenance != context->provenance) {
        return std::unexpected{error("identity")};
    }
    auto body = wire::detail::encode_finalized_evidence(evidence, context->expected.capacity());
    if (!body) {
        return std::unexpected{codec_error(body.error())};
    }
    auto reconstructed = reconstruct(*body, *context);
    if (!reconstructed) {
        return std::unexpected{reconstructed.error()};
    }
    auto result = projection(*reconstructed);
    if (!result) {
        return std::unexpected{result.error()};
    }
    auto writer = journal::Writer::create(std::move(context->run), context->limits.outer);
    if (!writer) {
        return std::unexpected{error("framing")};
    }
    std::array<std::string, 3> frames;
    auto first = writer->append({times[0], "creation", "results", context->creation});
    if (!first) {
        return std::unexpected{journal_error(first.error())};
    }
    frames[0] = std::move(*first);
    Json finalized = Json::object();
    Guard fg{finalized};
    finalized["profile"] = profile;
    finalized["evidence_profile"] = wire::wire_profile;
    finalized["evidence_digest"] = core::sha256(*body).to_hex();
    finalized["canonical_evidence"] = std::move(*body);
    auto second = writer->append({times[1], "finalization", "results", finalized});
    if (!second) {
        return std::unexpected{journal_error(second.error())};
    }
    frames[1] = std::move(*second);
    auto frame_object = Json::parse(std::string_view{frames[1]}.substr(1, frames[1].size() - 2));
    Guard sg{frame_object};
    auto analysis = receipt(*result, frame_object.at("digest").get_ref<const std::string&>());
    Guard ag{analysis};
    auto third = writer->append({times[2], "analysis", "results", analysis});
    if (!third) {
        return std::unexpected{journal_error(third.error())};
    }
    frames[2] = std::move(*third);
    if (!writer->finish()) {
        return std::unexpected{error("framing")};
    }
    return frames;
}
struct Replay::State {
    State(std::unique_ptr<Context::State> supplied, journal::Reader input)
        : context{std::move(supplied)}, reader{std::move(input)} {}
    std::unique_ptr<Context::State> context;
    journal::Reader reader;
    std::array<std::optional<journal::Record>, 3> prefix{};
    std::optional<PairwiseRunResult> result;
    std::size_t count{};
    bool live{true};
    ~State() {
        for (auto& record : prefix) {
            if (record) {
                retire(record->envelope);
            }
        }
    }
};
Replay::Replay(std::unique_ptr<State> state) : state_{std::move(state)} {}
Replay::Replay(Replay&&) noexcept = default;
Replay::~Replay() = default;
auto Replay::create(Context context) -> std::expected<Replay, core::Error> {
    if (!context.state_) {
        return std::unexpected{error("lifecycle")};
    }
    auto reader = journal::Reader::create(context.state_->run, context.state_->limits.outer);
    if (!reader) {
        return std::unexpected{error("capacity")};
    }
    return Replay{std::make_unique<State>(std::move(context.state_), std::move(*reader))};
}
auto Replay::feed(std::string_view bytes) -> std::expected<std::size_t, core::Error> {
    if (!state_ || !std::exchange(state_->live, false)) {
        return std::unexpected{error("lifecycle")};
    }
    if (state_->count == 3 && !bytes.empty()) {
        return std::unexpected{error("sequence")};
    }
    auto fed = state_->reader.feed(bytes);
    if (!fed) {
        return std::unexpected{journal_error(fed.error())};
    }
    if (fed->record) {
        auto& record = *fed->record;
        Guard guard{record.envelope};
        auto& value = record.envelope;
        constexpr std::array types{"creation", "finalization", "analysis"};
        if (state_->count >= 3 || value.at("type") != types[state_->count] ||
            value.at("producer") != "results") {
            return std::unexpected{error("sequence")};
        }
        const auto& payload = value.at("payload");
        if (state_->count == 0) {
            if (payload != state_->context->creation) {
                return std::unexpected{error("identity")};
            }
        } else if (state_->count == 1) {
            if (payload.size() != 4 || !payload.contains("profile") ||
                payload["profile"] != profile || !payload.contains("evidence_profile") ||
                payload["evidence_profile"] != wire::wire_profile ||
                !payload.contains("canonical_evidence") ||
                !payload["canonical_evidence"].is_string() ||
                !payload.contains("evidence_digest") || !payload["evidence_digest"].is_string()) {
                return std::unexpected{error("schema")};
            }
            const auto& body = payload["canonical_evidence"].get_ref<const std::string&>();
            if (core::sha256(body).to_hex() !=
                payload["evidence_digest"].get_ref<const std::string&>()) {
                return std::unexpected{error("digest")};
            }
            auto evidence = reconstruct(body, *state_->context);
            if (!evidence) {
                return std::unexpected{evidence.error()};
            }
            auto result = projection(*evidence);
            if (!result) {
                return std::unexpected{result.error()};
            }
            state_->result.emplace(std::move(*result));
        } else {
            auto expected =
                receipt(*state_->result,
                        state_->prefix[1]->envelope["digest"].get_ref<const std::string&>());
            Guard expected_guard{expected};
            if (payload != expected) {
                return std::unexpected{error("receipt")};
            }
        }
        state_->prefix[state_->count++].emplace(std::move(record));
        if (state_->count == 3 && fed->consumed != bytes.size()) {
            return std::unexpected{error("sequence")};
        }
    }
    state_->live = true;
    return fed->consumed;
}
auto Replay::finish() -> std::expected<Outcome, core::Error> {
    if (!state_ || !std::exchange(state_->live, false)) {
        return std::unexpected{error("lifecycle")};
    }
    auto framing = state_->reader.finish();
    if (!framing) {
        return std::unexpected{error("framing")};
    }
    const bool complete = state_->count == 3 && framing->tail == journal::Tail::clean_prefix;
    Outcome outcome{complete ? Completion::complete_snapshot : Completion::incomplete_snapshot,
                    *framing, std::move(state_->prefix), std::nullopt};
    if (complete) {
        outcome.result.emplace(std::move(*state_->result));
    }
    return outcome;
}
} // namespace epycbench::results::snapshot
