constexpr std::string_view genetic_study_profile = "epycbench.genetic-study-plan/v1";
constexpr std::string_view hill_climb_study_profile = "epycbench.hill-climb-study-plan/v1";
constexpr std::string_view hybrid_study_profile = "epycbench.hybrid-study-plan/v1";
constexpr std::string_view bayesian_study_profile = "epycbench.bayesian-study-plan/v1";
constexpr std::string_view annealing_study_profile = "epycbench.annealing-study-plan/v1";
constexpr std::string_view population_study_profile = "epycbench.population-bound-study-plan/v1";
constexpr std::size_t genetic_report_pointer_bytes = 1024U;
enum class GeneticAction { run, status, stop };

auto bounded_genetic_rejection_pointer(std::string_view pointer) -> std::string {
    if (pointer.size() > genetic_report_pointer_bytes ||
        Json(pointer).dump().size() > genetic_report_pointer_bytes)
        return {};
    return std::string{pointer};
}

auto genetic_child_leaf(std::uint64_t generation) -> std::string {
    return "generation-" + std::to_string(generation);
}

auto genetic_child_document(const GeneticChild& child) -> Json {
    auto proposals = Json::array();
    std::size_t selected{};
    for (const auto& proposal : child.proposals) {
        auto row = Json{{"ordinal", proposal.ordinal}, {"trial_ordinal", nullptr}, {"rejection", nullptr}};
        if (proposal.rejection) {
            const auto& [code, pointer] = *proposal.rejection;
            row["rejection"] = {{"code", code}, {"pointer", pointer}};
        } else {
            row["trial_ordinal"] = selected++;
        }
        proposals.push_back(std::move(row));
    }
    return {{"study_plan_sha256", child.study_plan.to_hex()}, {"generation", child.generation},
            {"proposals", std::move(proposals)}};
}

auto decode_genetic_child(const Json& raw, const Plan& plan, StudyStrategy strategy) -> std::expected<GeneticChild, Error> {
    const auto invalid = [] {
        return std::unexpected{problem("genetic.child_plan", "Invalid frozen genetic child plan")};
    };
    auto parent = Digest::parse(raw.at("study_plan_sha256").get<std::string_view>());
    const auto& index = raw.at("generation");
    const auto& proposals = raw.at("proposals");
    if (!parent || !index.is_number_unsigned() || index.get<std::uint64_t>() >= maximum_trials ||
        !proposals.is_array() || proposals.empty() || proposals.size() != plan.schedule.attempt_budget())
        return invalid();
    GeneticChild child{*parent, index.get<std::uint64_t>(), {}, strategy};
    child.proposals.reserve(proposals.size());
    std::vector<bool> seen(maximum_trials);
    std::size_t selected{};
    for (const auto& row : proposals) {
        const auto& ordinal = row.at("ordinal");
        if (!ordinal.is_number_unsigned() || ordinal.get<std::uint64_t>() >= maximum_trials)
            return invalid();
        const auto source = ordinal.get<std::size_t>();
        if (seen[source]) return invalid();
        seen[source] = true;
        const auto& rejection = row.at("rejection");
        if (rejection.is_null()) {
            if (selected == plan.frozen.size() || row.at("trial_ordinal") != selected++) return invalid();
            child.proposals.push_back({source, {}});
        } else {
            auto code = rejection.at("code").get<std::string>();
            auto pointer = rejection.at("pointer").get<std::string>();
            if (code.empty() || code.size() > 256U || pointer.size() > 4096U) return invalid();
            child.proposals.push_back({source, std::pair{std::move(code), std::move(pointer)}});
        }
    }
    if (selected != plan.frozen.size() || genetic_child_document(child) != raw) return invalid();
    return child;
}

struct GeneticStudyPlan {
    scenario::ConfigurationSpace space;
    topology::LoadedTopology machine;
    exp::GeneticHistoryConfig history;
    std::optional<exp::ResolvedPopulation> population;
    Digest build;
    Json controller;
};

auto cooling_document(const exp::AnnealingCooling& cooling) -> Json {
    return {{"initial_temperature", cooling.initial_temperature}, {"minimum_temperature", cooling.minimum_temperature},
        {"steps_per_halving", cooling.steps_per_halving}};
}

auto genetic_study_document(scenario::ScenarioFormat format, std::string_view space,
                            std::string_view saved_topology, std::uint64_t seed, std::size_t budget,
                            exp::GeneticProposalOptions options, std::size_t maximum_generations,
                            StudyStrategy strategy, const std::optional<exp::AnnealingCooling>& cooling,
                            const Digest& build) -> Json {
    const bool hill = strategy == StudyStrategy::hill_climb;
    const bool annealing = strategy == StudyStrategy::annealing;
    const bool hybrid = strategy == StudyStrategy::hybrid;
    const bool bayesian = exp::uses_bayesian_proposals(strategy);
    auto plan = Json{{"profile", hybrid ? hybrid_study_profile : bayesian ? bayesian_study_profile : annealing ? annealing_study_profile : hill ? hill_climb_study_profile : genetic_study_profile},
            {"proposal_profile", hybrid ? exp::hybrid_proposal_profile : bayesian ? exp::bayesian_proposal_profile : annealing ? exp::annealing_profile : hill ? exp::hill_climb_proposal_profile : exp::genetic_proposal_profile},
            {"space_format", format == scenario::ScenarioFormat::yaml ? "yaml" : "json"},
            {"configuration_space", space}, {"space_sha256", core::sha256(space).to_hex()},
            {"topology", saved_topology}, {"topology_sha256", core::sha256(saved_topology).to_hex()},
            {"root_seed", seed}, {"attempt_budget", budget}, {"maximum_generations", maximum_generations}};
    if (annealing) plan["cooling"] = cooling_document(cooling.value());
    else if (hill) plan["batch_size"] = options.population;
    else if (!bayesian) {
        plan["population"] = options.population;
        plan["mutation_per_mille"] = options.mutation_per_mille;
    }
    plan["build_sha256"] = build.to_hex();
    return plan;
}

auto genetic_study_tree(std::string_view bytes) -> Json {
    return Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 64) throw std::invalid_argument{"genetic study nesting"};
        return true;
    }, false);
}

auto population_study_document(Json strategy, const exp::ResolvedPopulation& population) -> Json {
    return {{"profile", population_study_profile}, {"strategy", std::move(strategy)},
            {"population", population.document()}};
}

auto is_genetic_study_plan(std::string_view bytes) -> bool {
    if (bytes.find(genetic_study_profile) == std::string_view::npos &&
        bytes.find(hill_climb_study_profile) == std::string_view::npos &&
        bytes.find(annealing_study_profile) == std::string_view::npos &&
        bytes.find(bayesian_study_profile) == std::string_view::npos &&
        bytes.find(hybrid_study_profile) == std::string_view::npos &&
        bytes.find(population_study_profile) == std::string_view::npos) return false;
    try {
        const auto value = genetic_study_tree(bytes);
        const auto profile = value.find("profile");
        return profile != value.end() && (*profile == genetic_study_profile || *profile == hill_climb_study_profile ||
            *profile == annealing_study_profile || *profile == bayesian_study_profile ||
            *profile == hybrid_study_profile || *profile == population_study_profile);
    } catch (const std::invalid_argument&) {
        return false;
    }
}

