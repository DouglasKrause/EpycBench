// Included beside the managed-study consumers so custody has one implementation.
auto search_confirmation(const ops::Directory& generation, const GeneticStudyPlan& study,
                         const exp::GeneticGenerationIntent& intent, const GeneticChildData& child,
                         bool finished) -> std::expected<Json, Error> {
    const auto names = generation.list(4U * child.plan.schedule.trials().size() + 18U);
    if (!names) return std::unexpected{names.error()};
    if (!std::ranges::binary_search(*names, confirmation_leaf))
        return Json{{"state", "not_created"}, {"result", nullptr}};
    if (!finished || !child.store.head())
        return std::unexpected{problem("search_comparison.confirmation", "Confirmation requires a finished unstopped study")};
    auto directory = ops::Directory::acquire(generation.path() / confirmation_leaf);
    if (!directory) return std::unexpected{directory.error()};
    const auto bytes = directory->read(plan_file, maximum_plan_bytes);
    if (!bytes) return std::unexpected{bytes.error()};
    auto plan = decode_plan(*bytes);
    if (!plan) return std::unexpected{plan.error()};
    if (!plan->confirmation || !plan->confirmation->genetic ||
        plan->confirmation->genetic->study_plan != study.history.plan_digest ||
        plan->confirmation->genetic->generation != intent.index())
        return std::unexpected{problem("search_comparison.confirmation", "Confirmation differs from the owning study generation")};
    auto origin = verify_genetic_confirmation_origin(generation, study, intent, *child.store.head(), *plan);
    if (!origin) return std::unexpected{origin.error()};
    exp::Ledger ledger{plan->schedule};
    auto store = exp::LedgerStore::open(*directory, core::sha256(*bytes).to_hex(), ledger);
    if (!store) return std::unexpected{store.error()};
    auto summary = confirmation_status(*directory, *plan, ledger);
    if (!summary) return std::unexpected{summary.error()};
    (*summary)["plan_sha256"] = core::sha256(*bytes).to_hex();
    (*summary)["ledger_head_sha256"] = store->head() ? Json(store->head()->to_hex()) : Json(nullptr);
    (*summary)["charged_attempts"] = ledger.charged_attempts();
    return summary;
}

struct ManagedAnalysisWindow {
    std::size_t offset;
    std::size_t limit;
    AnalysisPage page{};
    std::size_t trial_count{};
    bool stopped{};
};

