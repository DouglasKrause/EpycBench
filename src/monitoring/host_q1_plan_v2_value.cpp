#include "epycbench/core/canonical_json.hpp"
#include "monitoring/host_q1_plan_v2_value_test_access.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>
#include <utility>
namespace epycbench::monitoring::qualification {
namespace {
class BoundedPlanSax final : public nlohmann::json_sax<nlohmann::json> {
  public:
    bool null() override { return count_value(); }
    bool boolean(bool) override { return count_value(); }
    bool number_integer(number_integer_t) override { return count_value(); }
    bool number_unsigned(number_unsigned_t) override { return count_value(); }
    bool number_float(number_float_t, const string_t&) override { return count_value(); }
    bool string(string_t&) override { return count_value(); }
    bool binary(binary_t&) override {
        invalid_ = true;
        return false;
    }
    bool start_object(std::size_t) override { return start(false); }
    bool key(string_t&) override {
        if (depth_ == 0 || ++counts_[depth_ - 1U] > 10U) {
            invalid_ = true;
            return false;
        }
        return true;
    }
    bool end_object() override {
        --depth_;
        return true;
    }
    bool start_array(std::size_t) override { return start(true); }
    bool end_array() override {
        --depth_;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        invalid_ = true;
        return false;
    }
    [[nodiscard]] bool invalid() const noexcept { return invalid_; }