auto decode_genetic_study(std::string_view bytes, topology::cli::Environment& environment)
    -> std::expected<GeneticStudyPlan, Error> {
    const auto invalid = [] {
        return std::unexpected{problem("genetic.study_plan", "Invalid immutable genetic study plan")};
    };
    try {
        const auto root = genetic_study_tree(bytes);
        const bool population_bound = root.at("profile") == population_study_profile;
        const auto& raw = population_bound ? root.at("strategy") : root;
        const auto& profile = raw.at("profile");
        const bool hill = profile == hill_climb_study_profile;
        const bool annealing = profile == annealing_study_profile;
        const bool bayesian = profile == bayesian_study_profile;
        const bool hybrid = profile == hybrid_study_profile;
        const bool single_proposal = annealing || bayesian || hybrid;
        if (!hill && !single_proposal && profile != genetic_study_profile) return invalid();
        const auto strategy = hybrid ? StudyStrategy::hybrid : bayesian ? StudyStrategy::bayesian :
            annealing ? StudyStrategy::annealing : hill ? StudyStrategy::hill_climb : StudyStrategy::genetic;
        const auto* batch_key = hill ? "batch_size" : "population";
        const auto format = raw.at("space_format").get<std::string_view>();
        const auto source_format = format == "yaml" ? scenario::ScenarioFormat::yaml : scenario::ScenarioFormat::json;
        const auto& source = raw.at("configuration_space").get_ref<const std::string&>();
        const auto& saved_topology = raw.at("topology").get_ref<const std::string&>();
        if ((format != "yaml" && format != "json") || source.size() > mib ||
            saved_topology.size() > maximum_plan_bytes / 2U)
            return invalid();
        for (const auto* field : {"root_seed", "attempt_budget", "maximum_generations"})
            if (!raw.at(field).is_number_unsigned()) return invalid();
        if (!single_proposal && (!raw.at(batch_key).is_number_unsigned() ||
            (!hill && !raw.at("mutation_per_mille").is_number_unsigned()))) return invalid();
        const auto seed = raw.at("root_seed").get<std::uint64_t>();
        const auto budget = raw.at("attempt_budget").get<std::uint64_t>();
        const auto population = single_proposal ? 1U : raw.at(batch_key).get<std::uint64_t>();
        const auto generations = raw.at("maximum_generations").get<std::uint64_t>();
        const auto mutation = hill || single_proposal ? 0U : raw.at("mutation_per_mille").get<std::uint64_t>();
        if (budget == 0U || budget > maximum_trials || population == 0U || population > maximum_trials ||
            generations == 0U || generations > maximum_trials || mutation > 1000U)
            return invalid();
        const exp::GeneticProposalOptions options{static_cast<std::size_t>(population),
                                                   static_cast<std::uint16_t>(mutation)};
        std::optional<exp::AnnealingCooling> cooling;
        if (annealing) {
            const auto& value = raw.at("cooling");
            for (const auto* key : {"initial_temperature", "minimum_temperature", "steps_per_halving"})
                if (!value.at(key).is_number_unsigned() || value.at(key).get<std::uint64_t>() > exp::annealing_loss_scale)
                    return invalid();
            cooling.emplace(value.at("initial_temperature").get<std::uint32_t>(),
                value.at("minimum_temperature").get<std::uint32_t>(), value.at("steps_per_halving").get<std::uint32_t>());
        }
        auto build = Digest::parse(raw.at("build_sha256").get<std::string_view>());
        if (!build) return invalid();
        auto document = genetic_study_document(source_format, source, saved_topology, seed,
            static_cast<std::size_t>(budget), options, static_cast<std::size_t>(generations), strategy,
            cooling, *build);
        const auto controller = raw.value("controller", Json(nullptr));
        if (!controller.is_null()) document["controller"] = controller;
        std::optional<exp::ResolvedPopulation> binding;
        if (population_bound) {
            auto population_bytes = core::canonicalize_json(root.at("population"));
            if (!population_bytes) return std::unexpected{std::move(population_bytes.error())};
            auto decoded = exp::ResolvedPopulation::decode(*population_bytes);
            if (!decoded) return std::unexpected{std::move(decoded.error())};
            binding.emplace(std::move(*decoded));
            document = population_study_document(std::move(document), *binding);
        }
        auto canonical = core::canonicalize_json(document);
        if (!canonical || *canonical != bytes) return invalid();
        auto space = scenario::ConfigurationSpace::parse({"genetic-study", source, source_format}, maximum_trials);
        if (!space) return std::unexpected{space.error()};
        if (space->varies_task_parameters())
            return std::unexpected{problem("genetic.problem", "Genetic search requires fixed task parameters")};
        auto axes = exp::GeneticSpace::create(space->axis_cardinalities());
        if (!axes) return std::unexpected{axes.error()};
        auto machine = environment.deserialize(saved_topology, topology::TopologyJsonInputPolicy::require_canonical);
        if (!machine) return std::unexpected{machine.error()};
        if (!controller.is_null()) {
            const auto common = space->resolve_controller({machine->topology, machine->stable_ids, machine->fingerprints});
            if (!common || controller != Json{{"core", common->core.value()}, {"cpu", common->cpu.value()}})
                return invalid();
        }
        if (binding) {
            const auto population_build = binding->required_host_build();
            if (population_build != *build) return invalid();
            const auto& value = binding->document();
            const auto& topology = value.at("topology");
            if (value.at("space") != Json{{"format", format}, {"sha256", core::sha256(source).to_hex()}} ||
                value.at("profile") != exp::package_population_profile ||
                topology.at("sha256") != core::sha256(saved_topology).to_hex() ||
                topology.at("physical") != machine->fingerprints.physical.to_string() ||
                topology.at("effective") != machine->fingerprints.effective.to_string() ||
                value.at("candidate_count") != space->size() ||
                value.at("axis_cardinalities") != space->axis_cardinalities() ||
                value.at("objective") != scenario::objective_token(space->objective()))
                return std::unexpected{problem("genetic.population_binding", "The frozen population differs from the study inputs")};
        }
        return GeneticStudyPlan{std::move(*space), std::move(*machine),
            {core::sha256(bytes), *axes, seed, static_cast<std::size_t>(budget), options,
             static_cast<std::size_t>(generations), strategy, cooling}, std::move(binding), *build, controller};
    } catch (const Json::exception&) {
        return invalid();
    } catch (const std::invalid_argument&) {
        return invalid();
    }
}

auto genetic_child_seed(const GeneticStudyPlan& study, std::uint64_t generation)
    -> std::expected<core::DerivedSeed, Error> {
    return core::derive_seed(study.history.root_seed, {core::SeedDomain::block_order,
        {{"genetic-generation", std::to_string(generation)}, {"genetic-study", study.history.plan_digest.to_hex()}}});
}

