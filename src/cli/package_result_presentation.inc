// Included with the offline command so package projections share Result admission.
using PackageResult = results::PackageResult;
using ExportProfile = results::result_export::Profile;

auto package_source(const PackageResult& result, const std::filesystem::path& path) -> HistoryJson {
    HistoryJson source{{"artifact", path.filename().string()}, {"run_id", result.run_id},
        {"content_sha256", result.digest.to_hex()}, {"build_sha256", result.build.to_hex()},
        {"scenario_digest", result.scenario_digest}, {"resolved_plan_digest", result.resolved_plan_digest},
        {"physical_topology", result.physical_topology}, {"effective_topology", result.effective_topology},
        {"execution", result.execution}, {"validity", result.validity}, {"conclusion", result.conclusion},
        {"complete_valid_blocks", result.eligible_blocks}, {"evidence_source", result.document.at("evidence_source")}};
    if (result.summary.contains("case_id")) source["case_id"] = result.summary.at("case_id");
    return source;
}

auto no_package_energy(bool estimate = false) -> HistoryJson {
    return {{"schema", "epycbench.package-result-energy/v1"}, {"status", "unavailable"},
        {"scope", "package"}, {"reason", "not_recorded"}, {"estimate_requested", estimate},
        {"workload_attribution", nullptr}};
}

auto package_bindings(const PackageResult& result) -> HistoryJson {
    auto bindings = HistoryJson::object();
    for (const auto& [role, binding] : result.document.at("plan").at("bindings").items()) {
        auto& row = bindings[role];
        for (const auto* key : {"task_manifest_sha256", "oracle_sha256", "implementation_manifest_sha256", "module_sha256"})
            row[key] = binding.at(key);
    }
    return bindings;
}

auto package_callback_interval(const HistoryJson& trial) -> HistoryJson {
    auto interval = HistoryJson::object();
    for (const auto* key : {"begin_ns", "end_ns"}) {
        const auto time = trial.find(key);
        interval[key] = time != trial.end() && time->is_number_unsigned()
            ? HistoryJson(std::to_string(time->get<std::uint64_t>())) : HistoryJson(nullptr);
    }
    return interval;
}

auto collect_package_observations(const PackageResult& result, std::size_t limit, const auto& project) -> HistoryJson {
    auto rows = HistoryJson::array();
    const auto& blocks = result.document.at("blocks");
    for (const auto& block : blocks) {
        const auto append = [&](const HistoryJson& rank, const HistoryJson* chunk,
                                std::string_view condition = {}) {
            if (rows.size() < limit) {
                auto row = project(rank, block.at("ordinal"), chunk);
                if (!condition.empty()) row["condition"] = condition;
                rows.push_back(std::move(row));
            }
        };
        if (block.contains("mixed")) {
            const auto& mixed = block.at("mixed");
            const auto& attempts = mixed.at("condition_order");
            const auto together = result.document.at("plan").at("scenario").at("roles").size();
            const auto count = mixed.at("attempted").get<std::size_t>();
            std::size_t solo{};
            for (std::size_t i = 0; i < count && rows.size() < limit; ++i) {
                const bool concurrent = attempts.at(i) == together;
                const auto& evidence = concurrent ? block : mixed.at("alone").at(solo++).at("block");
                for (const auto& rank : evidence.at("ranks"))
                    append(rank, nullptr, concurrent ? "together" : "alone");
            }
        } else {
            for (const auto& rank : block.at("ranks")) append(rank, nullptr);
        }
        if (block.contains("stream")) {
            for (const auto& chunk:block.at("stream").at("chunks"))
                for (const auto& rank:chunk.at("ranks")) append(rank,&chunk);
        }
        if (rows.size() == limit) break;
    }
    return rows;
}

auto package_observations(const PackageResult& result, std::size_t limit) -> HistoryJson {
    return collect_package_observations(result, limit,
        [](const HistoryJson& rank, const HistoryJson& ordinal, const HistoryJson* chunk) {
            auto row = rank;
            row["block"] = ordinal;
            if (chunk) {
                row["chunk"] = chunk->at("ordinal");
                row["pass"] = chunk->at("pass");
                row["slot"] = chunk->at("slot");
                const auto& handoff = chunk->at("handoff_ns");
                row["handoff_ns"] = handoff.is_number_unsigned()
                    ? HistoryJson(std::to_string(handoff.get<std::uint64_t>())) : HistoryJson(nullptr);
                row["waited_for_slot"] = row.at("role") == chunk->at("ranks").at(0U).at("role")
                    ? chunk->at("producer_waited") : chunk->at("consumer_waited");
            }
            const auto& trial = rank.at("trial");
            if (!trial.is_null()) row["interval"] = package_callback_interval(trial);
            return row;
        });
}