auto search_generation(const ops::Directory& owner, const GeneticStudyPlan& study,
                       const exp::GeneticGenerationIntent& intent, const Json* assessed, bool finished, bool retain_lineage = false,
                       ManagedAnalysisWindow* analysis = nullptr)
    -> std::expected<Json, Error> {
    auto directory = ops::Directory::acquire(owner.path() / genetic_child_leaf(intent.index()));
    if (!directory) return std::unexpected{directory.error()};
    auto child = load_genetic_child(*directory, study, intent);
    if (!child) return std::unexpected{child.error()};
    if (analysis && child->ledger.scientifically_stopped()) analysis->stopped = true;
    const auto factual = assessed ? *assessed : describe_genetic_child(intent, *child);
    const Json ledger_head = child->store.head() ? Json(child->store.head()->to_hex()) : Json(nullptr);
    if (assessed && (factual.at("child_ledger_head_sha256") != ledger_head ||
                     factual.at("charged") != child->ledger.charged_attempts()))
        return std::unexpected{problem("search_comparison.child_changed", "A completed child changed after verified history replay")};
    auto report = Json{{"generation", intent.index()}, {"child_plan_sha256", intent.child_plan.to_hex()},
        {"child_ledger_head_sha256", child->store.head() ? Json(child->store.head()->to_hex()) : Json(nullptr)},
        {"charged_attempts", child->ledger.charged_attempts()},
        {"completion", assessed ? "verified" : "not_recorded"}, {"proposals", Json::array()}};
    std::vector<std::uint64_t> reservations;
    for (const auto& entry : child->ledger.entries())
        if (std::holds_alternative<exp::Reservation>(entry.event)) reservations.push_back(entry.sequence);
    std::size_t proposal_index{};
    for (const auto& proposal : factual.at("proposals")) {
        Json row{{"ordinal", proposal.at("ordinal")}, {"trial_ordinal", proposal.at("trial_ordinal")},
                 {"state", proposal.at("state")}, {"reservation_sequence", nullptr}, {"result", nullptr}};
        for (const auto* key : {"trial_sha256", "candidate_sha256", "problem_sha256", "measured_problem_sha256", "observed_inputs_sha256",
                                "calibrated_repetitions", "result", "rejection", "result_unavailable", "verification_code"})
            if (proposal.contains(key)) row[key] = proposal.at(key);
        if (!row.at("trial_ordinal").is_null()) {
            const auto ordinal = row.at("trial_ordinal").get<std::size_t>();
            if (ordinal < reservations.size()) row["reservation_sequence"] = reservations[ordinal];
        }
        row["eligibility"] = proposal.value("parent_status", row.at("trial_ordinal").is_null() ? "rejected_proposal" : "not_assessed_pending_generation");
        if (analysis && !row.at("trial_ordinal").is_null()) {
            const auto ordinal = analysis->trial_count++;
            if (ordinal >= analysis->offset && ordinal - analysis->offset < analysis->limit) {
                const auto local = row.at("trial_ordinal").get<std::size_t>();
                const auto& trial = child->ledger.schedule().trials()[local];
                Json location{{"generation", intent.index()}, {"local_trial", local},
                              {"candidate_ordinal", proposal.at("ordinal")}, {"child_plan_sha256", intent.child_plan.to_hex()},
                              {"child_ledger_head_sha256", ledger_head}, {"eligibility", row.at("eligibility")}};
                for (const auto* key : {"verification_code", "result_unavailable"})
                    if (row.contains(key)) location[key] = row.at(key);
                auto added = append_analysis_trial(analysis->page, *directory, child->plan.frozen.at(local),
                    child->ledger, trial, ordinal, row.at("eligibility") == "eligible",
                    genetic_child_leaf(intent.index()), std::move(location));
                if (!added) return std::unexpected{added.error()};
            }
        }
        if (retain_lineage) {
            row["proposal_index"] = proposal_index;
            row["discovery_score"] = nullptr;
            if (row.at("eligibility") == "eligible") {
                row["discovery_score"] = {
                    {"rank", proposal.at("rank")}, {"objective_value", proposal.at("objective_value")},
                    {"candidate_tasks_per_second", proposal.at("candidate_tasks_per_second")}};
                if (proposal.contains("performance_unit"))
                    row["discovery_score"]["performance_unit"] = proposal.at("performance_unit");
            }
            row["lineage"] = Json::object();
            for (const auto* key : {"origin", "parent_ordinal", "axis_change", "crossover", "mutation", "elite_probability_units"})
                if (proposal.contains(key)) row["lineage"][key] = proposal.at(key);
        }
        ++proposal_index;
        report["proposals"].push_back(std::move(row));
    }
    auto confirmation = search_confirmation(*directory, study, intent, *child, finished);
    if (!confirmation) return std::unexpected{confirmation.error()};
    report["confirmation"] = std::move(*confirmation);
    return report;
}

enum class SearchView { comparison, replay, population };