auto freeze_genetic_child(const GeneticStudyPlan& study, std::uint64_t generation, std::span<const std::size_t> proposed,
                          const kernel::packages::Catalog& catalog) -> std::expected<Plan, Error> {
    if (study.controller.is_null())
        return std::unexpected{problem("plan.controller", "New generations require a frozen controller placement")};
    auto admitted = admit_study_build(study.build);
    if (!admitted) return std::unexpected{admitted.error()};
    const scenario::ResourceResolutionContext context{study.machine.topology, study.machine.stable_ids,
                                                       study.machine.fingerprints, &catalog};
    GeneticChild child{study.history.plan_digest, generation, {}, study.history.strategy};
    std::vector<exp::CandidateIdentity> selected;
    std::vector<Json> sources;
    std::size_t authored_bytes{};
    for (const auto ordinal : proposed) {
        auto candidate = study.space.resolve(ordinal, context);
        if (!candidate) return std::unexpected{candidate.error()};
        if (study.population) {
            auto matched = study.population->verify_resolution(ordinal, *candidate);
            if (!matched) return std::unexpected{std::move(matched.error())};
        }
        if (!candidate->accepted() || !candidate->candidate_identity || !candidate->problem_identity) {
            const auto& issues = candidate->diagnostics;
            child.proposals.push_back({ordinal, issues.empty()
                ? std::pair{std::string{"experiment.plan.unresolved"}, std::string{}}
                : std::pair{issues.front().code.value(),
                            bounded_genetic_rejection_pointer(issues.front().json_pointer)}});
            continue;
        }
        if (!selected.empty() && selected.front().problem != *candidate->problem_identity)
            return std::unexpected{problem("genetic.problem", "Resolved genetic candidates describe different problems")};
        auto authored = Json(std::move(candidate->authored_scenario));
        const auto size = authored.dump().size();
        if (size > maximum_plan_bytes - authored_bytes)
            return std::unexpected{problem("plan.capacity", "Frozen Scenarios exceed the experiment plan limit")};
        authored_bytes += size;
        child.proposals.push_back({ordinal, {}});
        selected.push_back({*candidate->candidate_identity, *candidate->problem_identity});
        sources.push_back(std::move(authored));
    }
    auto seed = genetic_child_seed(study, generation);
    if (!seed) return std::unexpected{seed.error()};
    auto schedule = exp::Schedule::create(study.space.objective(), seed->value(), proposed.size(), selected);
    if (!schedule) return std::unexpected{schedule.error()};
    std::vector<FrozenTrial> frozen;
    frozen.reserve(selected.size());
    for (const auto& trial : schedule->trials()) {
        auto resolved = freeze_trial(std::move(sources[trial.ordinal]), trial.comparison_order_seed,
                                     context, study.build);
        if (!resolved) return std::unexpected{resolved.error()};
        frozen.push_back(std::move(*resolved));
    }
    return Plan{std::move(*schedule), std::move(frozen), {}, {}, study.build, std::move(child)};
}

auto genetic_ordinals(const GeneticChild& child) -> std::vector<std::size_t> {
    std::vector<std::size_t> selected;
    for (const auto& proposal : child.proposals)
        if (!proposal.rejection) selected.push_back(proposal.ordinal);
    return selected;
}

auto verify_genetic_child_plan(const GeneticStudyPlan& study, const exp::GeneticGenerationIntent& intent,
                               const Plan& plan) -> std::expected<void, Error> {
    const auto invalid = [] {
        return std::unexpected{problem("genetic.child_binding", "The child plan does not match the retained generation")};
    };
    const auto& child = plan.genetic_child;
    auto seed = genetic_child_seed(study, intent.index());
    if (!seed) return std::unexpected{seed.error()};
    if (!child || child->study_plan != study.history.plan_digest || plan.build != study.build ||
        child->strategy != study.history.strategy || child->generation != intent.index() ||
        child->proposals.size() != intent.proposal_count() ||
        plan.schedule.objective() != study.space.objective() || plan.schedule.seed() != seed->value() ||
        genetic_ordinals(*child) != intent.scheduled_ordinals)
        return invalid();
    std::size_t trial{};
    for (std::size_t i{}; i < child->proposals.size(); ++i) {
        const auto& proposal = child->proposals[i];
        if (proposal.ordinal != intent.proposal_ordinal(i)) return invalid();
        if (study.population) {
            const auto& entries = study.population->document().at("entries");
            if (proposal.ordinal >= entries.size()) return invalid();
            const auto& entry = entries.at(proposal.ordinal);
            if (proposal.rejection) {
                const auto& [code, pointer] = *proposal.rejection;
                const auto expected = Json{{"code", code}, {"pointer", pointer}};
                if (entry.at("state") != "rejected" || entry.at("rejection") != expected) return invalid();
            } else {
                if (entry.at("state") != "resolved" || trial >= plan.schedule.trials().size()) return invalid();
                const auto& identity = plan.schedule.trials()[trial].configuration;
                if (entry.at("candidate_sha256") != identity.candidate.to_hex() ||
                    entry.at("problem_sha256") != identity.problem.to_hex()) return invalid();
            }
        }
        if (proposal.rejection) continue;
        auto source = study.space.materialize(proposal.ordinal);
        if (!source || trial >= plan.frozen.size()) return invalid();
        if (plan.frozen[trial].controller != study.controller ||
            !matches_recipe_trial(std::move(*source), plan.frozen[trial],
                                  plan.schedule.trials()[trial].comparison_order_seed)) return invalid();
        ++trial;
    }
    if (trial != plan.frozen.size()) return invalid();
    return {};
}

auto native_error_is(const Error& error, int value) noexcept -> bool {
    return error.os_error && error.os_error->value == value;
}

auto install_genetic_child(const ops::Directory& parent, std::uint64_t generation, std::string_view bytes)
    -> std::expected<void, Error> {
    const auto leaf = genetic_child_leaf(generation);
    auto created = parent.create_subdirectory(leaf);
    if (!created && !native_error_is(created.error(), EEXIST)) return std::unexpected{created.error()};
    auto child = ops::Directory::acquire(parent.path() / leaf);
    if (!child) return std::unexpected{child.error()};
    const auto files = child->list(1U);
    if (!files) return std::unexpected{files.error()};
    if (files->empty()) return child->write_new(plan_file, bytes, maximum_plan_bytes);
    if (files->front() == plan_file) {
        const auto previous = child->read(plan_file, maximum_plan_bytes);
        if (!previous) return std::unexpected{previous.error()};
        if (*previous == bytes) return {};
    }
    return std::unexpected{problem("genetic.child_exists", "Existing child content differs from the frozen generation")};
}

struct GeneticChildData {
    Plan plan;
    exp::Ledger ledger;
    exp::LedgerStore store;
};