auto package_unscored(const PackageResult& result, std::size_t limit) -> HistoryJson {
    auto rows = HistoryJson::array();
    auto dispositions = HistoryJson::array();
    std::size_t available{};
    const auto append = [&](const HistoryJson& rank, const HistoryJson& role, const HistoryJson& block,
                            std::size_t attempt, bool calibrating) {
        const auto trial = rank.find("trial");
        const auto id = rank.find("rank");
        if (trial == rank.end() || id == rank.end()) return;
        ++available;
        if (rows.size() >= limit) return;
        rows.push_back({{"phase", calibrating ? "calibration" : "warmup"}, {"block", block},
            {"attempt", attempt}, {"role", role}, {"rank", *id}, {"interval", package_callback_interval(*trial)},
            {"execute_status", trial->is_object() ? trial->value("execute_status", HistoryJson(nullptr)) : HistoryJson(nullptr)},
            {"verify_status", trial->is_object() ? trial->value("verify_status", HistoryJson(nullptr)) : HistoryJson(nullptr)},
            {"error", rank.value("error", HistoryJson(nullptr))}});
        if (trial->is_object() && trial->contains("timing_error") && !trial->at("timing_error").is_null())
            rows.back()["timing_error"] = trial->at("timing_error");
    };
    const auto calibration = result.document.find("calibration");
    if (calibration != result.document.end()) {
        std::size_t attempt{};
        for (const auto& probe : calibration->at("attempts")) {
            const auto& block = probe.at("block");
            dispositions.push_back({{"phase", "calibration"}, {"attempt", attempt},
                {"repetitions", probe.at("repetitions")}, {"runner_exit", block.at("runner_exit")},
                {"error", block.at("error")}});
            for (const auto& rank : block.at("ranks")) append(rank, rank.at("role"), nullptr, attempt, true);
            ++attempt;
        }
    }
    for (const auto& block : result.document.at("blocks")) {
        const auto warmup = block.find("warmup");
        if (warmup == block.end() || !warmup->is_array()) continue;
        for (const auto& group : *warmup) {
            const auto attempts = group.find("attempts");
            const auto role = group.find("role");
            if (attempts == group.end() || !attempts->is_array() || role == group.end()) continue;
            if (dispositions.size() < limit)
                dispositions.push_back({{"phase", "warmup"}, {"block", block.at("ordinal")}, {"role", *role},
                    {"status", group.value("status", HistoryJson(nullptr))}, {"attempts", attempts->size()}});
            std::size_t attempt{};
            for (const auto& ranks : *attempts) {
                if (ranks.is_array())
                    for (const auto& rank : ranks) append(rank, *role, block.at("ordinal"), attempt, false);
                ++attempt;
            }
        }
    }
    return {{"dispositions", std::move(dispositions)}, {"observations_available", available},
        {"observations_returned", rows.size()}, {"observations", std::move(rows)}};
}

