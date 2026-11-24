#include "runtime/package_runner.hpp"

#include "measurement/package_control_json.hpp"
#include "platform_linux/package_mapping.hpp"
#include "platform_linux/package_process.hpp"
#include "platform_linux/package_worker_monitoring.hpp"
#include "results/package_observation.hpp"
#include "results/package_stream.hpp"
#include "results/package_worker_counter_wire.hpp"
#include "runtime/package_admission.hpp"
#include "scenario/package_order.hpp"

#include <algorithm>

namespace epycbench::runtime {
namespace {
using Json = nlohmann::json;
using U64 = std::uint64_t;
namespace worker = platform_linux::package_execution;
namespace control = measurement::package_control;

auto hex(const std::optional<core::Sha256Digest>& digest) -> Json {
    return digest ? Json(digest->to_hex()) : Json(nullptr);
}
template <class T> auto optional(const std::optional<T>& value) -> Json {
    return value ? Json(*value) : Json(nullptr);
}

auto encode_work(const EpycBenchPackageWorkV1& w) -> Json {
    return {{"repetitions", w.repetitions},
            {"work_units", w.work_units},
            {"output_count", w.output_count}};
}

auto encode_regions(const std::vector<worker::RegionEvidence>& evidence) -> Json {
    auto regions = Json::array();
    for (const auto& region : evidence)
        regions.push_back({{"name", region.name},
                           {"kind", region.kind},
                           {"bytes", region.bytes},
                           {"alignment", region.alignment},
                           {"before", hex(region.before)},
                           {"after", hex(region.after)},
                           {"reference", hex(region.reference)}});
    return regions;
}

auto encode_trial(const worker::Trial& t) -> Json {
    return {{"begin_ns", optional(t.begin_ns)},
            {"end_ns", optional(t.end_ns)},
            {"partition_begin", t.partition_begin},
            {"partition_count", t.partition_count},
            {"execute_status", t.execute_status},
            {"verify_status", t.verify_status},
            {"expected", encode_work(t.expected)},
            {"actual", encode_work(t.actual)},
            {"regions", encode_regions(t.regions)},
            {"timing_error", t.timing_error ? Json(t.timing_error->code.value()) : Json(nullptr)}};
}

auto encode_rank(const worker::RankTrial& r) -> Json {
    auto regions = Json::array(), transfers = Json::array();
    const auto& placement = r.placement;
    for (const auto& region : placement.regions) {
        auto targets = Json::array(), distribution = Json::array();
        for (const auto node : region.targets)
            targets.push_back(node.value());
        for (const auto& row : region.distribution)
            distribution.push_back(
                {{"node", row.node.value()}, {"before", row.before}, {"after", row.after}});
        regions.push_back(
            {{"name", region.name},
             {"pages", region.pages},
             {"resident_before", region.resident_before},
             {"resident_after", region.resident_after},
             {"node", region.node.value()},
             {"policy", runtime::package_input_policy_name(region.policy)},
             {"targets", std::move(targets)},
             {"distribution", std::move(distribution)},
             {"unresolved_before", region.unresolved_before},
             {"unresolved_after", region.unresolved_after},
             {"owner_rank", region.owner_rank ? Json(*region.owner_rank) : Json(nullptr)},
             {"released", region.released ? Json(*region.released) : Json(nullptr)}});
    }
    for (const auto& link : r.transfers)
        transfers.push_back({{"source_role", link.source_role},
                             {"output", link.output},
                             {"input", link.input},
                             {"bytes", link.bytes},
                             {"sha256", link.sha256.to_hex()}});
    Json evidence{
        {"role", r.role},
        {"rank", r.rank},
        {"rank_count", r.rank_count},
        {"error", r.error ? Json{{"code", r.error->code.value()},
                                 {"message", r.error->message},
                                 {"context", r.error->context}}
                          : Json(nullptr)},
        {"cleanup_error", r.cleanup_error ? Json(r.cleanup_error->code.value()) : Json(nullptr)},
        {"trial", r.trial ? encode_trial(*r.trial) : Json(nullptr)},
        {"transfers", std::move(transfers)},
        {"hardware_counters",
         r.trial && r.trial->hardware_counters
             ? results::package_worker_counter_wire::project(*r.trial->hardware_counters)
             : Json(nullptr)},
        {"placement",
         {{"cpu", placement.requested_cpu.value()},
          {"numa_node", placement.requested_node.value()},
          {"cpu_before",
           placement.cpu_before ? Json(placement.cpu_before->value()) : Json(nullptr)},
          {"cpu_after", placement.cpu_after ? Json(placement.cpu_after->value()) : Json(nullptr)},
          {"page_bytes", placement.page_bytes},
          {"regions", std::move(regions)},
          {"verified", placement.verified}}}};
    if (!r.monitoring.is_null()) evidence["monitoring"] = r.monitoring;
    return evidence;
}

auto supplied_ranks(const Json& rows) -> std::vector<worker::SuppliedRank> {
    std::vector<worker::SuppliedRank> records;
    if (!rows.is_array() || rows.size() > 65536U)
        throw std::invalid_argument("Expected a bounded array of supplied ranks");
    for (const auto& row : rows) {
        const core::LinuxCpuId cpu{row.at("cpu").get<std::uint32_t>()};
        const core::LinuxNumaNodeId node{row.at("numa_node").get<std::uint32_t>()};
        worker::Placement placement{cpu, node, cpu, cpu, row.at("page_bytes").get<U64>(),
                                    {},  false};
        for (const auto& region : row.at("regions")) {
            worker::RegionPlacement evidence{
                region.at("name").get<std::string>(), region.at("pages").get<U64>(),
                region.at("resident_before").get<U64>(), region.at("resident_after").get<U64>(),
                core::LinuxNumaNodeId{region.value("node", node.value())}};
            evidence.policy =
                runtime::package_input_policy(region.value("policy", std::string{"bind"}));
            if (region.contains("targets"))
                for (const auto& target : region.at("targets"))
                    evidence.targets.emplace_back(target.get<std::uint32_t>());
            if (region.contains("distribution"))
                for (const auto& location : region.at("distribution"))
                    evidence.distribution.push_back(
                        {core::LinuxNumaNodeId{location.at("node").get<std::uint32_t>()},
                         location.at("before").get<U64>(), location.at("after").get<U64>()});
            evidence.unresolved_before = region.value("unresolved_before", U64{});
            evidence.unresolved_after = region.value("unresolved_after", U64{});
            if (region.contains("owner_rank") && !region.at("owner_rank").is_null())
                evidence.owner_rank = region.at("owner_rank").get<std::uint32_t>();
            placement.regions.push_back(std::move(evidence));
        }
        records.push_back({row.at("role").get<std::string>(), row.at("rank").get<std::uint32_t>(),
                           row.at("begin_ns").get<U64>(), row.at("end_ns").get<U64>(),
                           std::move(placement)});
        if (row.contains("monitoring")) {
            if (!row.at("monitoring").is_object())
                throw std::invalid_argument("Expected supplied monitoring channels by name");
            records.back().monitoring = row.at("monitoring");
        }
        if (row.contains("warmup")) {
            const auto& warmup = row.at("warmup");
            if (!warmup.is_array() || warmup.size() > 64U)
                throw std::invalid_argument("Expected up to 64 warmup intervals");
            for (const auto& interval : warmup)
                records.back().warmup.push_back(
                    {interval.at("begin_ns").get<U64>(), interval.at("end_ns").get<U64>()});
        }
        if (row.contains("finalization")) {
            const auto& f = row.at("finalization");
            records.back().finalization = worker::SuppliedRank::FinalizationTimes{
                f.at("release_ns").get<U64>(), f.at("scan_end_ns").get<U64>(),
                f.at("begin_ns").get<U64>(), f.at("end_ns").get<U64>()};
            if (f.contains("monitoring")) {
                if (!f.at("monitoring").is_object())
                    throw std::invalid_argument("Expected supplied finalization monitoring channels by name");
                records.back().finalization->monitoring = f.at("monitoring");
            }
        }
    }
    return records;
}

auto supplied_stream(const Json& payload) -> worker::SuppliedStream {
    const auto& stream = payload.at("stream");
    worker::SuppliedStream supplied{
        stream.at("release_ns").get<U64>(), stream.at("completion_ns").get<U64>(), {}, {}};
    const auto& chunks = stream.at("chunks");
    if (!chunks.is_array() || chunks.size() > 64U)
        throw std::invalid_argument("Expected bounded supplied stream chunks");
    for (const auto& chunk : chunks) {
        supplied.chunks.push_back(supplied_ranks(chunk.at("ranks")));
        supplied.handoff_ns.push_back(chunk.at("handoff_ns").get<U64>());
    }
    return supplied;
}

auto encode_stream(const worker::StreamEvidence& evidence) -> Json {
    Json chunks = Json::array();
    Json transfers = Json::array();
    for (const auto& chunk : evidence.chunks) {
        const auto ordinal = chunks.size();
        chunks.push_back(
            {{"ordinal", ordinal},
             {"pass", ordinal / evidence.chunks_per_pass},
             {"slot", ordinal % 2U},
             {"handoff_ns", optional(chunk.handoff_ns)},
             {"producer_waited", chunk.producer_waited},
             {"consumer_waited", chunk.consumer_waited},
             {"ranks", Json::array({encode_rank(chunk.ranks[0]), encode_rank(chunk.ranks[1])})}});
        const auto& handoff = chunks.back().at("ranks").at(1U).at("transfers");
        if (handoff.size() == 1U) transfers.push_back(handoff.front());
    }
    Json logical_handoff = nullptr;
    if (evidence.logical_handoff && transfers.size() == chunks.size()) {
        const auto& handoff = *evidence.logical_handoff;
        logical_handoff = {{"bytes", handoff.bytes}, {"reference_sha256", handoff.reference.to_hex()},
            {"observed_sha256", handoff.observed.to_hex()}};
        logical_handoff["binding_sha256"] = results::package_stream::handoff_binding(logical_handoff, transfers);
    }
    return {{"release_ns", optional(evidence.release_ns)},
            {"completion_ns", optional(evidence.completion_ns)},
            {"slot_count", 2U},
            {"chunks_per_pass", evidence.chunks_per_pass},
            {"external_inputs", evidence.external_inputs},
            {"logical_handoff", std::move(logical_handoff)},
            {"chunks", std::move(chunks)},
            {"error", evidence.error ? Json(*evidence.error) : Json(nullptr)}};
}

auto execute_block(const Json& request) -> platform_linux::package_process::Block {
    const auto& plan = request.at("plan");
    const auto& scenario = plan.at("scenario");
    const auto& digest = request.at("plan_digest");
    const auto error = [](std::string message) {
        return core::Error{core::ErrorCategory::runtime,
                           core::StableErrorCode{"runtime.package.binding"},
                           std::move(message),
                           "package_runner",
                           {},
                           Json::object(),
                           core::ErrorSeverity::error,
                           core::ContinuationPolicy::abort_command};
    };
    if (digest != "sha256:" + core::sha256(plan.dump()).to_hex())
        return std::unexpected{error("Runner received a different resolved plan")};
    const auto files = request.at("manifests").get<std::vector<std::filesystem::path>>();
    auto catalog = kernel::packages::Catalog::load_manifests(files);
    if (!catalog)
        return std::unexpected{catalog.error()};
    auto roles = runtime::package_roles(plan, *catalog);
    if (!roles)
        return std::unexpected{roles.error()};
    const auto repetitions = request.at("repetitions").get<U64>();
    const auto& execution = scenario.at("execution");
    const auto ceiling = execution.at("repetitions").get<U64>();
    if (!repetitions || repetitions > ceiling ||
        (repetitions != ceiling && !execution.contains("calibration")))
        return std::unexpected{
            error("Worker repetition count is outside the admitted control policy")};
    for (auto& role : *roles)
        role.request.repetitions = repetitions;
    const auto& mode = scenario.at("mode");
    const auto composition = mode == "pipeline" ? worker::Composition::pipeline
                             : mode == "mixed"  ? worker::Composition::mixed
                                                : worker::Composition::sequential;
    const auto ordinal = request.at("ordinal").get<std::size_t>();
    for (auto& role : *roles)
        role.request.block = ordinal;
    if (mode == "comparison") {
        auto order = scenario::package_candidate_first(
            scenario.at("execution").value("order_seed", U64{}), ordinal);
        if (!order)
            return std::unexpected{order.error()};
        if (*order)
            std::ranges::reverse(*roles);
    }
    const bool supplied = !request.at("workers").is_null();
    const bool calibrating = request.at("calibrating").get<bool>();
    std::optional<worker::PackageMonitor> monitoring;
    if (execution.contains("monitoring") && !calibrating) {
        std::vector<core::LinuxCpuId> cpus;
        for (const auto& role : *roles) cpus.insert(cpus.end(), role.cpus.begin(), role.cpus.end());
        monitoring.emplace(measurement::package_monitoring::read(execution.at("monitoring")), cpus, supplied);
    }
    const auto has_supplied_monitoring = [](const auto& rank) {
        return !rank.monitoring.is_null() || (rank.finalization && !rank.finalization->monitoring.is_null());
    };
    const auto complete_monitoring = [&](worker::RankTrial& rank) {
        if (monitoring && rank.trial && rank.trial->begin_ns && rank.trial->end_ns)
            monitoring->complete(rank.monitoring, rank.placement.requested_cpu,
                                 *rank.trial->begin_ns, *rank.trial->end_ns);
    };
    if (plan.contains("mapping")) {
        if (mode != "standalone" || roles->size() != 1U || repetitions != 1U ||
            request.at("calibrating") == true)
            return std::unexpected{error("Fresh mapping requires one standalone worker operation")};
        const auto* fixture = supplied ? &request.at("workers").at("ranks") : nullptr;
        if (fixture && (!fixture->is_array() || fixture->size() != 1U))
            return std::unexpected{error("Expected one supplied fresh mapping rank")};
        if (!monitoring && fixture && fixture->at(0).contains("monitoring"))
            return std::unexpected{error("Supplied monitoring requires an authored monitoring policy")};
        const auto& mapping = plan.at("mapping");
        auto result = worker::run_mapping(roles->front(), mapping, fixture ? &fixture->at(0) : nullptr,
                                          monitoring ? &*monitoring : nullptr);
        if (!result) return std::unexpected{std::move(result.error())};
        if (monitoring) {
            monitoring->stop();
            complete_monitoring(result->rank);
        }
        auto row = encode_rank(result->rank);
        row["placement"]["numa_node"] = mapping.at("linux_numa_node");
        row["mapping"] = std::move(result->mapping);
        const bool verified = results::package_observation::verified_mapping(
            mapping, plan.at("placement").at(0).at("cpus").at(0), row);
        Json block{{"ordinal", ordinal}, {"error", nullptr}, {"ranks", Json::array({std::move(row)})},
            {"overlap_begin_ns", nullptr}, {"overlap_end_ns", nullptr}, {"replicas", Json::array()},
            {"collectives", Json::array()}, {"input_proofs", Json::array({{{"role", roles->front().id},
                {"regions", Json::array()}, {"error", nullptr}}})}};
        return Json{{"block", std::move(block)}, {"ordinal", ordinal}, {"plan_digest", digest},
                    {"supplied", supplied}, {"verified", verified}};
    }
    if (execution.contains("pipeline_chunks")) {
        const auto fixture =
            supplied ? std::optional{supplied_stream(request.at("workers"))} : std::nullopt;
        if (!monitoring && fixture && std::ranges::any_of(fixture->chunks, [&](const auto& ranks) {
                return std::ranges::any_of(ranks, has_supplied_monitoring);
            }))
            return std::unexpected{error("Supplied monitoring requires an authored monitoring policy")};
        auto stream = worker::run_stream(*roles, execution.at("pipeline_chunks").get<std::size_t>(),
                                         fixture ? &*fixture : nullptr, &plan.at("geometry"),
                                         monitoring ? &*monitoring : nullptr);
        if (!stream)
            return std::unexpected{stream.error()};
        if (monitoring) {
            monitoring->stop();
            for (auto& chunk : stream->chunks)
                for (auto& rank : chunk.ranks) complete_monitoring(rank);
        }
        Json block{{"ordinal", ordinal},
                   {"ranks", Json::array()},
                   {"error", nullptr},
                   {"stream", encode_stream(*stream)}};
        return Json{{"block", std::move(block)},
                    {"ordinal", ordinal},
                    {"plan_digest", digest},
                    {"supplied", supplied},
                    {"verified", stream->valid()}};
    }
    std::vector<worker::SuppliedRank> records;
    worker::WorkerProviders providers;
    const bool warming = execution.contains("warmup") && !calibrating;
    if (supplied) {
        records = supplied_ranks(request.at("workers").at("ranks"));
        if (!warming && std::ranges::any_of(records, [](const auto& r) { return !r.warmup.empty(); }))
            return std::unexpected{error("Supplied warmup intervals require a measured warmup phase")};
        if (!monitoring && std::ranges::any_of(records, has_supplied_monitoring))
            return std::unexpected{error("Supplied monitoring requires an authored measured monitoring policy")};
        providers = worker::supplied_workers(records);
    }
    providers.admitted_geometry = &plan.at("geometry");
    providers.monitor = monitoring ? &*monitoring : nullptr;
    if (warming)
        providers.warmup = control::warmup_settings(execution);
    auto group = worker::run_workers(*roles, composition, providers);
    if (!group)
        return std::unexpected{group.error()};
    if (monitoring) {
        monitoring->stop();
        for (auto& rank : group->ranks) complete_monitoring(rank);
        for (auto& team : group->collectives) {
            auto& phase = team.finalization;
            if (!phase || !phase->begin_ns || !phase->end_ns) continue;
            const auto owner = std::ranges::find_if(group->ranks, [&](const auto& rank) {
                return rank.role == team.role && rank.rank == team.owner_rank;
            });
            if (owner != group->ranks.end())
                monitoring->complete(phase->monitoring, owner->placement.requested_cpu,
                                     *phase->begin_ns, *phase->end_ns);
        }
    }
    Json block{{"ordinal", ordinal},
               {"ranks", Json::array()},
               {"error", nullptr},
               {"overlap_begin_ns", optional(group->overlap_begin_ns)},
               {"overlap_end_ns", optional(group->overlap_end_ns)}};
    for (const auto& rank : group->ranks)
        block["ranks"].push_back(encode_rank(rank));
    block["input_proofs"] = Json::array();
    for (const auto& role : group->input_proofs) {
        auto regions = Json::array();
        for (const auto& region : role.regions)
            regions.push_back({{"name", region.name},
                               {"bytes", region.bytes},
                               {"sha256", region.sha256.to_hex()}});
        block["input_proofs"].push_back(
            {{"role", role.role},
             {"regions", std::move(regions)},
             {"error", role.error ? Json(role.error->code.value()) : Json(nullptr)}});
    }
    if (warming) {
        block["warmup"] = Json::array();
        for (const auto& role : group->warmup) {
            auto attempts = Json::array();
            for (const auto& attempt : role.attempts) {
                auto ranks = Json::array();
                for (const auto& rank : attempt)
                    ranks.push_back(
                        {{"rank", rank.rank},
                         {"trial", rank.trial ? encode_trial(*rank.trial) : Json(nullptr)},
                         {"error", rank.error ? Json(rank.error->code.value()) : Json(nullptr)}});
                attempts.push_back(std::move(ranks));
            }
            block["warmup"].push_back({{"role", role.role},
                                       {"status", control::disposition(role.status)},
                                       {"attempts", std::move(attempts)}});
        }
    }
    block["replicas"] = Json::array();
    for (const auto& group_evidence : group->replicas) {
        auto owners = Json::array(), regions = Json::array();
        for (const auto& owner : group_evidence.owners)
            owners.push_back(
                {{"role", owner.role}, {"rank", owner.rank}, {"node", owner.node.value()}});
        for (const auto& region : group_evidence.regions)
            regions.push_back({{"name", region.name},
                               {"bytes", region.bytes},
                               {"content_sha256", region.content_sha256.to_hex()}});
        block["replicas"].push_back(
            {{"group", group_evidence.group},
             {"policy", runtime::package_input_policy_name(group_evidence.policy)},
             {"owners", std::move(owners)},
             {"regions", std::move(regions)},
             {"error",
              group_evidence.error ? Json(group_evidence.error->code.value()) : Json(nullptr)}});
    }
    block["collectives"] = Json::array();
    for (const auto& team : group->collectives) {
        block["collectives"].push_back(
            {{"role", team.role},
             {"owner_rank", team.owner_rank},
             {"begin_ns", optional(team.begin_ns)},
             {"end_ns", optional(team.end_ns)},
             {"overlap_begin_ns", optional(team.overlap_begin_ns)},
             {"overlap_end_ns", optional(team.overlap_end_ns)},
             {"cancelled", team.cancelled},
             {"verify_status", team.verify_status},
             {"expected", encode_work(team.expected)},
             {"actual", encode_work(team.actual)},
             {"regions", encode_regions(team.regions)},
             {"error", team.error ? Json(team.error->code.value()) : Json(nullptr)}});
        if (team.task_observation)
            block["collectives"].back()["task_observation"] = *team.task_observation;
        if (team.finalization) {
            const auto& f = *team.finalization;
            block["collectives"].back()["finalization"] = {
                {"release_ns", optional(f.release_ns)},
                {"scan_end_ns", optional(f.scan_end_ns)},
                {"begin_ns", optional(f.begin_ns)},
                {"end_ns", optional(f.end_ns)},
                {"execute_status", f.execute_status},
                {"actual", encode_work(f.actual)},
                {"error", f.error ? Json(f.error->code.value()) : Json(nullptr)}};
            if (!f.monitoring.is_null())
                block["collectives"].back()["finalization"]["monitoring"] = f.monitoring;
        }
    }
    return Json{{"block", std::move(block)},
                {"ordinal", ordinal},
                {"plan_digest", digest},
                {"supplied", group->supplied},
                {"verified", group->valid()}};
}
} // namespace

int serve_package_runner() {
    return platform_linux::package_process::serve(
        [](const Json& request) -> platform_linux::package_process::Block {
            return request.contains("operation") ? runtime::inspect_in_runner(request)
                                                 : execute_block(request);
        });
}

} // namespace epycbench::runtime