auto load_genetic_child(const ops::Directory& child, const GeneticStudyPlan& study,
                        const exp::GeneticGenerationIntent& intent) -> std::expected<GeneticChildData, Error> {
    const auto bytes = child.read(plan_file, maximum_plan_bytes);
    if (!bytes) return std::unexpected{bytes.error()};
    if (core::sha256(*bytes) != intent.child_plan)
        return std::unexpected{problem("genetic.child_digest", "Child plan bytes differ from the retained digest")};
    auto plan = decode_plan(*bytes);
    if (!plan) return std::unexpected{plan.error()};
    auto matched = verify_genetic_child_plan(study, intent, *plan);
    if (!matched) return std::unexpected{matched.error()};
    exp::Ledger ledger{plan->schedule};
    auto store = exp::LedgerStore::open(child, intent.child_plan.to_hex(), ledger);
    if (!store) return std::unexpected{store.error()};
    return GeneticChildData{std::move(*plan), std::move(ledger), std::move(*store)};
}

struct FrozenGeneration {
    exp::GeneticGenerationIntent intent;
    std::string child_plan;
};

using StudyBatch = std::variant<exp::GeneticGeneration, exp::HillClimbBatch, exp::AnnealingStep, exp::BayesianStep>;

auto next_study_batch(const exp::GeneticHistory& history, StudyStrategy strategy)
    -> std::expected<std::optional<StudyBatch>, Error> {
    if (exp::uses_bayesian_proposals(strategy)) {
        auto step = history.next_bayesian();
        if (!step) return std::unexpected{step.error()};
        return *step ? std::optional{StudyBatch{std::move(**step)}} : std::nullopt;
    }
    if (strategy == StudyStrategy::annealing) {
        auto step = history.next_annealing();
        if (!step) return std::unexpected{step.error()};
        return *step ? std::optional{StudyBatch{std::move(**step)}} : std::nullopt;
    }
    if (strategy == StudyStrategy::hill_climb) {
        auto batch = history.next_hill_climb();
        if (!batch) return std::unexpected{batch.error()};
        return *batch ? std::optional{StudyBatch{std::move(**batch)}} : std::nullopt;
    }
    auto batch = history.next();
    if (!batch) return std::unexpected{batch.error()};
    return *batch ? std::optional{StudyBatch{std::move(**batch)}} : std::nullopt;
}

auto freeze_genetic_generation(const GeneticStudyPlan& study, StudyBatch batch,
                               const kernel::packages::Catalog& catalog)
    -> std::expected<FrozenGeneration, Error> {
    return std::visit([&]<class Batch>(Batch& generation) -> std::expected<FrozenGeneration, Error> {
        constexpr bool genetic = std::is_same_v<Batch, exp::GeneticGeneration>;
        constexpr bool annealing = std::is_same_v<Batch, exp::AnnealingStep>;
        constexpr bool bayesian = std::is_same_v<Batch, exp::BayesianStep>;
        const auto index = [&] {
            if constexpr (genetic) return generation.generation;
            else return generation.step;
        }();
        std::vector<std::size_t> proposed;
        if constexpr (annealing || bayesian) {
            proposed.push_back(generation.proposal.value().ordinal);
        } else {
            proposed.reserve(generation.proposals.size());
            for (const auto& proposal : generation.proposals) proposed.push_back(proposal.ordinal);
        }
        auto plan = freeze_genetic_child(study, index, proposed, catalog);
        if (!plan) return std::unexpected{plan.error()};
        auto bytes = document(*plan).dump();
        if (bytes.size() > maximum_plan_bytes)
            return std::unexpected{problem("plan.capacity", "The frozen child plan exceeds its bound")};
        const auto reopened = decode_plan(bytes);
        if (!reopened) return std::unexpected{reopened.error()};
        auto scheduled = genetic_ordinals(*plan->genetic_child);
        const auto digest = core::sha256(bytes);
        if constexpr (bayesian) {
            auto intent = exp::GeneticGenerationIntent::for_bayesian(std::move(generation), digest, std::move(scheduled));
            return FrozenGeneration{std::move(intent), std::move(bytes)};
        } else if constexpr (annealing) {
            auto intent = exp::GeneticGenerationIntent::for_annealing(std::move(generation), digest, std::move(scheduled));
            return FrozenGeneration{std::move(intent), std::move(bytes)};
        } else if constexpr (!genetic) {
            auto intent = exp::GeneticGenerationIntent::for_hill_climb(std::move(generation), digest, std::move(scheduled));
            return FrozenGeneration{std::move(intent), std::move(bytes)};
        } else {
            return FrozenGeneration{{std::move(generation), digest, std::move(scheduled)}, std::move(bytes)};
        }
    }, batch);
}

auto publish_genetic_generation(const ops::Directory& parent, exp::GeneticHistory& history, FrozenGeneration frozen)
    -> std::expected<void, Error> {
    // An unpublished child has no dispatched attempts. Publish the durable
    // intent only after securing the exact child plan.
    auto saved = install_genetic_child(parent, frozen.intent.index(), frozen.child_plan);
    if (!saved) return saved;
    return history.begin(std::move(frozen.intent));
}

struct GeneticCohort {
    Digest build;
    Digest problem;
    std::string physical;
    std::string effective;
    auto operator==(const GeneticCohort&) const -> bool = default;
};

auto genetic_cohort_document(const GeneticCohort& cohort) -> Json {
    return {{"build_sha256", cohort.build.to_hex()}, {"problem_sha256", cohort.problem.to_hex()},
            {"physical_topology", cohort.physical}, {"effective_topology", cohort.effective}};
}

struct GeneticChildAssessment {
    exp::GeneticGenerationCompletion completion;
    std::optional<GeneticCohort> cohort;
    Json report;
};