auto package_replay(const PackageResult& result, const std::filesystem::path& path, std::size_t limit,
                    std::optional<HistoryJson> package_energy = {}, bool include_observations = true) -> HistoryJson {
    const auto& plan = result.document.at("plan");
    auto rows = include_observations ? package_observations(result, limit) : HistoryJson::array();
    auto blocks = HistoryJson::array();
    std::size_t available{};
    for (const auto& block : result.document.at("blocks")) {
        const auto& ranks = block.at("ranks");
        auto count=ranks.size();
        if (block.contains("mixed"))
            for (const auto& solo : block.at("mixed").at("alone"))
                count += solo.at("block").at("ranks").size();
        const auto stream=block.find("stream");
        const bool streaming=stream!=block.end();
        if (streaming)
            for (const auto& chunk:stream->at("chunks")) count+=chunk.at("ranks").size();
        available += count;
        if (!include_observations || blocks.size() == limit) continue;
        HistoryJson receipt{{"ordinal", block.at("ordinal")}, {"rank_count", count},
                            {"runner_exit", block.at("runner_exit")}, {"error", block.at("error")}};
        for (const auto* key : {"overlap_begin_ns", "overlap_end_ns"}) {
            const auto timestamp = block.find(key);
            receipt[key] = timestamp != block.end() && timestamp->is_number_unsigned()
                ? HistoryJson(std::to_string(timestamp->get<std::uint64_t>())) : HistoryJson(nullptr);
        }
        if (block.contains("collectives")) receipt["collectives"]=block.at("collectives");
        if (block.contains("replicas")) receipt["replicas"]=block.at("replicas");
        if (block.contains("input_proofs")) receipt["input_proofs"] = block.at("input_proofs");
        if (block.contains("mixed")) {
            receipt["mixed"] = result.mixed_blocks.at(block.at("ordinal").get<std::size_t>());
            auto& solo = receipt["mixed"]["alone_evidence"];
            solo = HistoryJson::array();
            for (const auto& one : block.at("mixed").at("alone")) {
                const auto& evidence = one.at("block");
                HistoryJson attempt{{"role", one.at("role")}, {"runner_exit", evidence.at("runner_exit")},
                                    {"error", evidence.at("error")}};
                for (const auto* field : {"collectives", "replicas", "input_proofs"})
                    if (evidence.contains(field)) attempt[field] = evidence.at(field);
                solo.push_back(std::move(attempt));
            }
        }
        if (streaming) {
            receipt["stream"]={{"slot_count",stream->at("slot_count")},{"error",stream->at("error")},
                {"chunks",stream->at("chunks").size()},{"chunks_per_pass",stream->at("chunks_per_pass")}};
            receipt["stream"]["external_inputs"] = stream->at("external_inputs");
            if (stream->contains("logical_handoff"))
                receipt["stream"]["logical_handoff"] = stream->at("logical_handoff");
            for (const auto* key:{"release_ns","completion_ns"}) {
                const auto& timestamp=stream->at(key);
                receipt["stream"][key]=timestamp.is_number_unsigned()
                    ? HistoryJson(std::to_string(timestamp.get<std::uint64_t>())) : HistoryJson(nullptr);
            }
        }
        blocks.push_back(std::move(receipt));
    }
    HistoryJson report{{"schema", "epycbench.package-result-replay/v1"}, {"source", package_source(result, path)},
        {"mode", plan.at("scenario").at("mode")}, {"timing_scope", plan.at("scope")},
        {"placement_plan", plan.at("placement")}, {"summary", result.summary},
        {"selection", include_observations ? "matched" : "missing_scope"},
        {"blocks_available", result.document.at("blocks").size()}, {"blocks", std::move(blocks)},
        {"observations_available", available}, {"observations_returned", rows.size()},
        {"observations", std::move(rows)}};
    const auto& execution = plan.at("scenario").at("execution");
    if (include_observations && (execution.contains("calibration") || execution.contains("warmup")))
        report["unscored"] = package_unscored(result, limit);
    if (package_energy) report["package_energy"] = std::move(*package_energy);
    return report;
}

auto package_counters(const PackageResult& result) -> HistoryJson {
    const auto& execution = result.document.at("plan").at("scenario").at("execution");
    if (!execution.value("hardware_counters", false))
        return {{"status", "unavailable"}, {"reason", "not_recorded"}};
    auto rows = HistoryJson::array();
    const bool supplied = result.document.at("evidence_source") == "supplied";
    std::size_t available{}, measured{}, qualified{};
    for (const auto& block : result.document.at("blocks")) {
        for (const auto& rank : block.at("ranks")) {
            const auto counter = rank.find("hardware_counters");
            const auto worker = counter != rank.end() && counter->is_object()
                ? counter->value("worker", HistoryJson(nullptr)) : HistoryJson(nullptr);
            const bool present = worker.is_object();
            const bool observed = present && worker.value("window", HistoryJson(nullptr)).is_object();
            const bool full_bracket = observed && !supplied && result.validity == "valid" &&
                worker.value("coverage", HistoryJson(nullptr)) == "complete";
            if (observed) ++measured;
            if (full_bracket) ++qualified;
            ++available;
            if (rows.size() >= maximum_display_observations) continue;
            rows.push_back({{"block", block.at("ordinal")}, {"role", rank.at("role")}, {"rank", rank.at("rank")},
                {"evidence", present ? *counter : HistoryJson(nullptr)}, {"qualified_full_bracket", full_bracket}});
        }
    }
    return {{"scope", "user_mode_worker_bracket"}, {"status", measured ? "observed" : "unavailable"},
        {"reason", measured ? HistoryJson(nullptr) : HistoryJson(supplied ? "supplied_workers" : "worker_counters_unavailable")},
        {"observations_available", available}, {"observations_returned", rows.size()}, {"observed_windows", measured},
        {"qualified_full_rank_brackets", qualified}, {"observations", std::move(rows)}};
}

