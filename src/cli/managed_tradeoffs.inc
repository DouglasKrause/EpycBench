// Called with ownership of the managed study root. Completion replay checks the
// original Results against their generation ledger and ranks before returning samples.
auto managed_tradeoffs(std::ostream& output, const ops::Directory& owner, topology::cli::Environment& environment,
                       exp::TradeoffPolicy policy, std::string_view performance)
    -> std::expected<void, Error> {
    DiscoveryTradeoffs observed;
    auto study = retained_search_study(owner, environment, SearchView::population, &observed);
    if (!study) return std::unexpected{study.error()};
    const auto& declared = study->at("declared_protocol");
    const auto selected = performance.empty() ? declared.at("objective").get<std::string_view>() : performance;
    policy.performance = selected == "tasks_per_second" ? Objective::throughput : Objective::time_per_task;
    if (auto checked = exp::validate_tradeoff_policy(policy); !checked) return std::unexpected{checked.error()};

    const bool stopped = study->at("state") == "scientifically_stopped";
    auto trials = Json::array();
    auto rejected = Json::array();
    auto confirmations = Json::array();
    std::vector<exp::TradeoffSample> samples;
    for (const auto& generation : study->at("generations")) {
        const auto index = generation.at("generation").get<std::size_t>();
        for (const auto& proposal : generation.at("proposals")) {
            if (const auto rejection = proposal.find("rejection"); rejection != proposal.end()) {
                rejected.push_back({{"generation", index}, {"candidate_ordinal", proposal.at("ordinal")},
                    {"code", rejection->at("code")}, {"pointer", rejection->at("pointer")}});
                continue;
            }
            const auto& id = proposal.at("trial_sha256").get_ref<const std::string&>();
            const auto& eligibility = proposal.at("eligibility");
            auto row = Json{{"ordinal", trials.size()}, {"generation", index}, {"local_trial", proposal.at("trial_ordinal")},
                {"candidate_ordinal", proposal.at("ordinal")}, {"trial_sha256", id}, {"candidate_sha256", proposal.at("candidate_sha256")},
                {"problem_sha256", proposal.at("problem_sha256")}, {"state", proposal.at("state")},
                {"evaluation", "excluded"}, {"reason", stopped ? Json("scientific_stop") : eligibility},
                {"result", proposal.at("result")}};
            if (auto& result = row.at("result"); !result.is_null())
                result["artifact"] = (fs::path(genetic_child_leaf(index)) / result.at("artifact").get<std::string>()).string();
            if (!stopped && eligibility == "eligible") {
                const auto sample = observed.find(id);
                if (sample == observed.end()) {
                    row["evaluation"] = "unavailable";
                    row["reason"] = "task_metric_build_or_topology_unavailable";
                } else {
                    auto value = sample->second;
                    value.trial_ordinal = trials.size();
                    samples.push_back(std::move(value));
                    row["evaluation"] = "included";
                    row["reason"] = nullptr;
                }
            }
            if (proposal.contains("result_unavailable")) row["result_unavailable"] = proposal.at("result_unavailable");
            if (proposal.contains("verification_code")) row["verification_code"] = proposal.at("verification_code");
            for (const auto* key : {"measured_problem_sha256", "observed_inputs_sha256", "calibrated_repetitions"})
                if (proposal.contains(key)) row[key] = proposal.at(key);
            trials.push_back(std::move(row));
        }
        auto confirmation = generation.at("confirmation");
        confirmation["generation"] = index;
        confirmations.push_back(std::move(confirmation));
    }
    auto report = Json{{"schema", "epycbench.managed-study-tradeoffs/v1"}, {"purpose", "managed_discovery"},
        {"scope", "retained_discovery_generations"}, {"plan_sha256", study->at("plan_sha256")},
        {"history_head_sha256", study->at("history_head_sha256")}, {"state", study->at("state")},
        {"observed_cohort", study->at("observed_cohort")}, {"population_binding", study->at("full_population_artifact_binding")},
        {"candidate_count", declared.at("candidate_count")}, {"charged_attempts", study->at("charged_attempts")},
        {"trial_count", trials.size()}, {"trials", std::move(trials)}, {"rejected_proposals", std::move(rejected)},
        {"coverage", std::move(study->at("coverage"))}, {"confirmations", std::move(confirmations)}};
    return render_tradeoffs(output, std::move(report), policy, samples,
        declared.value("performance_scope", std::string_view{}) == "whole_group");
}