auto describe_genetic_child(const exp::GeneticGenerationIntent& intent,
                            const GeneticChildData& child) -> Json {
    const auto& ledger = child.ledger;
    auto rows = Json::array();
    std::size_t trial{};
    for (std::size_t i{}; i < intent.proposal_count(); ++i) {
        const auto& proposal = child.plan.genetic_child->proposals[i];
        auto row = Json{{"ordinal", proposal.ordinal}, {"trial_ordinal", nullptr}};
        if (intent.bayesian) {
            const auto& lineage = intent.bayesian->proposal.value();
            row["origin"] = lineage.origin == exp::BayesianProposalOrigin::local ? "local" :
                lineage.origin == exp::BayesianProposalOrigin::surrogate ? "surrogate" : "exploration";
            if (lineage.parent) row["parent_ordinal"] = *lineage.parent;
            if (const auto& change = lineage.change)
                row["axis_change"] = {{"axis", change->axis}, {"before", change->before}, {"after", change->after}};
            row["elite_probability_units"] = lineage.elite_probability_units ? Json(*lineage.elite_probability_units) : Json(nullptr);
        } else if (intent.hill_climb || intent.annealing) {
            const auto& lineage = intent.annealing ? intent.annealing->proposal.value() : intent.hill_climb->proposals[i];
            constexpr std::array origins{"initial", "neighbor", "restart"};
            row["origin"] = origins.at(static_cast<std::size_t>(lineage.origin));
            if (const auto& change = lineage.change)
                row["axis_change"] = {{"axis", change->axis}, {"before", change->before}, {"after", change->after}};
        } else {
            const auto& lineage = intent.generation.proposals[i];
            constexpr std::array origins{"initial", "offspring", "exploration"};
            row["origin"] = origins.at(static_cast<std::size_t>(lineage.origin));
            if (const auto& crossover = lineage.crossover)
                row["crossover"] = {{"first_parent", crossover->first_parent},
                    {"second_parent", crossover->second_parent},
                    {"second_parent_axes", crossover->second_parent_axes}, {"ordinal", crossover->ordinal}};
            if (const auto& mutation = lineage.mutation)
                row["mutation"] = {{"axis", mutation->axis}, {"before", mutation->before}, {"after", mutation->after}};
        }
        if (proposal.rejection) {
            const auto& [code, pointer] = *proposal.rejection;
            row["state"] = "rejected";
            row["rejection"] = {{"code", code}, {"pointer", bounded_genetic_rejection_pointer(pointer)}};
        } else {
            const auto& scheduled = ledger.schedule().trials()[trial];
            row["trial_ordinal"] = trial++;
            row["trial_sha256"] = scheduled.id.to_hex();
            row["candidate_sha256"] = scheduled.configuration.candidate.to_hex();
            row["problem_sha256"] = scheduled.configuration.problem.to_hex();
            const auto* outcome = trial_outcome(ledger, scheduled.id);
            row["state"] = outcome ? disposition_names.at(static_cast<std::size_t>(outcome->disposition)) :
                ledger.active() == &scheduled ? "unresolved_reservation" :
                ledger.scientifically_stopped() ? "scientifically_stopped" : "pending";
            if (outcome && outcome->result) {
                const auto& result = *outcome->result;
                row["result"] = {{"run_id", result.run_id}, {"content_sha256", result.content_digest.to_hex()},
                   {"artifact", (fs::path("trial-" + std::to_string(scheduled.ordinal)) / result.artifact).string()}};
            } else if (outcome && outcome->unavailable) {
                constexpr std::array reasons{"run_failed", "invalid_result", "result_missing", "interrupted"};
                row["result_unavailable"] = reasons.at(static_cast<std::size_t>(*outcome->unavailable));
            }
        }
        rows.push_back(std::move(row));
    }
    auto report = Json{{"generation", intent.index()}, {"directory", genetic_child_leaf(intent.index())},
            {"child_plan_sha256", intent.child_plan.to_hex()},
            {"child_ledger_head_sha256", child.store.head() ? Json(child.store.head()->to_hex()) : Json(nullptr)},
            {"scheduled", intent.scheduled_ordinals.size()}, {"charged", ledger.charged_attempts()},
            {"scientific_stop", ledger.scientifically_stopped()}, {"proposals", std::move(rows)}};
    if (const auto& hill = intent.hill_climb) {
        report["incumbent_before"] = hill->incumbent ? Json(hill->incumbent->ordinal) : Json(nullptr);
        report["unseen_neighbors"] = hill->unseen_neighbors;
    } else if (const auto& annealing = intent.annealing) {
        report["incumbent_before"] = annealing->incumbent_ordinal ? Json(*annealing->incumbent_ordinal) : Json(nullptr);
        report["temperature"] = annealing->temperature;
    } else if (const auto& bayesian = intent.bayesian) {
        report["verified_count"] = bayesian->verified_count;
        report["elite_count"] = bayesian->elite_count;
    } else {
        report["offspring_draws"] = intent.generation.offspring_draws;
        report["duplicate_offspring"] = intent.generation.duplicate_offspring;
    }
    return report;
}

auto assess_genetic_child(const ops::Directory& directory, const GeneticStudyPlan& study,
                          const exp::GeneticGenerationIntent& intent, const GeneticChildData& child,
                          std::optional<GeneticCohort> cohort, DiscoveryTradeoffs* tradeoffs = nullptr)
    -> std::expected<GeneticChildAssessment, Error> {
    const auto& ledger = child.ledger;
    if (ledger.active() || ledger.next())
        return std::unexpected{problem("genetic.child_incomplete", "The ordinary child ledger is still resumable")};
    GeneticChildAssessment result{
        {intent.index(), intent.child_plan, child.store.head(), ledger.charged_attempts(),
         {}, ledger.scientifically_stopped()},
        std::move(cohort), describe_genetic_child(intent, child)};
    struct Scored {
        std::size_t row;
        double throughput;
        double objective_value;
        exp::GeneticParentResult reference;
    };
    std::vector<Scored> scored;
    const auto physical = study.machine.fingerprints.physical.to_string();
    const auto effective = study.machine.fingerprints.effective.to_string();
    const auto bound_build = study.population ? study.population->required_host_build() : std::nullopt;
    const bool maximize = study.space.objective() == Objective::throughput;
    auto& rows = result.report.at("proposals");
    for (std::size_t i{}; i < rows.size(); ++i) {
        auto& row = rows[i];
        if (row.at("trial_ordinal").is_null()) continue;
        const auto trial_index = row.at("trial_ordinal").get<std::size_t>();
        const auto& trial = ledger.schedule().trials()[trial_index];
        const auto* outcome = trial_outcome(ledger, trial.id);
        if (ledger.scientifically_stopped() || !outcome || outcome->disposition != exp::Disposition::completed) {
            row["parent_status"] = ledger.scientifically_stopped() ? "scientific_stop" : "outcome_ineligible";
            continue;
        }
        auto evidence = verified_completed(directory, trial_index, child.plan.frozen[trial_index], *outcome);
        if (!evidence) {
            row["parent_status"] = "result_unverified";
            row["verification_code"] = evidence.error().code.value();
            continue;
        }
        if (!evidence->discovery_eligible) {
            row["parent_status"] = "supplied_evidence";
            continue;
        }
        if (!evidence->observed_inputs) {
            row["parent_status"] = "observed_inputs_unavailable";
            continue;
        }
        if (!evidence->build || !evidence->physical_topology || !evidence->effective_topology ||
            (!evidence->whole_group && evidence->scaling.size() != 1U)) {
            row["parent_status"] = "comparison_evidence_missing";
            continue;
        }
        if (*evidence->physical_topology != physical || *evidence->effective_topology != effective) {
            row["parent_status"] = "topology_mismatch";
            continue;
        }
        if (bound_build && evidence->build != bound_build) {
            row["parent_status"] = "population_build_mismatch";
            continue;
        }
        auto sample = tradeoff_sample(trial, *evidence);
        if (!sample) {
            row["parent_status"] = "task_metric_unavailable";
            continue;
        }
        const auto throughput = sample->tasks_per_second;
        const auto objective_value = maximize ? throughput : 1.0 / throughput;
        if (!std::isfinite(objective_value) || objective_value <= 0.0) {
            row["parent_status"] = "task_metric_unavailable";
            continue;
        }
        const GeneticCohort observed{*evidence->build, sample->problem, physical, effective};
        if (evidence->observed_inputs) {
            row["measured_problem_sha256"] = observed.problem.to_hex();
            row["observed_inputs_sha256"] = evidence->observed_inputs->to_hex();
        }
        if (evidence->calibrated_repetitions) row["calibrated_repetitions"] = *evidence->calibrated_repetitions;
        row["cohort"] = genetic_cohort_document(observed);
        if (result.cohort && *result.cohort != observed) {
            row["parent_status"] = "incompatible_cohort";
            continue;
        }
        if (evidence->whole_group) row["performance_unit"] = "composite_tasks_per_second";
        if (tradeoffs) tradeoffs->emplace(trial.id.to_hex(), std::move(*sample));
        result.cohort = observed;
        scored.push_back({i, throughput, objective_value,
                          {intent.scheduled_ordinals[trial_index], 0U, trial.id, std::move(evidence->association),
                           intent.hill_climb || intent.annealing || intent.bayesian ? std::optional{throughput} : std::nullopt}});
    }
    std::ranges::sort(scored, [maximize](const Scored& a, const Scored& b) {
        return maximize ? a.objective_value > b.objective_value : a.objective_value < b.objective_value;
    });
    for (std::size_t i{}; i < scored.size(); ++i)
        scored[i].reference.rank = i != 0U && scored[i].objective_value == scored[i - 1U].objective_value
            ? scored[i - 1U].reference.rank : i;
    std::ranges::sort(scored, {}, &Scored::row);
    for (auto& score : scored) {
        auto& row = rows[score.row];
        row["parent_status"] = "eligible";
        row["rank"] = score.reference.rank;
        row["candidate_tasks_per_second"] = score.throughput;
        row["objective_value"] = score.objective_value;
        result.completion.eligible_parents.push_back(std::move(score.reference));
    }
    return result;
}