auto package_fingerprint_work(const PackageResult& result) -> HistoryJson {
    constexpr auto limit = maximum_display_observations;
    auto rows = collect_package_observations(result, limit + 1U,
        [](const HistoryJson& rank, const HistoryJson& block, const HistoryJson* chunk) {
            const auto& trial = rank.at("trial");
            HistoryJson row{{"block", block}, {"role", rank.at("role")}, {"rank", rank.at("rank")},
                {"placement", rank.at("placement")}, {"error", rank.at("error")}};
            if (rank.contains("mapping")) row["mapping"] = rank.at("mapping");
            if (chunk) row["chunk"] = chunk->at("ordinal");
            if (trial.is_object())
                for (const auto* field : {"expected", "actual", "regions", "verify_status", "execute_status"})
                    if (trial.contains(field)) row[field] = trial.at(field);
            return row;
        });
    const bool truncated = rows.size() > limit;
    if (truncated) rows.erase(rows.end() - 1);
    auto groups = HistoryJson::array();
    for (const auto& block : result.document.at("blocks")) {
        HistoryJson row{{"block", block.at("ordinal")}};
        for (const auto* field : {"collectives", "replicas", "input_proofs"}) {
            const auto group = block.find(field);
            if (group != block.end() && !group->empty()) row[field] = *group;
        }
        if (row.size() > 1U) groups.push_back(std::move(row));
        if (groups.size() == limit) break;
    }
    return {{"observations", std::move(rows)}, {"observations_truncated", truncated},
        {"block_evidence", std::move(groups)}};
}

auto package_fingerprint(const PackageResult& result, const std::filesystem::path& path,
                         std::optional<std::size_t> pair, std::optional<HistoryJson> package_energy) -> HistoryJson {
    const auto& plan = result.document.at("plan");
    const auto& scenario = plan.at("scenario");
    auto report = HistoryJson{{"schema", "epycbench.package-workload-fingerprint/v1"},
                              {"source", package_source(result, path)}};
    if (package_energy) report["package_energy"] = std::move(*package_energy);
    const auto& mode = scenario.at("mode");
    if (pair && (*pair != 0U || (mode != "comparison" && mode != "standalone"))) {
        report["status"] = "unavailable";
        report["reason"] = "missing_scope";
        report["pair"] = *pair;
        return report;
    }
    report["status"] = "recorded";
    report["mode"] = mode;
    report["workloads"] = scenario.at("workloads");
    report["bindings"] = package_bindings(result);
    report["roles"] = scenario.at("roles");
    report["planned_placement"] = plan.at("placement");
    report["timing_scope"] = plan.at("scope");
    report["summary"] = result.summary;
    report["observed_inputs_sha256"] = result.observed_inputs_sha256
        ? HistoryJson(result.observed_inputs_sha256->to_hex()) : HistoryJson(nullptr);
    report["observed_work"] = package_fingerprint_work(result);
    report["hardware_counters"] = package_counters(result);
    return report;
}

auto export_package_count(const HistoryJson& value) -> HistoryJson {
    if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
    if (value.is_string()) {
        const auto token = value.get<std::string_view>();
        std::uint64_t count{};
        const auto end = token.data() + token.size();
        const auto parsed = std::from_chars(token.data(), end, count);
        if (parsed.ec == std::errc{} && parsed.ptr == end) return std::to_string(count);
    }
    return nullptr;
}

auto export_package_work(const HistoryJson& trial, const char* kind) -> HistoryJson {
    const auto work = trial.find(kind);
    if (work == trial.end() || !work->is_object()) return nullptr;
    auto reduced = HistoryJson::object();
    for (const char* field : {"repetitions", "work_units", "output_count"})
        reduced[field] = export_package_count(work->value(field, HistoryJson(nullptr)));
    return reduced;
}

auto export_package_counters(const HistoryJson& rank) -> HistoryJson {
    const auto counters = rank.find("hardware_counters");
    if (counters == rank.end()) return nullptr;
    const auto worker = counters->find("worker");
    if (worker == counters->end() || !worker->is_object() ||
        worker->value("coverage", HistoryJson(nullptr)) != "complete") return nullptr;
    const auto window = worker->find("window");
    if (window == worker->end() || !window->is_object()) return nullptr;
    const auto cycles = export_package_count(window->value("cpu_cycles", HistoryJson(nullptr)));
    const auto instructions = export_package_count(window->value("retired_instructions", HistoryJson(nullptr)));
    if (cycles.is_null() || instructions.is_null()) return nullptr;
    return {{"cpu_cycles", cycles}, {"retired_instructions", instructions}};
}