auto retained_search_study(const ops::Directory& owner, topology::cli::Environment& environment,
                           SearchView view = SearchView::comparison,
                           DiscoveryTradeoffs* tradeoffs = nullptr, ManagedAnalysisWindow* analysis = nullptr)
    -> std::expected<Json, Error> {
    const bool retain_lineage = view != SearchView::comparison;
    const auto bytes = owner.read(plan_file, maximum_plan_bytes);
    if (!bytes) return std::unexpected{bytes.error()};
    auto study = decode_genetic_study(*bytes, environment);
    if (!study) return std::unexpected{study.error()};
    GeneticStudyVerification verification{owner, *study};
    verification.tradeoffs = tradeoffs;
    auto history = exp::GeneticHistory::open(owner, study->history, {&verification, verify_genetic_completion});
    if (!history) return std::unexpected{history.error()};
    bool finished{};
    std::string_view state = "pending";
    if (!history->pending()) {
        if (history->scientifically_stopped()) state = "scientifically_stopped";
        else {
            const auto next = next_study_batch(*history, study->history.strategy);
            if (!next) return std::unexpected{next.error()};
            finished = !*next;
            state = finished ? "exhausted" : "ready";
        }
    }
    const auto root = genetic_study_tree(*bytes);
    const auto& raw = study->population ? root.at("strategy") : root;
    const auto& config = study->history;
    Json report{{"plan_sha256", config.plan_digest.to_hex()},
        {"history_head_sha256", history->head() ? Json(history->head()->to_hex()) : Json(nullptr)},
        {"profile", raw.at("profile")}, {"proposal_profile", raw.at("proposal_profile")},
        {"root_seed", config.root_seed}, {"state", state},
        {"declared_protocol", {{"space_format", raw.at("space_format")},
            {"space_sha256", raw.at("space_sha256")}, {"topology_sha256", raw.at("topology_sha256")},
            {"candidate_count", study->space.size()}, {"axis_cardinalities", study->space.axis_cardinalities()},
            {"objective", scenario::objective_token(study->space.objective())},
            {"objective_orientation", scenario::objective_orientation(study->space.objective())},
            {"attempt_budget", config.attempt_budget}}},
        {"observed_cohort", {{"value", verification.cohort ? genetic_cohort_document(*verification.cohort) : Json(nullptr)},
            {"absent_reason", verification.cohort ? Json(nullptr) : Json("no_verified_completed_generation_cohort")}}},
        {"full_population_artifact_binding", {{"value", study->population ? Json(study->population->digest().to_hex()) : Json(nullptr)},
            {"absent_reason", study->population ? Json(nullptr) : Json("not_frozen_by_managed_plan")}}},
        {"time_cost", {{"value", nullptr}, {"absent_reason", "not_recorded"}}},
        {"generations", Json::array()}};
    auto first = study->space.materialize(0U);
    if (!first) return std::unexpected{first.error()};
    report["declared_protocol"]["performance_scope"] = whole_group_scenario(Json(*first))
        ? "whole_group" : "candidate_role";
    Json settings{{"maximum_generations", config.maximum_generations}};
    for (const auto* key : {"population", "mutation_per_mille", "batch_size", "cooling"})
        if (raw.contains(key)) settings[key] = raw.at(key);
    report["strategy_settings"] = std::move(settings);
    std::size_t charged{};
    for (const auto& generation : history->completed()) {
        const auto& assessed = verification.completed.at(generation.intent.index());
        auto row = search_generation(owner, *study, generation.intent, &assessed, finished, retain_lineage, analysis);
        if (!row) return std::unexpected{row.error()};
        charged += row->at("charged_attempts").get<std::size_t>();
        report["generations"].push_back(std::move(*row));
    }
    if (const auto* pending = history->pending()) {
        auto row = search_generation(owner, *study, *pending, nullptr, false, retain_lineage, analysis);
        if (!row) return std::unexpected{row.error()};
        charged += row->at("charged_attempts").get<std::size_t>();
        report["generations"].push_back(std::move(*row));
    }
    report["charged_attempts"] = charged;
    if (retain_lineage) {
        std::vector<bool> proposed(study->space.size()), explored(study->space.size());
        std::size_t proposal_count{};
        for (const auto& generation : report.at("generations")) {
            for (const auto& proposal : generation.at("proposals")) {
                const auto ordinal = proposal.at("ordinal").get<std::size_t>();
                proposed.at(ordinal) = true;
                if (!proposal.at("reservation_sequence").is_null()) explored.at(ordinal) = true;
                ++proposal_count;
            }
        }
        const auto proposed_count = std::ranges::count(proposed, true);
        const auto explored_count = std::ranges::count(explored, true);
        Json population_coverage{{"value", nullptr}, {"absent_reason", "not_frozen_by_managed_plan"}};
        if (study->population) {
            std::size_t resolved{}, rejected{}, untested{};
            auto members = Json::array();
            for (const auto& entry : study->population->document().at("entries")) {
                const auto ordinal = entry.at("ordinal").get<std::size_t>();
                const bool attempted = explored.at(ordinal);
                if (view == SearchView::population) {
                    Json member{{"ordinal", ordinal}, {"state", entry.at("state")},
                        {"proposed", static_cast<bool>(proposed.at(ordinal))}, {"charged", attempted}};
                    if (entry.contains("candidate_sha256")) member["candidate_sha256"] = entry.at("candidate_sha256");
                    if (entry.contains("rejection")) member["rejection"] = entry.at("rejection");
                    members.push_back(std::move(member));
                }
                if (entry.at("state") == "resolved") {
                    ++resolved;
                    if (!attempted) ++untested;
                } else {
                    if (attempted) return std::unexpected{problem("search_replay.population",
                        "A charged reservation references a rejected frozen candidate")};
                    ++rejected;
                }
            }
            population_coverage = {{"value", {{"resolved_candidates", resolved}, {"rejected_candidates", rejected},
                {"untested_resolved_candidates", untested}}},
                {"absent_reason", nullptr}};
            if (view == SearchView::population) {
                auto& value = population_coverage.at("value");
                value["axes"] = study->space.describe_axes();
                value["members"] = std::move(members);
            }
        }
        report["coverage"] = {{"explored_definition", "distinct_candidate_ordinals_with_charged_reservation"},
            {"recorded_proposals", proposal_count}, {"distinct_proposed_candidates", proposed_count},
            {"distinct_explored_candidates", explored_count}, {"frozen_population", std::move(population_coverage)}};
    }
    return report;
}