auto same_genetic_completion(const exp::GeneticGenerationCompletion& a,
                             const exp::GeneticGenerationCompletion& b) -> bool {
    return a.generation == b.generation && a.child_plan == b.child_plan &&
        a.child_ledger_head == b.child_ledger_head && a.charged_attempts == b.charged_attempts &&
        a.scientific_stop == b.scientific_stop &&
        std::ranges::equal(a.eligible_parents, b.eligible_parents, [](const auto& first, const auto& second) {
            return first.ordinal == second.ordinal && first.rank == second.rank && first.trial == second.trial &&
                first.result.run_id == second.result.run_id && first.result.artifact == second.result.artifact &&
                first.result.content_digest == second.result.content_digest && first.throughput == second.throughput;
        });
}

struct GeneticStudyVerification {
    const ops::Directory& directory;
    const GeneticStudyPlan& study;
    std::optional<GeneticCohort> cohort{};
    Json completed = Json::array();
    DiscoveryTradeoffs* tradeoffs{};
};

auto verify_genetic_completion(void* context, const exp::GeneticGenerationIntent& intent,
                               const exp::GeneticGenerationCompletion& completion) -> std::expected<void, Error> {
    auto& state = *static_cast<GeneticStudyVerification*>(context);
    auto directory = ops::Directory::acquire(state.directory.path() / genetic_child_leaf(intent.index()));
    if (!directory) return std::unexpected{directory.error()};
    auto child = load_genetic_child(*directory, state.study, intent);
    if (!child) return std::unexpected{child.error()};
    auto actual = assess_genetic_child(*directory, state.study, intent, *child, state.cohort, state.tradeoffs);
    if (!actual) return std::unexpected{actual.error()};
    if (!same_genetic_completion(actual->completion, completion))
        return std::unexpected{problem("genetic.completion_binding", "Child ledger or eligible Results changed")};
    if (const auto& decision = completion.annealing_decision)
        actual->report["annealing_decision"] = {{"accepted", decision->accepted}, {"loss_units", decision->loss_units},
            {"threshold", decision->threshold}, {"draw", decision->draw ? Json(*decision->draw) : Json(nullptr)}};
    state.cohort = std::move(actual->cohort);
    state.completed.push_back(std::move(actual->report));
    return {};
}

struct GeneticConfirmationDiscovery {
    GeneticStudyPlan study;
    exp::GeneticGenerationIntent intent;
    Digest ledger_head;
};

auto finished_genetic_generation(topology::cli::Environment& environment, const ops::Directory& owner,
                                 const Digest& study_digest, std::uint64_t generation)
    -> std::expected<GeneticConfirmationDiscovery, Error> {
    const auto bytes = owner.read(plan_file, maximum_plan_bytes);
    if (!bytes) return std::unexpected{bytes.error()};
    if (core::sha256(*bytes) != study_digest)
        return std::unexpected{problem("confirmation.genetic_study", "The genetic study does not match its retained digest")};
    auto study = decode_genetic_study(*bytes, environment);
    if (!study) return std::unexpected{study.error()};
    GeneticStudyVerification verification{owner, *study};
    auto history = exp::GeneticHistory::open(owner, study->history, {&verification, verify_genetic_completion});
    if (!history) return std::unexpected{history.error()};
    const auto unavailable = [] {
        return std::unexpected{problem("confirmation.genetic_study", "Confirmation requires a finished, unstopped genetic study")};
    };
    if (history->pending() || history->scientifically_stopped()) return unavailable();
    const auto next = next_study_batch(*history, study->history.strategy);
    if (!next) return std::unexpected{next.error()};
    if (*next) return unavailable();
    const auto completed = history->completed();
    if (generation >= completed.size() || !completed[generation].completion.child_ledger_head)
        return std::unexpected{problem("confirmation.genetic_generation", "The requested generation has no completed trial ledger")};
    const auto& selected = completed[generation];
    return GeneticConfirmationDiscovery{std::move(*study), selected.intent, *selected.completion.child_ledger_head};
}

auto make_genetic_confirmation(std::ostream& output, topology::cli::Environment& environment,
                               const fs::path& topology_file, const std::optional<fs::path>& module_directory,
                               const ops::Directory& owner, const fs::path& path, const GeneticChild& probe,
                               const Digest& generation_digest, std::size_t ordinal, std::uint64_t seed)
    -> std::expected<void, Error> {
    if (path.filename() != genetic_child_leaf(probe.generation))
        return std::unexpected{problem("confirmation.location", "Keep the generation in its owning genetic study")};
    auto discovery = finished_genetic_generation(environment, owner, probe.study_plan, probe.generation);
    if (!discovery) return std::unexpected{discovery.error()};
    if (discovery->intent.child_plan != generation_digest)
        return std::unexpected{problem("confirmation.genetic_generation", "The generation differs from the retained plan")};
    auto generation = ops::Directory::acquire(path);
    if (!generation) return std::unexpected{generation.error()};
    auto source = load_genetic_child(*generation, discovery->study, discovery->intent);
    if (!source) return std::unexpected{source.error()};
    if (source->store.head() != discovery->ledger_head || ordinal >= discovery->intent.scheduled_ordinals.size())
        return std::unexpected{problem("confirmation.genetic_trial", "Choose a completed trial ordinal from the retained generation")};
    const auto& study = discovery->study;
    const GeneticConfirmationSource genetic{{probe.study_plan, probe.generation, discovery->ledger_head, study.history.strategy},
        study.space, study.machine.fingerprints, discovery->intent.scheduled_ordinals[ordinal], study.history.root_seed};
    auto confirmation = make_confirmation(environment, topology_file, module_directory, *generation,
        generation_digest, source->plan, source->ledger, ordinal, seed, &genetic);
    if (!confirmation) return std::unexpected{confirmation.error()};
    auto child = confirmation_directory(*generation, source->ledger.schedule().trials().size());
    if (!child) return std::unexpected{child.error()};
    return initial_status(output, *child, std::move(*confirmation));
}