auto export_package_observations(const PackageResult& result) -> HistoryJson {
    const auto& roles = result.document.at("plan").at("scenario").at("roles");
    const bool native = result.document.at("evidence_source") == "native";
    return collect_package_observations(result, std::numeric_limits<std::size_t>::max(),
        [&](const HistoryJson& rank, const HistoryJson& block, const HistoryJson* chunk) {
            const auto role_id = rank.find("role");
            const auto role = std::ranges::find_if(roles, [&](const auto& item) {
                return role_id != rank.end() && item.at("id") == *role_id;
            });
            const auto& trial = rank.at("trial");
            const auto begin = trial.find("begin_ns");
            const auto end = trial.find("end_ns");
            HistoryJson elapsed;
            if (begin != trial.end() && end != trial.end() && begin->is_number_unsigned() &&
                end->is_number_unsigned() && *begin < *end)
                elapsed = std::to_string(end->get<std::uint64_t>() - begin->get<std::uint64_t>());
            const auto placement = rank.find("placement");
            const bool placed = placement != rank.end() && placement->is_object() &&
                placement->value("verified", HistoryJson(false)) == true;
            const bool succeeded = trial.is_object() && rank.at("error").is_null() &&
                trial.value("execute_status", HistoryJson(nullptr)) == 0 &&
                trial.value("verify_status", HistoryJson(nullptr)) == 0;
            HistoryJson row{{"block", block}, {"phase", "measurement"},
                {"role_ordinal", role == roles.end() ? HistoryJson(nullptr) : HistoryJson(role - roles.begin())},
                {"rank", export_package_count(rank.at("rank"))}, {"elapsed_ns", std::move(elapsed)},
                {"placement_verified", placed}, {"callbacks_succeeded", succeeded},
                {"expected_work", export_package_work(trial, "expected")},
                {"actual_work", export_package_work(trial, "actual")},
                {"worker_counters", native && succeeded ? export_package_counters(rank) : HistoryJson(nullptr)}};
            if (chunk) {
                row["chunk"] = chunk->at("ordinal");
                row["pass"] = chunk->at("pass");
                row["slot"] = chunk->at("slot");
            }
            return row;
        });
}

auto project_package_export(const PackageResult& result, ExportProfile profile) -> HistoryJson {
    const auto& summary = result.summary;
    HistoryJson record{{"source", {{"content_sha256", result.digest.to_hex()}, {"execution", result.execution},
        {"validity", result.validity}, {"conclusion", result.conclusion}, {"complete_valid_blocks", result.eligible_blocks},
        {"evidence_source", result.document.at("evidence_source")}}}, {"timing_scope", summary.at("metric_scope")}};
    auto metrics = HistoryJson::array();
    for (const auto& work : summary.at("work_metrics"))
        metrics.push_back({{"role_ordinal", metrics.size()}, {"metric", work.at("metric")},
            {"scope", work.at("scope")}, {"median", work.at("median")}});
    if (profile == ExportProfile::observation_table) {
        record["schema"] = "epycbench.package-result-observation-table/v1";
        record["work_metrics"] = std::move(metrics);
    } else {
        record["schema"] = profile == ExportProfile::summary
            ? "epycbench.package-result-summary/v1" : "epycbench.package-result-shareable/v1";
        auto& reduced = record["summary"];
        for (const auto* field : {"execution", "validity", "conclusion", "verified_blocks", "eligible_blocks",
                                  "metric_scope", "evidence_source", "scaling"})
            reduced[field] = summary.at(field);
        for (const auto* field : {"calibration", "precision", "whole_group"})
            if (summary.contains(field)) reduced[field] = summary.at(field);
        reduced["work_metrics"] = std::move(metrics);
        if (profile == ExportProfile::summary) {
            record["scaling"] = result.scaling;
            if (summary.contains("monitoring")) reduced["monitoring"] = summary.at("monitoring");
            if (summary.contains("case_id")) reduced["case_id"] = summary.at("case_id");
            if (summary.contains("mixed_slowdown")) {
                auto relative = summary.at("mixed_slowdown");
                std::size_t ordinal{};
                for (auto& role : relative.at("roles")) {
                    role.erase("role");
                    role["role_ordinal"] = ordinal++;
                }
                reduced["mixed_slowdown"] = std::move(relative);
            }
        }
    }
    if (profile != ExportProfile::summary) record["observations"] = export_package_observations(result);
    return record;
}