  private:
    bool start(bool array) {
        if (!count_value())
            return false;
        if (depth_ >= 3U) {
            invalid_ = true;
            return false;
        }
        counts_[depth_] = 0;
        arrays_[depth_] = array;
        ++depth_;
        return true;
    }
    bool count_value() {
        if (depth_ != 0U && arrays_[depth_ - 1U] && ++counts_[depth_ - 1U] > 4294967296ULL) {
            invalid_ = true;
            return false;
        }
        return true;
    }
    std::array<std::uint64_t, 3> counts_{};
    std::array<bool, 3> arrays_{};
    std::size_t depth_{};
    bool invalid_{};
};
auto error(std::string why) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q1_plan_value.invalid"},
            .message = "Monitoring Q1 plan-v2 parsed value is invalid",
            .operation = "validate_monitoring_q1_plan_value",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(why)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto qid(std::string_view value) noexcept -> bool {
    constexpr std::string_view prefix = "MON-007.host-v2.";
    if (!value.starts_with(prefix))
        return false;
    const auto suffix = value.substr(prefix.size());
    if (suffix.empty() || suffix.size() > 64 || suffix.front() == '-' || suffix.back() == '-')
        return false;
    return std::ranges::all_of(suffix, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}
auto digest(std::string_view value, bool prefix) -> std::expected<core::Sha256Digest, core::Error> {
    if (prefix) {
        if (!value.starts_with("sha256:"))
            return std::unexpected{error("digest")};
        value.remove_prefix(7);
    }
    auto parsed = core::Sha256Digest::parse(value);
    if (!parsed)
        return std::unexpected{error("digest")};
    return parsed;
}
auto canonical_size(const nlohmann::json& value) -> std::expected<std::uint64_t, core::Error> {
    auto bytes = core::canonicalize_json(value);
    if (!bytes)
        return std::unexpected{error("capacity_canonicalization")};
    if (bytes->size() == std::numeric_limits<std::size_t>::max())
        return std::unexpected{error("capacity_overflow")};
    return static_cast<std::uint64_t>(bytes->size() + 1U);
}
auto successor_tree_limit_value() -> std::expected<std::uint64_t, core::Error> {
    const std::array<std::string_view, 3> paths{"host-environment.json", "q0b/evidence.json",
                                                "q1/evidence.json"};
    nlohmann::json artifacts = nlohmann::json::array();
    for (auto path : paths)
        artifacts.push_back({{"path", path}, {"sha256", std::string(64, 'f')}});
    const nlohmann::json manifest = {
        {"schema", "epycbench.monitoring-qualification-manifest/v3"},
        {"qualification_id", "MON-007.host-v2." + std::string(64, 'z')},
        {"tool",
         {{"identity", "epycbench.monitoring-qualification-tool/v1"}, {"version", "1.0.0"}}},
        {"profile",
         {{"identity", "epycbench.monitoring-qualification.host-generic/v2"},
          {"stage", "final_frozen_profile"},
          {"sha256", std::string(64, 'f')}}},
        {"environment_digest", "sha256:" + std::string(64, 'f')},
        {"artifacts", artifacts}};
    auto manifest_size = canonical_size(manifest);
    if (!manifest_size)
        return std::unexpected{std::move(manifest_size.error())};
    std::uint64_t checksums = 64U + 2U + 13U + 1U;
    for (auto path : paths)
        checksums += 64U + 2U + path.size() + 1U;
    std::uint64_t inventory = 0;
    for (auto entry : {"q0b", "q1", "host-environment.json", "q0b/evidence.json",
                       "q1/evidence.json", "manifest.json", "checksums.sha256"})
        inventory += std::string_view{entry}.size() + 1U;
    return 1048576ULL + 8388608ULL + 8388608ULL + *manifest_size + checksums + inventory;
}
auto model_byte_sizes(const MonitoringQ1PlanV2ParsedCandidate& plan, std::uint64_t n,
                      std::string_view commit)
    -> std::expected<std::array<std::uint64_t, 2>, core::Error> {
    constexpr auto u = std::numeric_limits<std::uint64_t>::max();
    constexpr auto u32 = std::numeric_limits<std::uint32_t>::max();
    const nlohmann::json counter = {{"major_faults", u}, {"minor_faults", u}};
    nlohmann::json observations = nlohmann::json::array();
    if (n != 0U && plan.workers.size() > std::numeric_limits<std::size_t>::max() / n)
        return std::unexpected{error("capacity_overflow")};
    observations.get_ref<nlohmann::json::array_t&>().reserve(static_cast<std::size_t>(n) *
                                                             plan.workers.size());
    for (std::uint64_t cycle = 0; cycle < n; ++cycle)
        for ([[maybe_unused]] auto worker : plan.workers)
            observations.push_back({{"cycle_index", u},
                                    {"worker_id", u32},
                                    {"collection_start_ns", u},
                                    {"collection_end_ns", u},
                                    {"elapsed_ns", u},
                                    {"oracle_before", counter},
                                    {"production", counter},
                                    {"oracle_after", counter},
                                    {"minor_equal", true},
                                    {"major_equal", true}});
    const auto summary = [u, u32](bool worker) {
        nlohmann::json result = {{"count", u},
                                 {"minimum_elapsed_ns", u},
                                 {"maximum_elapsed_ns", u},
                                 {"lower_median_elapsed_ns", u},
                                 {"minor_equality_count", u},
                                 {"major_equality_count", u}};
        if (worker)
            result["worker_id"] = u32;
        return result;
    };
    nlohmann::json workers = nlohmann::json::array(), summaries = nlohmann::json::array();
    for ([[maybe_unused]] auto worker : plan.workers) {
        workers.push_back({{"worker_id", u32}, {"observation_count", u}});
        summaries.push_back(summary(true));
    }
    const nlohmann::json derivation = {{"algorithm", MonitoringQ1PlanV2Value::algorithm},
                                       {"mode", "governed_override"},
                                       {"target_total_observations", 4096},
                                       {"worker_count", u},
                                       {"conservative_observation_capacity_per_worker", u},
                                       {"observations_per_worker", u},
                                       {"total_observations", u},
                                       {"per_observation_allowance_ms", u},
                                       {"fixed_reserve_seconds", u},
                                       {"execution_deadline_seconds", u}};
    const nlohmann::json collection = {
        {"schema", MonitoringQ1PlanV2Value::schema},
        {"path", "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json"},
        {"sha256", std::string(64, 'f')},
        {"repository_commit", commit},
        {"observations_per_worker", u},
        {"conservative_observation_capacity_per_worker", u},
        {"production_warmup_observations", 0},
        {"sequencing", "round_robin_cycle_then_worker_id"},
        {"stopping", "fixed_inventory"},
        {"retry", "forbidden"},
        {"discard", "forbidden"},
        {"execution_deadline_seconds", u},
        {"success_envelope_bytes", 10485760},
        {"evidence_artifact_bytes", 8388608},
        {"build_manifest_bytes", 1048576},
        {"maximum_successor_tree_bytes", u},
        {"derivation", derivation}};
    nlohmann::json semantics;
    for (auto key : {"availability", "readability", "field_projection", "counter_width",
                     "cumulative_units", "worker_scope", "worker_tid_association",
                     "oracle_agreement", "operational_errors", "privacy"})
        semantics[key] = "passed";
    const nlohmann::json evidence = {
        {"schema", MonitoringQ1PlanV2Value::evidence_schema},
        {"qualification_id", plan.qid},
        {"tool",
         {{"identity", "epycbench.monitoring-qualification-tool/v1"}, {"version", "1.0.0"}}},
        {"repository", {{"commit", commit}, {"dirty", false}}},
        {"build", {{"identity", "sha256:" + std::string(64, 'f')}}},
        {"profile",
         {{"schema", MonitoringQ1PlanV2Value::profile_schema},
          {"identity", "epycbench.monitoring-qualification.host-generic/v2"},
          {"stage", "final_frozen_profile"},
          {"sha256", plan.profile_sha}}},
        {"q0_provenance",
         {{"qualification_id", plan.qid},
          {"manifest_sha256", std::string(64, 'f')},
          {"checksums_sha256", std::string(64, 'f')},
          {"host_environment_sha256", std::string(64, 'f')},
          {"q0b_evidence_sha256", std::string(64, 'f')}}},
        {"collection_plan", collection},
        {"host_environment",
         {{"schema", "epycbench.monitoring-host-environment/v2"},
          {"sha256", "sha256:" + std::string(64, 'f')}}},
        {"qualification_plan",
         {{"schema", "epycbench.monitoring-host-qualification-plan/v2"},
          {"sha256", plan.plan_digest}}},
        {"phase", "Q1"},
        {"prerequisites", nlohmann::json::array()},
        {"implementation",
         {{"source", "linux-proc-task-stat"},
          {"fields", {10, 12}},
          {"counters", "cumulative-uint64"},
          {"clock", "clock-monotonic-raw"}}},
        {"oracle",
         {{"source", "linux-getrusage-thread"},
          {"scope", "target-worker"},
          {"comparison", "before-production-after-equality"}}},
        {"workers", workers},
        {"observations", observations},
        {"summaries", {{"workers", summaries}, {"global", summary(false)}}},
        {"source_semantics", semantics},
        {"known_event", {{"required", false}, {"disposition", "not_performed"}}},
        {"finalization", {{"disposition", "succeeded"}}},
        {"disposition", "pass"},
        {"privacy_validation", "passed"}};
    auto evidence_size = canonical_size(evidence);
    if (!evidence_size)
        return std::unexpected{std::move(evidence_size.error())};
    auto empty_wrapper = canonical_size({{"schema", MonitoringQ1PlanV2Value::backend_schema},
                                         {"build_manifest", nlohmann::json::object()},
                                         {"evidence", evidence}});
    if (!empty_wrapper || *empty_wrapper < 2U)
        return std::unexpected{error("capacity_canonicalization")};
    return std::array<std::uint64_t, 2>{*evidence_size, *empty_wrapper - 2U + 1048576U};
}
} // namespace
MonitoringQ1PlanV2Value::MonitoringQ1PlanV2Value(
    std::string q, core::Sha256Digest p, core::Sha256Digest d, std::vector<core::WorkerId> w,
    std::uint64_t n, std::uint64_t c, std::uint64_t t, std::uint64_t deadline, std::uint64_t tree,
    MonitoringQ1PlanDerivationMode mode, std::uint64_t allowance, std::uint64_t reserve) noexcept
    : qid_(std::move(q)), profile_(p), plan_(d), workers_(std::move(w)), n_(n), capacity_(c),
      total_(t), deadline_(deadline), tree_(tree), mode_(mode), allowance_(allowance),
      reserve_(reserve) {}
auto detail_MonitoringQ1PlanV2ValueTestAccess::compute_limits(
    const MonitoringQ1PlanV2ParsedCandidate& value, std::string_view commit)
    -> std::expected<MonitoringQ1PlanV2ComputedLimits, core::Error> {
    if ((commit.size() != 40U && commit.size() != 64U) ||
        !std::ranges::all_of(
            commit, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        value.workers.empty())
        return std::unexpected{error("capacity_input")};
    auto tree = successor_tree_limit_value();
    if (!tree)
        return std::unexpected{std::move(tree.error())};
    auto one = model_byte_sizes(value, 1U, commit);
    auto two = model_byte_sizes(value, 2U, commit);
    if (!one || !two)
        return std::unexpected{error("capacity_model")};
    const auto evidence_increment = (*two)[0] - (*one)[0];
    const auto envelope_increment = (*two)[1] - (*one)[1];
    const auto sizes_for =
        [&](std::uint64_t count) -> std::expected<std::array<std::uint64_t, 2>, core::Error> {
        const auto tail = count - 1U;
        if (tail > (std::numeric_limits<std::uint64_t>::max() - (*one)[0]) / evidence_increment ||
            tail > (std::numeric_limits<std::uint64_t>::max() - (*one)[1]) / envelope_increment)
            return std::unexpected{error("capacity_overflow")};
        return std::array<std::uint64_t, 2>{(*one)[0] + tail * evidence_increment,
                                            (*one)[1] + tail * envelope_increment};
    };
    std::uint64_t low = 0, high = 1;
    while (high <= std::numeric_limits<std::uint64_t>::max() / 2U) {
        auto sizes = sizes_for(high);
        if (!sizes)
            return std::unexpected{std::move(sizes.error())};
        if ((*sizes)[0] > MonitoringQ1PlanV2Value::artifact_limit ||
            (*sizes)[1] > MonitoringQ1PlanV2Value::envelope_limit)
            break;
        low = high;
        high *= 2U;
    }
    while (low + 1U < high) {
        const auto middle = low + (high - low) / 2U;
        auto sizes = sizes_for(middle);
        if (!sizes)
            return std::unexpected{std::move(sizes.error())};
        if ((*sizes)[0] <= MonitoringQ1PlanV2Value::artifact_limit &&
            (*sizes)[1] <= MonitoringQ1PlanV2Value::envelope_limit)
            low = middle;
        else
            high = middle;
    }
    if (low == 0U)
        return std::unexpected{error("capacity_empty")};
    return MonitoringQ1PlanV2ComputedLimits{low, *tree};
}
auto detail_MonitoringQ1PlanV2ValueTestAccess::model_fits(
    const MonitoringQ1PlanV2ParsedCandidate& value, std::string_view commit,
    std::uint64_t observations_per_worker) -> std::expected<bool, core::Error> {
    if (observations_per_worker == 0U)
        return std::unexpected{error("capacity_input")};
    auto sizes = model_byte_sizes(value, observations_per_worker, commit);
    if (!sizes)
        return std::unexpected{std::move(sizes.error())};
    return (*sizes)[0] <= MonitoringQ1PlanV2Value::artifact_limit &&
           (*sizes)[1] <= MonitoringQ1PlanV2Value::envelope_limit;
}
auto detail_MonitoringQ1PlanV2ValueTestAccess::model_sizes(
    const MonitoringQ1PlanV2ParsedCandidate& value, std::string_view commit,
    std::uint64_t observations_per_worker)
    -> std::expected<MonitoringQ1PlanV2ModelSizes, core::Error> {
    if (observations_per_worker == 0U)
        return std::unexpected{error("capacity_input")};
    auto sizes = model_byte_sizes(value, observations_per_worker, commit);
    if (!sizes)
        return std::unexpected{std::move(sizes.error())};
    return MonitoringQ1PlanV2ModelSizes{(*sizes)[0], (*sizes)[1]};
}
auto detail_MonitoringQ1PlanV2ValueTestAccess::validate(MonitoringQ1PlanV2ParsedCandidate v,
                                                        MonitoringQ1PlanV2ComputedLimits computed)
    -> std::expected<MonitoringQ1PlanV2Value, core::Error> {
    if (!qid(v.qid))
        return std::unexpected{error("binding")};
    if (v.schema != MonitoringQ1PlanV2Value::schema ||
        v.profile_schema != MonitoringQ1PlanV2Value::profile_schema ||
        v.profile_path != MonitoringQ1PlanV2Value::profile_path || v.warmup != 0 ||
        v.sequencing != "round_robin_cycle_then_worker_id" || v.stopping != "fixed_inventory" ||
        v.retry != "forbidden" || v.discard != "forbidden" ||
        v.summaries != "count_min_max_lower_median" ||
        v.envelope != MonitoringQ1PlanV2Value::envelope_limit ||
        v.artifact != MonitoringQ1PlanV2Value::artifact_limit || v.privacy != "passed" ||
        v.algorithm != MonitoringQ1PlanV2Value::algorithm ||
        v.target != MonitoringQ1PlanV2Value::target_total)
        return std::unexpected{error("constant")};
    auto profile = digest(v.profile_sha, false);
    if (!profile)
        return std::unexpected{std::move(profile.error())};
    auto plan = digest(v.plan_digest, true);
    if (!plan)
        return std::unexpected{std::move(plan.error())};
    if (v.worker_count < 1 || v.worker_count > 4294967296ULL || v.workers.empty() ||
        v.worker_count != v.workers.size() || v.worker_count != v.derived_workers)
        return std::unexpected{error("worker_set")};
    if (v.capacity == 0 || v.capacity != v.derived_capacity || v.capacity != computed.capacity() ||
        v.tree == 0 || v.tree != computed.maximum_successor_tree_bytes())
        return std::unexpected{error("capacity_input")};
    if (v.n == 0 || v.n > v.capacity || v.n != v.derived_n || v.allowance == 0 || v.reserve == 0)
        return std::unexpected{error("derivation")};
    if (v.deadline == 0 || v.deadline != v.derived_deadline)
        return std::unexpected{error("deadline")};
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(v.workers.size());
    std::vector<core::WorkerId> workers;
    workers.reserve(v.workers.size());
    for (auto id : v.workers) {
        if (id > std::numeric_limits<std::uint32_t>::max() ||
            !seen.insert(static_cast<std::uint32_t>(id)).second)
            return std::unexpected{error("worker_set")};
        workers.emplace_back(static_cast<std::uint32_t>(id));
    }
    if (v.worker_count > std::numeric_limits<std::uint64_t>::max() / v.n)
        return std::unexpected{error("total_overflow")};
    const auto total = v.worker_count * v.n;
    if (total != v.total)
        return std::unexpected{error("total")};
    const auto default_n = std::min(v.capacity, std::max<std::uint64_t>(1, 4096 / v.worker_count));
    MonitoringQ1PlanDerivationMode mode;
    if (v.mode == "default") {
        mode = MonitoringQ1PlanDerivationMode::default_mode;
        if (v.n != default_n || v.allowance != 100 || v.reserve != 120)
            return std::unexpected{error("default_derivation")};
        const auto increment = total / 10 + (total % 10 != 0 ? 1U : 0U);
        if (v.reserve > std::numeric_limits<std::uint64_t>::max() - increment ||
            v.deadline != v.reserve + increment)
            return std::unexpected{error("deadline")};
    } else if (v.mode == "governed_override") {
        mode = MonitoringQ1PlanDerivationMode::governed_override;
        if (v.n > default_n || (v.n == default_n && v.allowance == 100 && v.reserve == 120))
            return std::unexpected{error("override_derivation")};
    } else
        return std::unexpected{error("mode")};
    if (mode == MonitoringQ1PlanDerivationMode::governed_override) {
        if (total > std::numeric_limits<std::uint64_t>::max() / v.allowance)
            return std::unexpected{error("deadline_overflow")};
        const auto product = total * v.allowance;
        const auto increment = product / 1000 + (product % 1000 != 0 ? 1U : 0U);
        if (v.reserve > std::numeric_limits<std::uint64_t>::max() - increment ||
            v.deadline != v.reserve + increment)
            return std::unexpected{error("deadline")};
    }
    return MonitoringQ1PlanV2Value{
        std::move(v.qid), *profile, *plan, std::move(workers), v.n,      v.capacity, total,
        v.deadline,       v.tree,   mode,  v.allowance,        v.reserve};
}

auto MonitoringQ1PlanV2Value::decode(std::string_view bytes, std::string_view captured_commit)
    -> std::expected<MonitoringQ1PlanV2Value, core::Error> {
    try {
        if (bytes.empty() || bytes.size() > 1024U * 1024U || bytes.back() != '\n' ||
            bytes.find('\n') != bytes.size() - 1U)
            return std::unexpected{error("canonical_bytes")};
        const auto body = bytes.substr(0, bytes.size() - 1U);
        BoundedPlanSax sax;
        if (!nlohmann::json::sax_parse(body.begin(), body.end(), &sax) || sax.invalid())
            return std::unexpected{error("structural_bound")};
        const auto root = nlohmann::json::parse(body);
        auto canonical = core::canonicalize_json(root);
        if (!canonical || *canonical != body)
            return std::unexpected{error("canonical_bytes")};
        const auto closed = [](const nlohmann::json& value,
                               std::initializer_list<std::string_view> names) {
            return value.is_object() && value.size() == names.size() &&
                   std::ranges::all_of(names, [&](auto name) { return value.contains(name); });
        };
        const auto text = [](const nlohmann::json& value) {
            if (!value.is_string())
                throw std::invalid_argument{"string"};
            return value.get<std::string>();
        };
        const auto number = [](const nlohmann::json& value) {
            if (!value.is_number_unsigned())
                throw std::invalid_argument{"uint64"};
            return value.get<std::uint64_t>();
        };
        if (!closed(root, {"schema", "qualification_id", "profile", "worker_set", "collection",
                           "evidence_limits", "privacy_validation", "derivation"}))
            return std::unexpected{error("schema")};
        const auto& p = root.at("profile");
        const auto& w = root.at("worker_set");
        const auto& c = root.at("collection");
        const auto& l = root.at("evidence_limits");
        const auto& d = root.at("derivation");
        if (!closed(p, {"schema", "sha256", "repository_path"}) ||
            !closed(w, {"qualification_plan_digest", "worker_count", "worker_ids"}) ||
            !closed(c,
                    {"observations_per_worker", "production_warmup_observations", "sequencing",
                     "stopping", "retry", "discard", "summaries", "execution_deadline_seconds"}) ||
            !closed(l, {"success_envelope_bytes", "evidence_artifact_bytes",
                        "maximum_successor_tree_bytes",
                        "conservative_observation_capacity_per_worker"}) ||
            !closed(d, {"algorithm", "mode", "target_total_observations", "worker_count",
                        "conservative_observation_capacity_per_worker", "observations_per_worker",
                        "total_observations", "per_observation_allowance_ms",
                        "fixed_reserve_seconds", "execution_deadline_seconds"}) ||
            !w.at("worker_ids").is_array())
            return std::unexpected{error("schema")};
        std::vector<std::uint64_t> ids;
        ids.reserve(w.at("worker_ids").size());
        for (const auto& id : w.at("worker_ids"))
            ids.push_back(number(id));
        MonitoringQ1PlanV2ParsedCandidate candidate{
            text(root.at("schema")),
            text(root.at("qualification_id")),
            text(p.at("schema")),
            text(p.at("sha256")),
            text(p.at("repository_path")),
            text(w.at("qualification_plan_digest")),
            number(w.at("worker_count")),
            std::move(ids),
            number(c.at("observations_per_worker")),
            number(c.at("production_warmup_observations")),
            text(c.at("sequencing")),
            text(c.at("stopping")),
            text(c.at("retry")),
            text(c.at("discard")),
            text(c.at("summaries")),
            number(c.at("execution_deadline_seconds")),
            number(l.at("success_envelope_bytes")),
            number(l.at("evidence_artifact_bytes")),
            number(l.at("maximum_successor_tree_bytes")),
            number(l.at("conservative_observation_capacity_per_worker")),
            text(root.at("privacy_validation")),
            text(d.at("algorithm")),
            text(d.at("mode")),
            number(d.at("target_total_observations")),
            number(d.at("worker_count")),
            number(d.at("conservative_observation_capacity_per_worker")),
            number(d.at("observations_per_worker")),
            number(d.at("total_observations")),
            number(d.at("per_observation_allowance_ms")),
            number(d.at("fixed_reserve_seconds")),
            number(d.at("execution_deadline_seconds"))};
        auto computed =
            detail_MonitoringQ1PlanV2ValueTestAccess::compute_limits(candidate, captured_commit);
        if (!computed)
            return std::unexpected{std::move(computed.error())};
        return detail_MonitoringQ1PlanV2ValueTestAccess::validate(std::move(candidate), *computed);
    } catch (const nlohmann::json::exception&) {
        return std::unexpected{error("decode")};
    } catch (const std::invalid_argument&) {
        return std::unexpected{error("decode")};
    } catch (const std::out_of_range&) {
        return std::unexpected{error("decode")};
    }
}
} // namespace epycbench::monitoring::qualification