auto managed_analysis(std::ostream& output, const ops::Directory& owner, topology::cli::Environment& environment,
                      std::size_t offset, std::size_t limit) -> std::expected<void, Error> {
    if (limit == 0U || limit > 64U)
        return std::unexpected{problem("analysis.page", "Use a page size from 1 through 64")};
    ManagedAnalysisWindow window{offset, limit};
    auto study = retained_search_study(owner, environment, SearchView::comparison, nullptr, &window);
    if (!study) return std::unexpected{study.error()};
    if (offset > window.trial_count)
        return std::unexpected{problem("analysis.page", "The offset exceeds the recorded trial count")};
    if (window.stopped || study->at("state") == "scientifically_stopped") window.page.points.clear();

    const auto end = offset + window.page.rows.size();
    const auto& declared = study->at("declared_protocol");
    auto report = Json{{"schema", "epycbench.managed-study-analysis/v1"}, {"purpose", "managed_discovery"},
        {"plan_sha256", study->at("plan_sha256")}, {"history_head_sha256", study->at("history_head_sha256")},
        {"objective", declared.at("objective")}, {"objective_orientation", declared.at("objective_orientation")},
        {"state", window.stopped ? Json("scientifically_stopped") : study->at("state")},
        {"charged_attempts", study->at("charged_attempts")}, {"trial_count", window.trial_count},
        {"offset", offset}, {"next_offset", end == window.trial_count ? Json(nullptr) : Json(end)},
        {"confirmations", Json::array()}, {"rejected_proposals", Json::array()}};
    for (auto& generation : study->at("generations")) {
        auto& confirmation = generation.at("confirmation");
        confirmation["generation"] = generation.at("generation");
        report["confirmations"].push_back(std::move(confirmation));
        for (const auto& proposal : generation.at("proposals"))
            if (const auto rejection = proposal.find("rejection"); rejection != proposal.end())
                report["rejected_proposals"].push_back({{"generation", generation.at("generation")},
                    {"candidate_ordinal", proposal.at("ordinal")}, {"rejection", *rejection}});
    }
    return emit_analysis(output, std::move(report), window.page);
}