auto render_package_performance_summary(const PackageResult& result) -> std::string {
    std::ostringstream text;
    const auto& summary = result.summary;
    if (summary.contains("case_id"))
        text << "Case " << summary.at("case_id").get<std::string_view>() << '\n';
    text << "Run " << result.execution << " · " << result.validity << '\n'
         << summary.at("verified_blocks") << " verified blocks · " << result.eligible_blocks << " eligible blocks\n";
    if (const auto calibration = summary.find("calibration"); calibration != summary.end()) {
        text << "Calibration " << calibration->at("status").get<std::string_view>();
        if (!calibration->at("selected_repetitions").is_null())
            text << " · " << calibration->at("selected_repetitions") << " repetitions per block";
        text << '\n';
    }
    if (const auto precision = summary.find("precision"); precision != summary.end())
        text << "Stop reason " << precision->at("stop_reason").get<std::string_view>() << '\n';
    if (result.document.at("evidence_source") == "supplied") {
        text << "Supplied worker timings are unscored.\n";
    } else if (const auto group = summary.find("whole_group"); group != summary.end() && group->at("median").is_number()) {
        text << "Whole-group median " << group->at("median").get<double>() << " composite tasks/s\n";
        if (const auto mixed = summary.find("mixed_slowdown"); mixed != summary.end()) {
            for (const auto& role : mixed->at("roles")) {
                const auto& slowdown = role.at("median_slowdown");
                if (slowdown.is_number())
                    text << role.at("role").get<std::string_view>() << " · median slowdown "
                         << slowdown.get<double>() << "× versus alone\n";
            }
        }
    } else if (!result.scaling.empty()) {
        const auto& point = result.scaling.front();
        if (result.document.at("plan").at("scenario").at("mode") == "comparison")
            text << "Baseline median " << point.at("baseline_median").get<double>() << " tasks/s · candidate median "
                 << point.at("candidate_median").get<double>() << " tasks/s\n";
        else text << "Median " << point.at("candidate_median").get<double>() << " tasks/s\n";
        for (const auto& work : summary.at("work_metrics")) {
            const auto& measured = work.at("median");
            const auto unit = work.at("work_unit").get<std::string_view>();
            if (!measured.is_number() || unit == "work_unit") continue;
            text << work.at("role").get<std::string_view>() << " · " << measured.get<double>();
            if (work.at("metric") == "seconds_per_unit") text << " s per " << unit;
            else text << ' ' << unit << "/s";
            text << '\n';
        }
        if (result.conclusion != "not_evaluated") {
            auto label = result.conclusion;
            std::ranges::replace(label, '_', ' ');
            text << "Conclusion " << label << '\n';
        }
    }
    return text.str();
}

auto package_comparable_plan(const PackageResult& result, bool compare_chunk_counts) -> HistoryJson {
    auto plan = result.document.at("plan");
    auto& controls = plan.at("scenario").at("execution");
    controls.erase("order_seed");
    controls.erase("controller_core"); // The resolved controller below records the actual placement.
    if (compare_chunk_counts) {
        // Chunk layouts have different lengths. Each validated block has already
        // verified those layouts and each rank's placement against this plan.
        controls.erase("pipeline_chunks");
        plan.erase("geometry");
        for (const auto& [role, binding] : plan.at("bindings").items())
            plan["stream_task_contracts"][role] = {binding.at("task_manifest_sha256"), binding.at("oracle_sha256")};
    }
    // A descriptive across-build match uses the effective configuration, geometry
    // and verified inputs. Report the exact binaries and manifests on each side.
    plan.erase("bindings");
    for (auto metric : result.summary.at("work_metrics")) {
        metric.erase("median");
        plan["metric_contracts"].push_back(std::move(metric));
    }
    return plan;
}