auto verify_genetic_confirmation_origin(const ops::Directory& generation,
                                        const GeneticStudyPlan& study, const exp::GeneticGenerationIntent& intent,
                                        const Digest& ledger_head, const Plan& confirmation)
    -> std::expected<void, Error> {
    const auto& origin = *confirmation.confirmation;
    if (study.build != origin.discovery_build)
        return std::unexpected{problem("confirmation.build", "The original discovery Build differs from the frozen study")};
    if (intent.child_plan != origin.discovery_plan ||
        ledger_head != origin.genetic->generation_ledger_head ||
        study.history.strategy != origin.genetic->strategy ||
        confirmation.schedule.seed() == study.history.root_seed ||
        origin.discovery_trial_ordinal >= intent.scheduled_ordinals.size())
        return std::unexpected{problem("confirmation.genetic_origin", "The confirmation lineage differs from the completed genetic study")};
    auto source = load_genetic_child(generation, study, intent);
    if (!source) return std::unexpected{source.error()};
    if (source->store.head() != ledger_head)
        return std::unexpected{problem("confirmation.genetic_generation", "The completed generation ledger has changed")};
    const auto candidate_ordinal = intent.scheduled_ordinals[origin.discovery_trial_ordinal];
    auto evidence = verify_confirmation_trial(generation, source->plan, source->ledger, candidate_ordinal, confirmation);
    if (!evidence) return std::unexpected{evidence.error()};
    if (!matches_topology(*evidence, study.machine.fingerprints))
        return std::unexpected{problem("confirmation.topology", "The discovery Result differs from the genetic study topology")};
    return {};
}

auto verify_genetic_confirmation_origin(const ops::Directory& generation,
                                        const GeneticConfirmationDiscovery& discovery, const Plan& confirmation)
    -> std::expected<void, Error> {
    return verify_genetic_confirmation_origin(generation, discovery.study, discovery.intent,
                                               discovery.ledger_head, confirmation);
}

auto genetic_status(std::ostream& output, const GeneticStudyVerification& verification,
                    const exp::GeneticHistory& history) -> std::expected<void, Error> {
    const auto& study = verification.study;
    const auto& config = study.history;
    const bool hill = config.strategy == StudyStrategy::hill_climb;
    const bool annealing = config.strategy == StudyStrategy::annealing;
    const bool bayesian = exp::uses_bayesian_proposals(config.strategy);
    auto pending = Json(nullptr);
    auto charged = history.charged_attempts();
    std::string_view state;
    if (const auto* intent = history.pending()) {
        const auto path = verification.directory.path() / genetic_child_leaf(intent->index());
        auto directory = ops::Directory::acquire(path);
        if (!directory) return std::unexpected{directory.error()};
        auto child = load_genetic_child(*directory, study, *intent);
        if (!child) return std::unexpected{child.error()};
        pending = describe_genetic_child(*intent, *child);
        charged += child->ledger.charged_attempts();
        state = "pending";
    } else if (history.scientifically_stopped()) {
        state = "scientifically_stopped";
    } else {
        auto next = next_study_batch(history, config.strategy);
        if (!next) return std::unexpected{next.error()};
        state = *next ? "ready" : "exhausted";
    }
    auto report = Json{{"profile", config.strategy == StudyStrategy::hybrid ? "epycbench.hybrid-study-status/v1" :
        bayesian ? "epycbench.bayesian-study-status/v1" :
        annealing ? "epycbench.annealing-study-status/v1" : hill ?
            "epycbench.hill-climb-study-status/v1" : "epycbench.genetic-study-status/v1"},
        {"plan_sha256", config.plan_digest.to_hex()}, {"state", state},
        {"objective", scenario::objective_token(study.space.objective())},
        {"attempt_budget", config.attempt_budget}, {"charged", charged},
        {"maximum_generations", config.maximum_generations},
        {"cohort", verification.cohort ? genetic_cohort_document(*verification.cohort) : Json(nullptr)},
        {"completed_generations", verification.completed}, {"pending_generation", std::move(pending)}};
    if (!study.controller.is_null()) report["controller"] = study.controller;
    if (study.population) report["population_sha256"] = study.population->digest().to_hex();
    const auto score_document = [](const auto& score) -> Json {
        return score ? Json{{"ordinal", score->ordinal}, {"tasks_per_second", score->throughput}} : Json(nullptr);
    };
    if (bayesian) {
        report["best_verified"] = score_document(history.bayesian_best());
    } else if (annealing) {
        report["cooling"] = cooling_document(config.annealing_cooling.value());
        report["incumbent"] = score_document(history.annealing_incumbent());
        report["best_verified"] = score_document(history.annealing_best());
    } else if (hill) {
        report["batch_size"] = config.options.population;
        report["incumbent"] = score_document(history.hill_climb_incumbent());
    } else {
        report["population"] = config.options.population;
        report["mutation_per_mille"] = config.options.mutation_per_mille;
    }
    const auto bytes = report.dump();
    if (bytes.size() > maximum_plan_bytes)
        return std::unexpected{problem("genetic.status_capacity", "The genetic status report exceeds its bound")};
    output << bytes << '\n';
    return {};
}

auto run_pending_genetic_child(GeneticAction action, topology::cli::Environment& environment,
                               const GeneticStudyVerification& verification,
                               const exp::GeneticGenerationIntent& intent,
                               const std::optional<fs::path>& module_directory, SignalScope* signals, bool one_attempt,
                               const SuppliedExperimentWorkers* supplied)
    -> std::expected<std::optional<exp::GeneticGenerationCompletion>, Error> {
    const auto path = verification.directory.path() / genetic_child_leaf(intent.index());
    auto directory = ops::Directory::acquire(path);
    if (!directory) return std::unexpected{directory.error()};
    auto child = load_genetic_child(*directory, verification.study, intent);
    if (!child) return std::unexpected{child.error()};
    Evaluation evaluation{*directory, child->plan.frozen, module_directory, child->store, signals,
                          supplied ? supplied->file : std::nullopt};
    const exp::Operations operations{&evaluation, persist, evaluate, cancelled};
    const bool recovered_active = child->ledger.active() != nullptr;
    if (recovered_active || (action == GeneticAction::run && child->ledger.next())) {
        auto admitted = admit_study_build(child->plan.build);
        if (!admitted) return std::unexpected{admitted.error()};
    }
    if (recovered_active) {
        // Directory admission confirms retirement of any inherited Run lease.
        auto recovered = exp::recover_retired_trial(child->ledger, operations);
        if (!recovered) return std::unexpected{recovered.error()};
    }
    if (action == GeneticAction::run && (!one_attempt || !recovered_active)) {
        auto admitted = admit_pending_trials(environment, child->ledger, child->plan.frozen, module_directory,
                                              supplied, one_attempt);
        if (!admitted) return std::unexpected{std::move(admitted.error())};
    }
    if (!child->ledger.scientifically_stopped() && (!one_attempt || !recovered_active)) {
        const auto extent = one_attempt ? exp::RunExtent::one_attempt : exp::RunExtent::remaining;
        auto executed = action == GeneticAction::stop
            ? exp::record_scientific_stop(child->ledger, operations) : exp::run(child->ledger, operations, extent);
        if (!executed) return std::unexpected{executed.error()};
    }
    if (child->ledger.next()) return std::optional<exp::GeneticGenerationCompletion>{};
    auto result = assess_genetic_child(*directory, verification.study, intent, *child, verification.cohort);
    if (!result) return std::unexpected{result.error()};
    return std::optional{std::move(result->completion)};
}