auto compare_search_studies(std::ostream& output, topology::cli::Environment& environment,
                            const fs::path& first_path, const fs::path& second_path)
    -> std::expected<void, Error> {
    auto first = ops::Directory::acquire(first_path);
    if (!first) return std::unexpected{first.error()};
    auto second = ops::Directory::acquire(second_path);
    if (!second) return std::unexpected{second.error()};
    auto left = retained_search_study(*first, environment);
    if (!left) return std::unexpected{left.error()};
    auto right = retained_search_study(*second, environment);
    if (!right) return std::unexpected{right.error()};
    Json checks = Json::object();
    bool compatible = true;
    for (const auto* key : {"space_format", "space_sha256", "topology_sha256", "candidate_count", "axis_cardinalities", "objective", "objective_orientation", "attempt_budget"}) {
        const bool equal = left->at("declared_protocol").at(key) == right->at("declared_protocol").at(key);
        checks[key] = equal;
        compatible = compatible && equal;
    }
    const auto& left_cohort = left->at("observed_cohort").at("value");
    const auto& right_cohort = right->at("observed_cohort").at("value");
    Json cohort_match = left_cohort.is_null() || right_cohort.is_null() ? Json(nullptr) : Json(left_cohort == right_cohort);
    const auto& left_population = left->at("full_population_artifact_binding").at("value");
    const auto& right_population = right->at("full_population_artifact_binding").at("value");
    const bool populations_bound = !left_population.is_null() && !right_population.is_null();
    Json report{{"schema", "epycbench.search-comparability/v1"},
        {"declared_protocol", {{"compatible", compatible}, {"checks", std::move(checks)}}},
        {"observed_cohort_match", {{"value", std::move(cohort_match)},
            {"absent_reason", left_cohort.is_null() || right_cohort.is_null() ? Json("eligible_cohort_missing") : Json(nullptr)}}},
        {"full_population_artifact_equality", {{"value", populations_bound ? Json(left_population == right_population) : Json(nullptr)},
            {"absent_reason", populations_bound ? Json(nullptr) : Json("not_frozen_by_managed_plans")}}},
        {"time_absent", true}, {"studies", Json::array({std::move(*left), std::move(*right)})}};
    const auto bytes = report.dump();
    if (bytes.size() > 16U * mib)
        return std::unexpected{problem("search_comparison.capacity", "The retained comparison report exceeds its bounded output size")};
    output << bytes << '\n';
    if (!output) return std::unexpected{problem("search_comparison.output", "Could not write the retained comparison report")};
    return {};
}

// This consumer opens only retained managed custody, never the dispatcher.
auto replay_search_study(std::ostream& output, topology::cli::Environment& environment,
                         const fs::path& study_path, bool population) -> std::expected<void, Error> {
    auto owner = ops::Directory::acquire(study_path);
    if (!owner) return std::unexpected{owner.error()};
    auto study = retained_search_study(*owner, environment, population ? SearchView::population : SearchView::replay);
    if (!study) return std::unexpected{study.error()};
    const Json report{{"schema", "epycbench.search-replay/v1"}, {"time_absent", true}, {"study", std::move(*study)}};

        const auto bytes = report.dump();
        if (bytes.size() > 16U * mib)
            return std::unexpected{problem("search_replay.capacity", "The retained replay report exceeds its bounded output size")};
        output << bytes << '\n';

    if (!output) return std::unexpected{problem("search_replay.output", "Could not write the retained replay report")};
    return {};
}