// Compare independently verified work and placement over the qualified block
// population. A precision-stopped Run can repeat the same problem fewer times.
auto package_observed_inputs(const PackageResult& result, const HistoryJson& role) -> HistoryJson {
    const auto regions = [](const HistoryJson& evidence) {
        auto facts = HistoryJson::object();
        for (const auto& region : evidence) {
            const auto kind = region.at("kind");
            auto& item = facts[region.at("name").get_ref<const std::string&>()];
            item = {{"kind", kind}, {"bytes", region.at("bytes")}};
            if (kind == EPYCBENCH_PACKAGE_INPUT || kind == EPYCBENCH_PACKAGE_SHARED_INPUT ||
                kind == EPYCBENCH_PACKAGE_SHARED)
                item["initial"] = region.at("before");
            if (!region.at("reference").is_null()) item["reference"] = region.at("reference");
        }
        return facts;
    };
    std::map<std::string, std::size_t> counts;
    for (const auto& disposition : result.summary.at("block_dispositions")) {
        if (!disposition.at("qualified").get<bool>()) continue;
        const auto& block = result.document.at("blocks").at(disposition.at("ordinal").get<std::size_t>());
        auto facts = HistoryJson::object();
        const auto rank_facts = [&](const HistoryJson& ranks) {
            auto observed = HistoryJson::object();
            for (const auto& rank : ranks) {
                if (rank.at("role") != role) continue;
                const auto& trial = rank.at("trial");
                observed[std::to_string(rank.at("rank").get<std::size_t>())] = {
                    {"regions", regions(trial.at("regions"))}, {"placement", rank.at("placement")},
                    {"verified_work", trial.at("expected")}};
            }
            return observed;
        };
        if (block.contains("stream")) {
            for (const auto& chunk : block.at("stream").at("chunks"))
                facts["chunks"].push_back(rank_facts(chunk.at("ranks")));
        } else {
            facts["ranks"] = rank_facts(block.at("ranks"));
        }
        if (block.contains("collectives"))
            for (const auto& group : block.at("collectives"))
                if (group.at("role") == role)
                    facts["aggregate"] = {{"regions", regions(group.at("regions"))},
                                          {"verified_work", group.at("expected")}};
        if (!facts.empty()) ++counts[core::sha256(facts.dump()).to_hex()];
    }
    std::size_t divisor{};
    for (const auto& [identity, count] : counts) divisor = std::gcd(divisor, count);
    auto distribution = HistoryJson::object();
    for (const auto& [identity, count] : counts) distribution[identity] = count / divisor;
    return distribution;
}