auto run_genetic_study(GeneticAction action, std::ostream& output, topology::cli::Environment& environment,
                       const ops::Directory& directory, const std::optional<fs::path>& module_directory,
                       std::string_view bytes, bool one_attempt, const SuppliedExperimentWorkers* supplied,
                       SignalScope* active_signals) -> std::expected<int, Error> {
    auto study = decode_genetic_study(bytes, environment);
    if (!study) return std::unexpected{study.error()};
    GeneticStudyVerification verification{directory, *study};
    auto history = exp::GeneticHistory::open(directory, study->history, {&verification, verify_genetic_completion});
    if (!history) return std::unexpected{history.error()};

    ExperimentSignals signals(active_signals);
    const bool running = action == GeneticAction::run;
    if (running && !signals.acquire())
        return std::unexpected{problem("signals", "Cannot acquire coordinator signal ownership")};
    if (action != GeneticAction::status) {
        while (!history->scientifically_stopped() && signals.scope().observe() == ControllerSignal::none) {
            if (!history->pending()) {
                if (action == GeneticAction::stop) {
                    auto stopped = history->stop_scientifically();
                    if (!stopped) return std::unexpected{stopped.error()};
                    break;
                }
                auto next = next_study_batch(*history, study->history.strategy);
                if (!next) return std::unexpected{next.error()};
                if (!*next) break;
                const auto catalog = runtime::discover_packages(module_directory);
                if (!catalog) return std::unexpected{catalog.error()};
                auto frozen = freeze_genetic_generation(*study, std::move(**next), *catalog);
                if (!frozen) return std::unexpected{frozen.error()};
                auto prepared = publish_genetic_generation(directory, *history, std::move(*frozen));
                if (!prepared) return std::unexpected{prepared.error()};
            }
            auto completion = run_pending_genetic_child(action, environment, verification, *history->pending(), module_directory,
                                                        running ? &signals.scope() : nullptr, one_attempt, supplied);
            if (!completion) return std::unexpected{completion.error()};
            if (!*completion) break;
            auto completed = history->complete(std::move(**completion));
            if (!completed) return std::unexpected{completed.error()};
            if (one_attempt) break;
        }
    }
    auto signal = ControllerSignal::none;
    if (running) {
        auto closed = signals.finish();
        if (!closed) return std::unexpected{problem("signals", "Cannot restore coordinator signals")};
        signal = *closed;
    }
    auto reported = genetic_status(output, verification, *history);
    if (!reported) return std::unexpected{reported.error()};
    if (signal != ControllerSignal::none) return signal == ControllerSignal::interrupt ? 130 : 143;
    return output ? 0 : 3;
}

auto write_genetic_study(std::ostream& output, topology::cli::Environment& environment,
                         const ops::Directory& directory, const fs::path& space_file,
                         const fs::path& topology_file, std::uint64_t seed, std::size_t budget,
                         exp::GeneticProposalOptions options, std::size_t maximum_generations,
                         const std::optional<fs::path>& module_directory, StudyStrategy strategy,
                         const std::optional<exp::AnnealingCooling>& cooling, bool bind_population,
                         const ReviewedPlanInputs* reviewed)
    -> std::expected<void, Error> {
    const auto sources = read_plan_sources(environment, space_file, topology_file, reviewed);
    if (!sources) return std::unexpected{sources.error()};
    if (sources->recipe.size() > mib || sources->topology.size() > maximum_plan_bytes / 2U)
        return std::unexpected{problem("genetic.plan_capacity", "Genetic study inputs exceed their size bound")};
    const scenario::ScenarioSource source{"genetic-study", sources->recipe, sources->format};
    const auto space = scenario::ConfigurationSpace::parse(source, maximum_trials);
    if (!space) return std::unexpected{space.error()};
    const auto build = current_study_build();
    if (!build) return std::unexpected{build.error()};
    auto document = genetic_study_document(sources->format, sources->recipe, sources->topology, seed, budget,
                                           options, maximum_generations, strategy, cooling, *build);
    auto machine = environment.deserialize(sources->topology, topology::TopologyJsonInputPolicy::require_canonical);
    if (!machine) return std::unexpected{machine.error()};
    const auto common = space->resolve_controller({machine->topology, machine->stable_ids, machine->fingerprints});
    if (!common) return std::unexpected{common.error()};
    document["controller"] = {{"core", common->core.value()}, {"cpu", common->cpu.value()}};
    const auto catalog = read_plan_catalog(*space, module_directory, reviewed);
    if (!catalog) return std::unexpected{catalog.error()};
    if (bind_population) {
        auto population = exp::ResolvedPopulation::resolve(source, *machine, current_study_build, &*catalog);
        if (!population) return std::unexpected{std::move(population.error())};
        document = population_study_document(std::move(document), *population);
    }
    auto bytes = core::canonicalize_json(document);
    if (!bytes) return std::unexpected{bytes.error()};
    if (bytes->size() > maximum_plan_bytes)
        return std::unexpected{problem("genetic.plan_capacity", "The managed plan exceeds its size bound")};
    auto study = decode_genetic_study(*bytes, environment);
    if (!study) return std::unexpected{study.error()};
    GeneticStudyVerification verification{directory, *study};
    auto history = exp::GeneticHistory::open(directory, study->history, {&verification, verify_genetic_completion});
    if (!history) return std::unexpected{history.error()};
    auto next = next_study_batch(*history, strategy);
    if (!next) return std::unexpected{next.error()};
    if (!*next)
        return std::unexpected{problem("genetic.empty_space", "No genetic proposals are available")};
    auto frozen = freeze_genetic_generation(*study, std::move(**next), *catalog);
    if (!frozen) return std::unexpected{frozen.error()};
    auto saved = directory.write_new(plan_file, *bytes, maximum_plan_bytes);
    if (!saved) return saved;
    auto begun = publish_genetic_generation(directory, *history, std::move(*frozen));
    if (!begun) return begun;
    return genetic_status(output, verification, *history);
}
