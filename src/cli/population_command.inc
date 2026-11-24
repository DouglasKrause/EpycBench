auto freeze_population(std::ostream& output, topology::cli::Environment& environment,
                       const fs::path& space_file, const fs::path& topology_file,
                       const fs::path& destination, const std::optional<fs::path>& modules)
    -> std::expected<void, Error> {
    const auto extension = space_file.extension();
    if (extension != ".json" && extension != ".yaml" && extension != ".yml")
        return std::unexpected{problem("population.format", "Configuration space requires a JSON or YAML file")};
    const auto source = environment.read_file(space_file);
    const auto saved_topology = environment.read_file(topology_file);
    if (!source || !saved_topology)
        return std::unexpected{problem("population.input", "Cannot read the population space and saved topology")};
    auto machine = environment.deserialize(*saved_topology, topology::TopologyJsonInputPolicy::require_canonical);
    if (!machine) return std::unexpected{machine.error()};
    if (machine->canonical_json != *saved_topology)
        return std::unexpected{problem("population.topology", "Topology bytes differ from their canonical loaded identity")};
    const scenario::ScenarioSource authored{
        "population", *source, extension == ".json" ? scenario::ScenarioFormat::json : scenario::ScenarioFormat::yaml};
    const auto space = scenario::ConfigurationSpace::parse(authored, maximum_trials);
    if (!space) return std::unexpected{space.error()};
    const auto catalog = runtime::discover_packages(modules);
    if (!catalog) return std::unexpected{catalog.error()};
    auto population = exp::ResolvedPopulation::resolve(authored, *machine,
        current_study_build, &*catalog);
    if (!population) return std::unexpected{population.error()};
    const auto published = environment.publish_file(destination, population->canonical(), false);
    if (!published) return std::unexpected{problem("population.publish", "Cannot create the frozen population artifact")};
    output << Json{{"profile", "epycbench.population-freeze-status/v1"},
        {"population_sha256", population->digest().to_hex()},
        {"candidate_count", population->document().at("candidate_count")},
        {"study_binding", "not_admitted"}}.dump() << '\n';
    if (!output) return std::unexpected{problem("population.output", "Cannot write the population freeze status")};
    return {};
}