auto compare_package_results(const PackageResult* baseline, const PackageResult* candidate,
                             const HistoricalSource& first, const HistoricalSource& second) -> HistoryJson {
    auto report = HistoryJson{{"schema", "epycbench.package-historical-comparison/v1"}};
    const auto identity = [](const PackageResult& r, const HistoricalSource& file) {
        auto source = package_source(r, file.path);
        auto& identities = source["packages"] = HistoryJson::object();
        for (const auto& [role, binding] : r.document.at("plan").at("bindings").items())
            for (const auto* key : {"module_sha256", "oracle_sha256", "implementation_manifest_sha256", "task_manifest_sha256"})
                identities[role][key] = binding.at(key);
        return source;
    };
    if (baseline) report["baseline"] = identity(*baseline, first);
    if (candidate) report["candidate"] = identity(*candidate, second);
    const auto unmatched = [&](std::string_view reason) {
        report["status"] = "unmatched";
        report["reasons"] = HistoryJson::array({{{"side", "both"}, {"reason", reason}}});
        return report;
    };
    if (!baseline || !candidate) return unmatched("different_result_profiles");
    const auto& left = baseline->document.at("plan").at("scenario");
    const auto& right = candidate->document.at("plan").at("scenario");
    if (first.pair != 0U || second.pair != 0U) return unmatched("missing_scope");
    const bool standalone = left.at("mode") == "standalone";
    const bool group = left.at("mode") == "mixed" || left.at("mode") == "pipeline";
    const auto& left_controls = left.at("execution");
    const auto& right_controls = right.at("execution");
    const bool compare_chunk_counts = left_controls.contains("pipeline_chunks") && right_controls.contains("pipeline_chunks") &&
        left_controls.at("pipeline_chunks") != right_controls.at("pipeline_chunks");
    if (left.at("mode") != right.at("mode") || (!standalone && !group && first.variant != second.variant) ||
        package_comparable_plan(*baseline, compare_chunk_counts) != package_comparable_plan(*candidate, compare_chunk_counts))
        return unmatched("different_package_scope");
    const auto& evidence = baseline->document.at("evidence_source");
    if (evidence != candidate->document.at("evidence_source"))
        return unmatched("different_evidence_sources");
    const auto* required_rate = group ? "observed_group_rate" : "observed_role_rates";
    if (baseline->execution != "completed" || candidate->execution != "completed" ||
        !baseline->summary.contains(required_rate) || !candidate->summary.contains(required_rate))
        return unmatched("valid_measurement_unavailable");
    if (baseline->calibrated_repetitions != candidate->calibrated_repetitions)
        return unmatched("different_calibrated_repetitions");
    if (!baseline->observed_inputs_sha256 || !candidate->observed_inputs_sha256)
        return unmatched("observed_inputs_unavailable");
    if (baseline->observed_inputs_sha256 != candidate->observed_inputs_sha256)
        return unmatched("different_observed_inputs");
    const bool first_role = standalone || first.variant == "first";
    const auto& role = left.at("roles").at(first_role ? 0U : 1U).at("id");
    if (compare_chunk_counts) {
        // The verified observed-input identity above covers logical external
        // inputs, handoff bytes, work units and output counts over the full stream.
        report["baseline"]["pipeline_chunks"] = left_controls.at("pipeline_chunks");
        report["candidate"]["pipeline_chunks"] = right_controls.at("pipeline_chunks");
    } else {
        for (const auto& selected : left.at("roles")) {
            const auto& id = selected.at("id");
            const auto facts = package_observed_inputs(*baseline, id);
            if (facts.empty() || facts != package_observed_inputs(*candidate, id))
                return unmatched("observed_inputs_or_placement_unmatched");
        }
    }
    const auto select = [&role, group](const PackageResult& result) -> std::optional<double> {
        const auto& value = group ? result.summary.at("observed_group_rate")
                                  : result.summary.at("observed_role_rates").at(role.get_ref<const std::string&>());
        if (!value.is_number()) return std::nullopt;
        const auto item = value.get<double>();
        if (!std::isfinite(item) || item <= 0.0 || !std::isfinite(1.0 / item)) return std::nullopt;
        return item;
    };
    const auto left_rate = select(*baseline);
    const auto right_rate = select(*candidate);
    if (!left_rate || !right_rate || !std::isfinite(*left_rate / *right_rate) ||
        !std::isfinite(*right_rate / *left_rate))
        return unmatched("retained_throughput_unavailable");
    const auto describe = [](double old_value, double new_value) {
        const auto ratio = new_value / old_value;
        return HistoryJson{{"baseline", old_value}, {"candidate", new_value},
            {"absolute_change", new_value - old_value}, {"ratio", ratio}, {"relative_change", ratio - 1.0}};
    };
    report["status"] = "descriptive";
    report["evidence_source"] = evidence;
    report["performance_evidence"] = evidence == "native";
    report["build_relation"] = baseline->build == candidate->build ? "same_build" : "different_build";
    report["metric_scope"] = baseline->summary.at("metric_scope");
    if (group) {
        report["composite_tasks_per_second"] = describe(*left_rate, *right_rate);
        report["seconds_per_composite_task"] = describe(1.0 / *left_rate, 1.0 / *right_rate);
        if (left.at("mode") == "mixed") {
            report["baseline"]["slowdown"] = baseline->summary.at("mixed_slowdown");
            report["candidate"]["slowdown"] = candidate->summary.at("mixed_slowdown");
        }
        return report;
    }
    report["role"] = role;
    report["tasks_per_second"] = describe(*left_rate, *right_rate);
    report["seconds_per_completed_task"] = describe(1.0 / *left_rate, 1.0 / *right_rate);
    const auto& old_work = baseline->summary.at("work_metrics").at(first_role ? 0U : 1U);
    const auto& new_work = candidate->summary.at("work_metrics").at(first_role ? 0U : 1U);
    const auto& old_median = old_work.at("median");
    const auto& new_median = new_work.at("median");
    if (old_median.is_number() && new_median.is_number()) {
        const auto old_value = old_median.get<double>();
        const auto new_value = new_median.get<double>();
        if (std::isfinite(old_value) && std::isfinite(new_value) && old_value > 0.0 && new_value > 0.0 &&
            std::isfinite(new_value / old_value)) {
            report["declared_work"] = {{"unit", old_work.at("work_unit")}, {"metric", old_work.at("metric")},
                {"scope", old_work.at("scope")}, {"change", describe(old_value, new_value)}};
            if (old_work.at("work_unit") == "byte" && old_work.at("metric") == "units_per_second") {
                report["bytes_per_second"] = describe(old_value, new_value);
                report["seconds_per_logical_byte"] = describe(1.0 / old_value, 1.0 / new_value);
            }
        }
    }
    return report;
}
