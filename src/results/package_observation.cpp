#include "results/package_observation.hpp"
#include "kernel/package_layout_rules.hpp"
#include "measurement/package_control_json.hpp"
#include "results/package_stream.hpp"
#include "results/package_worker_counter_wire.hpp"
#include "scenario/package_order.hpp"
#include "platform_linux/hugetlb_backing.hpp"

#include <array>
#include <bit>
namespace epycbench::results::package_observation {
using Digest = core::Sha256Digest;
namespace control = measurement::package_control;
auto invalid(std::string reason) -> core::Error {
    return {core::ErrorCategory::results,
            core::StableErrorCode{"result.package.invalid"},
            std::move(reason),
            "validate_package_result",
            {},
            Json::object(),
            core::ErrorSeverity::error,
            core::ContinuationPolicy::abort_command};
}

auto hex(const Json& value) -> bool {
    return value.is_string() && Digest::parse(value.get_ref<const std::string&>()).has_value();
}

bool unsigned_value(const Json& value) { return value.is_number_unsigned(); }

bool trial_shape(const Json& trial) {
    if (!trial.is_object())
        return false;
    const auto& expected = trial.at("expected");
    const auto& actual = trial.at("actual");
    if (!trial.at("execute_status").is_number_integer() ||
        !trial.at("verify_status").is_number_integer() ||
        !(trial.at("timing_error").is_null() || trial.at("timing_error").is_string()) ||
        !(trial.at("begin_ns").is_null() || unsigned_value(trial.at("begin_ns"))) ||
        !(trial.at("end_ns").is_null() || unsigned_value(trial.at("end_ns"))) ||
        !unsigned_value(trial.at("partition_begin")) ||
        !unsigned_value(trial.at("partition_count")) || !trial.at("regions").is_array() ||
        trial.at("regions").size() > EPYCBENCH_PACKAGE_MAX_REGIONS)
        return false;
    for (const auto* key : {"repetitions", "work_units", "output_count"})
        if (!unsigned_value(expected.at(key)) || !unsigned_value(actual.at(key)))
            return false;
    for (const auto& region : trial.at("regions")) {
        if (!region.at("name").is_string() || !unsigned_value(region.at("kind")) ||
            !unsigned_value(region.at("bytes")) || !unsigned_value(region.at("alignment")))
            return false;
        for (const char* name : {"before", "after", "reference"})
            if (!region.at(name).is_null() && !hex(region.at(name)))
                return false;
    }
    return true;
}

auto verified(const Json& trial, U64 repetitions, bool empty_partition) -> bool {
    const auto& expected = trial.at("expected");
    const auto& actual = trial.at("actual");
    if (!trial_shape(trial))
        return false;
    if (!trial.at("timing_error").is_null() || trial.at("execute_status") != 0 ||
        trial.at("verify_status") != 0 || expected != actual ||
        expected.at("repetitions") != repetitions ||
        (expected.at("work_units") == 0 && !empty_partition) ||
        !unsigned_value(trial.at("begin_ns")) || !unsigned_value(trial.at("end_ns")) ||
        trial.at("end_ns").get<U64>() <= trial.at("begin_ns").get<U64>())
        return false;
    std::set<std::string> names;
    for (const auto& region : trial.at("regions")) {
        if (!names.insert(region.at("name").get<std::string>()).second ||
            !unsigned_value(region.at("bytes")) || !unsigned_value(region.at("alignment")) ||
            region.at("alignment") == 0)
            return false;
        const auto& kind = region.at("kind");
        if (kind == EPYCBENCH_PACKAGE_INPUT) {
            if (!hex(region.at("before")) || region.at("before") != region.at("after"))
                return false;
        } else if (kind == EPYCBENCH_PACKAGE_OUTPUT) {
            // Numerical agreement can be task-defined. Retain both byte hashes and the oracle
            // verdict.
            if (!hex(region.at("after")) || !hex(region.at("reference")))
                return false;
        } else if (kind != EPYCBENCH_PACKAGE_SCRATCH)
            return false;
    }
    return true;
}

auto verified_mapping(const Json& admitted, const Json& cpu, const Json& rank) -> bool {
    const auto& mapping = rank.at("mapping");
    const auto& placement = rank.at("placement");
    const auto& trial = rank.at("trial");
    for (const auto& [key, value] : admitted.items())
        if (mapping.at(key) != value) return false;
    if (mapping.at("release_succeeded") != true || mapping.at("mapping_succeeded") != true ||
        mapping.at("invalid_request") != false || mapping.at("worker_retired") != true ||
        !mapping.at("failures").is_array() || !mapping.at("failures").empty() ||
        mapping.at("service_cpu_observed") != admitted.at("service_cpu") ||
        placement.at("verified") != true || placement.at("cpu") != cpu ||
        placement.at("cpu_before") != cpu || placement.at("cpu_after") != cpu ||
        placement.at("numa_node") != admitted.at("linux_numa_node") ||
        placement.at("page_bytes") != admitted.at("page_bytes") ||
        placement.at("regions") != Json::array() || trial.at("regions") != Json::array() ||
        trial.at("partition_begin") != 0 || trial.at("partition_count") != 0 ||
        rank.at("transfers") != Json::array() || !rank.at("hardware_counters").is_null() ||
        !rank.at("error").is_null() || !rank.at("cleanup_error").is_null() ||
        !verified(trial, 1U, false)) return false;
    const auto& pages = admitted.at("pages");
    const auto& before = mapping.at("faults_before");
    const auto& after = mapping.at("faults_after");
    const auto& backing = mapping.at("hugetlb_backing");
    const auto& residency = mapping.at("residency");
    const bool touch = pages != 0;
    const Json work{{"repetitions", 1U}, {"work_units", touch ? pages : Json(1U)}, {"output_count", 0U}};
    if (trial.at("expected") != work || mapping.at("attempted_pages") != pages ||
        mapping.at("verified_pages") != pages || mapping.at("unresolved_pages") != 0) return false;
    if (!touch)
        return mapping.at("requests") == 1 && mapping.at("binding_succeeded").is_null() &&
               backing.is_null() && before.is_null() && after.is_null() && residency == Json::array();
    if (mapping.at("requests") != 0 || mapping.at("binding_succeeded") != true ||
        residency != Json::array({{{"node", admitted.at("linux_numa_node")}, {"pages", pages}}})) return false;
    const auto valid_fault_read = [](const Json& read) {
        if (!read.is_object()) return false;
        if (read.contains("error")) {
            const auto& code = read.at("error");
            return read.size() == 1 && code.is_string() && code != "linux.task_stat.malformed" &&
                code != "linux.task_stat.counter_out_of_range" && code != "linux.task_stat.identity_mismatch";
        }
        return read.size() == 2 && unsigned_value(read.at("minor")) && unsigned_value(read.at("major"));
    };
    if (!valid_fault_read(before) || !valid_fault_read(after) ||
        (!before.contains("error") && !after.contains("error") &&
         (after.at("minor") < before.at("minor") || after.at("major") != before.at("major")))) return false;
    if (admitted.at("page_mode") == "base") return backing.is_null();
    if (!backing.is_object()) return false;
    const platform_linux::HugeTlbBacking observed{backing.at("extent_bytes").get<U64>(),
        backing.at("kernel_page_bytes").get<U64>(), backing.at("mmu_page_bytes").get<U64>(),
        backing.at("private_hugetlb_bytes").get<U64>(), backing.at("shared_hugetlb_bytes").get<U64>(),
        backing.at("hugetlb_vma").get<bool>()};
    return observed.qualifies(admitted.at("requested_bytes").get<U64>());
}

bool valid_geometry(const Json& row, bool owns_shared_regions) {
    for (const auto* field :
         {"partition_begin", "partition_count", "preparation_bytes", "shared_preparation_bytes"})
        if (!unsigned_value(row.at(field)))
            return false;
    if (row.at("partition_count").get<U64>() >
            std::numeric_limits<U64>::max() - row.at("partition_begin").get<U64>() ||
        row.at("preparation_bytes").get<U64>() > std::numeric_limits<std::ptrdiff_t>::max() ||
        row.at("shared_preparation_bytes").get<U64>() >
            std::numeric_limits<std::ptrdiff_t>::max() ||
        (!owns_shared_regions &&
         (row.at("shared_preparation_bytes") != 0 || !row.at("shared_regions").empty())))
        return false;
    if (row.contains("task_inspection") && (!row.at("task_inspection").is_object() ||
                                            row.at("task_inspection").dump().size() > 65536U))
        return false;
    std::set<std::string> names;
    for (const auto* field : {"regions", "shared_regions"}) {
        const auto& regions = row.at(field);
        if (!regions.is_array() || regions.size() > EPYCBENCH_PACKAGE_MAX_REGIONS)
            return false;
        for (const auto& r : regions) {
            const auto& name = r.at("name").get_ref<const std::string&>();
            for (const auto* key : {"kind", "bytes", "alignment"})
                if (!unsigned_value(r.at(key)))
                    return false;
            if (!names.insert(name).second ||
                !kernel::packages::valid_package_region(
                    name, r.at("kind").get<U64>(), r.at("bytes").get<U64>(),
                    r.at("alignment").get<U64>(), std::string_view{field} == "shared_regions"))
                return false;
        }
    }
    return true;
}

const Json& rank_node(const Json& role, std::size_t rank) {
    return role.contains("numa_nodes") ? role.at("numa_nodes").at(rank) : role.at("numa_node");
}

bool residency(const Json& location, const Json& targets, U64 pages, bool interleave,
               bool supplied) {
    if (location.at("targets") != targets || location.at("node") != targets.at(0) ||
        location.at("pages") != pages || location.at("resident_before") != pages ||
        location.at("resident_after") != pages || location.at("unresolved_before") != 0 ||
        location.at("unresolved_after") != 0)
        return false;
    const auto& observations = location.at("distribution");
    if (!observations.is_array() || observations.size() > targets.size())
        return false;
    if (observations.empty())
        return supplied && targets.size() == 1U && location.at("policy") == "bind";
    if (interleave &&
        (targets.size() < 2U || pages < targets.size() || observations.size() != targets.size()))
        return false;
    const auto lower = pages / targets.size();
    const auto upper = lower + (pages % targets.size() != 0);
    U64 before{}, after{};
    std::set<Json> seen;
    for (const auto& row : observations) {
        const auto& node = row.at("node");
        if (std::ranges::find(targets, node) == targets.end() || !seen.insert(node).second ||
            !unsigned_value(row.at("before")) || !unsigned_value(row.at("after")))
            return false;
        const auto first = row.at("before").get<U64>(), last = row.at("after").get<U64>();
        if (first > pages - before || last > pages - after ||
            (interleave && (first < lower || first > upper || last < lower || last > upper)))
            return false;
        before += first;
        after += last;
    }
    return before == pages && after == pages;
}

bool placed(const Json& placement, const Json& role, std::size_t rank, const Json& regions,
            const Json& shared_node, bool shared_cpu_local, bool supplied) {
    const auto& cpu = role.at("cpus").at(rank);
    const auto& node = rank_node(role, rank);
    if (placement.at("verified") != true || placement.at("cpu") != cpu ||
        placement.at("cpu_before") != cpu || placement.at("cpu_after") != cpu ||
        placement.at("numa_node") != node || !unsigned_value(placement.at("page_bytes")))
        return false;
    const auto page = placement.at("page_bytes").get<U64>();
    const auto& locations = placement.at("regions");
    if (!std::has_single_bit(page) || !locations.is_array() || locations.size() != regions.size())
        return false;
    const auto& input = role.at("input_placement");
    const auto& requested = input.at("policy");
    const bool replicas = requested == "replica_per_worker" || requested == "replica_per_node";
    for (const auto& region : regions) {
        const auto bytes = region.at("bytes").get<U64>();
        if (!bytes)
            return false;
        const auto kind = region.at("kind").get<unsigned>();
        const bool shared =
            kind == EPYCBENCH_PACKAGE_SHARED || kind == EPYCBENCH_PACKAGE_SHARED_INPUT;
        const bool selected =
            kind == EPYCBENCH_PACKAGE_INPUT ||
            (kind == EPYCBENCH_PACKAGE_SHARED_INPUT && !shared_cpu_local && !replicas);
        const auto targets = selected && input.contains("nodes")
                                 ? input.at("nodes")
                                 : Json::array({shared ? shared_node : node});
        const auto location = std::ranges::find_if(
            locations, [&](const auto& item) { return item.at("name") == region.at("name"); });
        const auto& policy = selected ? requested : Json("bind");
        if (location == locations.end() || location->at("policy") != policy ||
            !residency(*location, targets, 1U + (bytes - 1U) / page, policy == "interleave",
                       supplied) ||
            (policy == "interleave" && bytes % page != 0))
            return false;
        if (selected && replicas) {
            std::size_t owner = rank;
            if (requested == "replica_per_node") {
                owner = 0;
                while (rank_node(role, owner) != node)
                    ++owner;
            }
            if (location->at("owner_rank") != owner)
                return false;
        } else if (!location->at("owner_rank").is_null())
            return false;
    }
    return true;
}

using Ranks = std::map<RankKey, const Json*>;

// Physical owners and logical readers are distinct for per-node replicas. Bind the
// group's content hashes to the bytes every rank actually consumed.
bool replica_evidence(const Json& block, const Json& roles, const Ranks& ranks) {
    struct Replica {
        Json policy;
        std::set<Json> owners;
        std::set<Json> nodes;
        std::set<Json> regions;
    };
    std::map<std::string, Replica> groups;
    for (const auto& [key, row] : ranks) {
        const auto& role = roles.at(key.first);
        const auto placement = role.find("input_placement");
        if (placement == role.end() || !placement->contains("replica_group"))
            continue;
        const auto& policy = placement->at("policy");
        auto& group = groups[placement->at("replica_group").get<std::string>()];
        if (!group.policy.is_null() && group.policy != policy)
            return false;
        group.policy = policy;
        const auto& node = rank_node(role, key.second);
        const auto& locations = row->at("placement").at("regions");
        std::set<Json> inputs;
        for (const auto& region : row->at("trial").at("regions")) {
            if (region.at("kind") != EPYCBENCH_PACKAGE_INPUT)
                continue;
            inputs.insert(Json{{"name", region.at("name")},
                               {"bytes", region.at("bytes")},
                               {"content_sha256", region.at("before")}});
            const auto actual = std::ranges::find_if(
                locations, [&](const auto& item) { return item.at("name") == region.at("name"); });
            if (actual == locations.end())
                return false;
            if (actual->at("owner_rank") == key.second) {
                const auto owner =
                    Json{{"role", role.at("id")}, {"rank", key.second}, {"node", node}};
                if (group.owners.insert(owner).second && policy == "replica_per_node" &&
                    !group.nodes.insert(node).second)
                    return false;
            }
        }
        if (inputs.empty() || (!group.regions.empty() && group.regions != inputs))
            return false;
        group.regions = std::move(inputs);
    }
    const auto& observed = block.at("replicas");
    if (!observed.is_array() || observed.size() != groups.size())
        return false;
    for (const auto& group : observed) {
        auto it = groups.find(group.at("group").get<std::string>());
        if (it == groups.end() || it->second.policy != group.at("policy") ||
            !group.at("error").is_null())
            return false;
        const auto matches = [](const auto& entries, const auto& expected) {
            return entries.is_array() && entries.size() == expected.size() &&
                   std::set<Json>(entries.begin(), entries.end()) == expected;
        };
        if (!matches(group.at("owners"), it->second.owners) ||
            !matches(group.at("regions"), it->second.regions))
            return false;
        groups.erase(it);
    }
    return true;
}

bool linked(const Json& roles, const Ranks& ranks) {
    for (const auto& [key, row] : ranks) {
        const auto [role, rank] = key;
        const auto& transfers = row->at("transfers");
        const auto& links = roles.at(role).value("inputs", Json::object());
        if (!transfers.is_array() || transfers.size() != links.size())
            return false;
        for (const auto& [input, output] : links.items()) {
            if (role == 0)
                return false;
            const auto& previous = *ranks.at({role - 1U, rank});
            const auto& origin = previous.at("trial").at("regions");
            const auto& target = row->at("trial").at("regions");
            const auto src = std::ranges::find_if(origin, [&](const auto& r) {
                return r.at("kind") == EPYCBENCH_PACKAGE_OUTPUT && r.at("name") == output;
            });
            const auto dst = std::ranges::find_if(target, [&](const auto& r) {
                return r.at("kind") == EPYCBENCH_PACKAGE_INPUT && r.at("name") == input;
            });
            const auto link = std::ranges::find_if(transfers, [&](const auto& l) {
                return l.at("input") == input && l.at("output") == output;
            });
            if (src == origin.end() || dst == target.end() || link == transfers.end() ||
                link->at("source_role") != previous.at("role") ||
                link->at("bytes") != src->at("bytes") || src->at("bytes") != dst->at("bytes") ||
                link->at("sha256") != dst->at("before") || link->at("sha256") != src->at("after"))
                return false;
        }
    }
    return true;
}

auto input_digests(const Json& trial) -> Json {
    Json values = Json::object();
    for (const auto& region : trial.at("regions"))
        if (region.at("kind") == EPYCBENCH_PACKAGE_INPUT)
            values[region.at("name").get<std::string>()] = region.at("before");
    return values;
}

// A rank-local digest alone cannot identify a fixed input when a candidate
// partitions it differently. The Runner also hashes the concatenated bytes.
bool role_inputs(const Json& block, const Json& roles, const Ranks& ranks,
                 const std::vector<const kernel::packages::TaskManifest*>& tasks,
                 const std::map<std::string, const Json*>& collectives, bool comparing) {
    const auto& proofs = block.at("input_proofs");
    if (!proofs.is_array() || proofs.size() != roles.size())
        return false;
    Json first;
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const auto id = roles[i].at("id").get<std::string>();
        const auto proof =
            std::ranges::find_if(proofs, [&](const auto& p) { return p.at("role") == id; });
        if (proof == proofs.end() || !proof->at("error").is_null() ||
            !proof->at("regions").is_array())
            return false;
        struct Extent {
            U64 bytes{};
            std::size_t fragments{};
            Json sole_digest;
        };
        std::map<std::string, Extent> expected;
        const auto include = [&](const Json& r) {
            auto& field = expected[r.at("name").get<std::string>()];
            const auto bytes = r.at("bytes").get<U64>();
            if (bytes > UINT64_MAX - field.bytes)
                return false;
            field.bytes += bytes;
            ++field.fragments;
            field.sole_digest = r.at("before");
            return true;
        };
        for (std::size_t rank = 0; rank < roles[i].at("cpus").size(); ++rank)
            for (const auto& region : ranks.at({i, rank})->at("trial").at("regions"))
                if (region.at("kind") == EPYCBENCH_PACKAGE_INPUT && !include(region))
                    return false;
        if (const auto team = collectives.find(id); team != collectives.end())
            for (const auto& region : team->second->at("regions")) {
                const auto& name = region.at("name").get_ref<const std::string&>();
                if (region.at("kind") == EPYCBENCH_PACKAGE_SHARED_INPUT &&
                    !std::ranges::contains(tasks[i]->control_inputs, name) && !include(region))
                    return false;
            }
        const auto& regions = proof->at("regions");
        if (regions.size() != expected.size())
            return false;
        Json signature = Json::object();
        for (const auto& r : regions) {
            const auto name = r.at("name").get<std::string>();
            const auto it = expected.find(name);
            if (it == expected.end() || signature.contains(name) ||
                r.at("bytes") != it->second.bytes || !hex(r.at("sha256")) ||
                (it->second.fragments == 1U && r.at("sha256") != it->second.sole_digest))
                return false;
            signature[name] = {{"bytes", r.at("bytes")}, {"sha256", r.at("sha256")}};
        }
        if (comparing && i != 0 && signature != first)
            return false;
        if (i == 0)
            first = std::move(signature);
    }
    return true;
}

auto warmup_role(const Json& evidence, const Json& role, std::size_t index, const Ranks& ranks,
                 U64 repetitions, U64 previous_end, const control::WarmupSettings& settings)
    -> bool {
    std::vector<U64> durations;
    const auto& attempts = evidence.at("attempts");
    if (!attempts.is_array() || attempts.empty() || attempts.size() > settings.maximum_trials)
        return false;
    auto decision = control::warmup_decision(settings, durations);
    for (const auto& attempt : attempts) {
        if (!decision || *decision != control::WarmupStatus::continue_trials ||
            !attempt.is_array() || attempt.size() != role.at("cpus").size())
            return false;
        Interval window;
        std::set<std::size_t> seen;
        for (const auto& observation : attempt) {
            const auto rank = observation.at("rank").get<std::size_t>();
            const auto row = ranks.find({index, rank});
            if (row == ranks.end() || !seen.insert(rank).second ||
                !observation.at("error").is_null())
                return false;
            const auto& trial = observation.at("trial");
            const auto& measured = row->second->at("trial");
            if (!verified(trial, repetitions, false) ||
                trial.at("expected") != measured.at("expected") ||
                trial.at("partition_begin") != measured.at("partition_begin") ||
                trial.at("partition_count") != measured.at("partition_count") ||
                trial.at("regions").size() != measured.at("regions").size())
                return false;
            for (std::size_t i = 0; i < trial.at("regions").size(); ++i) {
                const auto& first = trial.at("regions").at(i);
                const auto& final = measured.at("regions").at(i);
                for (const char* key : {"name", "bytes", "alignment", "kind", "reference"})
                    if (first.at(key) != final.at(key))
                        return false;
                if (first.at("before") != final.at("before"))
                    return false;
            }
            window.include(trial.at("begin_ns").get<U64>(), trial.at("end_ns").get<U64>());
        }
        if (window.begin < previous_end)
            return false;
        previous_end = window.end;
        durations.push_back(window.end - window.begin);
        decision = control::warmup_decision(settings, durations);
    }
    if (!decision || *decision == control::WarmupStatus::continue_trials ||
        *decision == control::WarmupStatus::trial_failed ||
        evidence.at("status") != control::disposition(*decision))
        return false;
    for (std::size_t rank = 0; rank < role.at("cpus").size(); ++rank)
        if (ranks.at({index, rank})->at("trial").at("begin_ns").get<U64>() < previous_end)
            return false;
    return true;
}

bool same_problem(const Json& roles, const Ranks& ranks, const kernel::packages::TaskManifest& task,
                  const std::map<std::string, const Json*>& collectives) {
    std::array<U64, 2> total{};
    std::array<U64, 2> extent{};
    const auto equal_ranks = roles.at(0).at("cpus").size() == roles.at(1).at("cpus").size();
    for (std::size_t role = 0; role < 2; ++role) {
        U64 position{};
        for (std::size_t rank = 0; rank < roles.at(role).at("cpus").size(); ++rank) {
            const auto& trial = ranks.at({role, rank})->at("trial");
            const auto size = trial.at("partition_count").get<U64>();
            // Fixed inputs are contiguous across a rank team. Per-worker tasks
            // each own a complete domain and may begin at the same origin.
            if ((!task.per_worker_input && !task.collective &&
                 trial.at("partition_begin") != position) ||
                size > std::numeric_limits<U64>::max() - position)
                return false;
            position += size;
            if (!task.collective) {
                const auto units = trial.at("expected").at("work_units").get<U64>();
                if (units > std::numeric_limits<U64>::max() - total[role])
                    return false;
                total[role] += units;
            }
            if (role == 1U && equal_ranks) {
                const auto& baseline = ranks.at({0, rank})->at("trial");
                if (trial.at("partition_begin") != baseline.at("partition_begin") ||
                    trial.at("partition_count") != baseline.at("partition_count") ||
                    input_digests(trial) != input_digests(baseline))
                    return false;
            }
        }
        extent[role] = position;
    }
    if (!task.collective)
        return total[0] == total[1] && extent[0] == extent[1];
    const auto& a = *collectives.at(roles[0].at("id").get<std::string>());
    const auto& b = *collectives.at(roles[1].at("id").get<std::string>());
    const auto shared_inputs = [&](const Json& group) {
        Json inputs = Json::object();
        for (const auto& region : group.at("regions")) {
            const auto& name = region.at("name").get_ref<const std::string&>();
            if (region.at("kind") == EPYCBENCH_PACKAGE_SHARED_INPUT &&
                !std::ranges::contains(task.control_inputs, name))
                inputs[name] = region.at("before");
        }
        return inputs;
    };
    return a.at("expected") == b.at("expected") && shared_inputs(a) == shared_inputs(b);
}

auto block_input_identity(const Json& block, const Json& roles) -> std::string {
    Json signatures = Json::array();
    const auto& proof = block.at("input_proofs");
    for (const auto& role : roles) {
        const auto it = std::ranges::find_if(
            proof, [&](const auto& p) { return p.at("role") == role.at("id"); });
        Json inputs = Json::object();
        for (const auto& region : it->at("regions"))
            inputs[region.at("name").get<std::string>()] = {{"bytes", region.at("bytes")},
                                                            {"sha256", region.at("sha256")}};
        signatures.push_back(std::move(inputs));
    }
    return core::sha256(signatures.dump()).to_hex();
}

auto bind_ranks(const Json& plan, const kernel::packages::Catalog& catalog)
    -> std::expected<BoundRanks, core::Error> {
    BoundRanks bound;
    auto& [by_role, geometry, tasks, rank_count, cooperative] = bound;
    const auto& source = plan.at("scenario");
    const auto& roles = source.at("roles");
    const bool streaming = source.at("execution").contains("pipeline_chunks");
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const auto& role = roles.at(i);
        const auto id = role.at("id").get<std::string>();
        const auto& binding = plan.at("bindings").at(id);
        if (!by_role.emplace(id, i).second || !role.at("cpus").is_array() ||
            role.at("cpus").empty() || role.at("cpus").size() > 1024U ||
            plan.at("placement").at(i).at("cpus") != role.at("cpus") ||
            (role.contains("numa_nodes") &&
             plan.at("placement").at(i).at("numa_nodes") != role.at("numa_nodes")) ||
            !hex(binding.at("module_sha256")) || !hex(binding.at("oracle_sha256")))
            return std::unexpected{invalid("Invalid role, placement or binary identity")};
        for (const std::string key : {"task_manifest", "implementation_manifest"})
            if (binding.at(key + "_sha256") !=
                core::sha256(binding.at(key).get_ref<const std::string&>()).to_hex())
                return std::unexpected{invalid("Embedded manifest digest mismatch")};
        const auto& workload = source.at("workloads").at(role.at("workload").get<std::string>());
        const auto* impl = catalog.find(workload.at("task").get<std::string>(),
                                        role.at("implementation").get<std::string>());
        if (!impl)
            return std::unexpected{invalid("Unknown implementation in bound role")};
        if (impl->task->collective)
            cooperative.insert(id);
        tasks.push_back(impl->task.get());
        rank_count += role.at("cpus").size();
    }
    for (const auto& g : plan.at("geometry")) {
        if (streaming) {
            if (!valid_geometry(g, false))
                return std::unexpected{invalid("Invalid frozen streaming geometry")};
            continue;
        }
        const auto it = by_role.find(g.at("role").get<std::string>());
        const auto rank = g.at("rank").get<std::size_t>();
        if (it == by_role.end() || rank >= roles[it->second].at("cpus").size() ||
            !valid_geometry(g, rank == 0 && tasks[it->second]->collective) ||
            !geometry.emplace(RankKey{it->second, rank}, &g).second)
            return std::unexpected{invalid("Invalid frozen worker geometry")};
        const auto& role = roles.at(it->second);
        const auto input = role.find("input_placement");
        if (input != role.end() && input->contains("replica_group")) {
            auto owner = rank;
            if (input->at("policy") == "replica_per_node") {
                owner = 0;
                while (rank_node(role, owner) != rank_node(role, rank))
                    ++owner;
            }
            if (g.at("replica_owner_rank") != owner)
                return std::unexpected{invalid("Invalid frozen replica owner")};
        } else if (g.contains("replica_owner_rank"))
            return std::unexpected{invalid("Unexpected frozen replica owner")};
        if (rank == 0)
            for (const auto& name : tasks[it->second]->control_inputs) {
                const auto& shared = g.at("shared_regions");
                if (!std::ranges::any_of(shared, [&](const auto& region) {
                        return region.at("kind") == EPYCBENCH_PACKAGE_SHARED_INPUT &&
                               region.at("name") == name;
                    }))
                    return std::unexpected{invalid(
                        "Declared control input is missing from immutable shared geometry")};
            }
    }
    if (streaming) {
        if (!cooperative.empty() || !package_stream::valid_geometry(plan))
            return std::unexpected{invalid("Invalid frozen streaming partitions or regions")};
    } else if (geometry.size() != rank_count)
        return std::unexpected{invalid("Missing frozen worker geometry")};
    return bound;
}

auto inspect_bound_block(const Json& plan, const BoundRanks& bound, const Json& block,
                         U64 measured_repetitions, std::size_t ordinal, U64 previous_end, bool warm,
                         std::string_view scope, bool retain_nonoverlap)
    -> std::expected<std::vector<MeasuredRole>, core::Error> {
    const auto& [by_role, geometry, tasks, rank_count, cooperative] = bound;
    const auto& source = plan.at("scenario");
    const auto& roles = source.at("roles");
    const auto& exec = source.at("execution");
    const auto& mode = source.at("mode");
    if (!block.at("ranks").is_array() || block.at("ranks").size() > rank_count ||
        block.at("ordinal") != ordinal)
        return std::unexpected{invalid("Unexpected observation ordinal or rank population")};
    if (!warm && block.contains("warmup"))
        return std::unexpected{invalid("Unrequested warmup in observation")};
    if (block.contains("stream"))
        return std::unexpected{invalid("Stream evidence requires authored chunking")};
    const bool runner_verified = block.at("runner_verified") == true;
    bool valid = (runner_verified || (retain_nonoverlap && mode == "mixed")) &&
                 block.at("ranks").size() == rank_count &&
                 block.at("error").is_null() && block.at("runner_exit") == 0;
    U64 latest_start{}, earliest_end = std::numeric_limits<U64>::max();
    std::vector<Interval> intervals(roles.size());
    Ranks seen;
    std::map<std::string, const Json*> collectives;
    if (block.contains("collectives")) {
        for (const auto& group : block.at("collectives")) {
            const auto id = group.at("role").get<std::string>();
            if (!cooperative.contains(id) || !collectives.emplace(id, &group).second ||
                group.at("owner_rank") != 0)
                return std::unexpected{invalid("Unexpected collective owner")};
            if (group.contains("task_observation") &&
                (!group.at("task_observation").is_object() ||
                 group.at("task_observation").dump().size() > 65536U ||
                 group.at("verify_status") != 0 || !group.at("error").is_null()))
                return std::unexpected{
                    invalid("Unexpected task observation without a verified collective")};
        }
    }
    valid &= collectives.size() == cooperative.size();
    for (const auto& row : block.at("ranks")) {
        const auto role = by_role.find(row.at("role").get<std::string>());
        if (role == by_role.end())
            return std::unexpected{invalid("Unexpected role in measured block")};
        const auto r = role->second;
        const auto rank = row.at("rank").get<std::size_t>();
        const auto& cpus = roles.at(r).at("cpus");
        if (rank >= cpus.size() || row.at("rank_count") != cpus.size() ||
            !seen.emplace(RankKey{r, rank}, &row).second)
            return std::unexpected{invalid("Unexpected or duplicate worker rank")};
        const auto& trial = row.at("trial");
        const auto& placement = row.at("placement");
        const auto& declared = *geometry.at({r, rank});
        if (!trial.is_null() && !trial_shape(trial))
            return std::unexpected{invalid("Malformed observed rank trial")};
        if (tasks[r]->mapping != kernel::packages::MappingResource::none) {
            if (!row.contains("mapping"))
                return std::unexpected{invalid("Missing fresh mapping evidence")};
            if (verified_mapping(plan.at("mapping"), cpus.at(rank), row)) {
                const auto start = trial.at("begin_ns").get<U64>();
                const auto end = trial.at("end_ns").get<U64>();
                valid &= start >= previous_end;
                intervals[r].include(start, end);
            } else valid = false;
            continue;
        }
        if (row.contains("mapping"))
            return std::unexpected{invalid("Unexpected fresh mapping evidence")};
        if (!row.at("error").is_null() || !row.at("cleanup_error").is_null() || trial.is_null() ||
            !verified(trial, measured_repetitions,
                      tasks[r]->collective && declared.at("partition_count") == 0)) {
            valid = false;
            continue;
        }
        Json memory = trial.at("regions");
        if (const auto group = collectives.find(role->first);
            group != collectives.end() && rank == 0U)
            for (const auto& region : group->second->at("regions"))
                memory.push_back(region);
        auto region_list = declared.at("regions");
        for (const auto& region : declared.at("shared_regions"))
            region_list.push_back(region);
        if (memory.size() != region_list.size() ||
            trial.at("partition_begin") != declared.at("partition_begin") ||
            trial.at("partition_count") != declared.at("partition_count")) {
            valid = false;
            continue;
        }
        for (std::size_t j = 0; j < memory.size(); ++j)
            for (const char* field : {"name", "kind", "bytes", "alignment"})
                if (memory[j].at(field) != region_list[j].at(field))
                    valid = false;
        const auto& locality = plan.at("placement").at(r).at("locality").at(rank);
        const auto& shared_node = tasks[r]->shared_cpu_local ? locality.at("linux_numa_node")
                                                             : locality.at("memory_node");
        if (placement.at("page_bytes") != locality.at("base_page_bytes") ||
            !placed(placement, roles.at(r), rank, memory, shared_node, tasks[r]->shared_cpu_local,
                    scope == "supplied")) {
            valid = false;
            continue;
        }
        for (const auto& region : memory) {
            const auto& observed = placement.at("regions");
            const auto entry = std::ranges::find_if(
                observed, [&](const auto& item) { return item.at("name") == region.at("name"); });
            const auto kind = region.at("kind");
            const bool released = scope == "native" && (kind == EPYCBENCH_PACKAGE_INPUT ||
                                                        kind == EPYCBENCH_PACKAGE_SHARED_INPUT);
            valid &= entry != observed.end() &&
                     entry->at("released") == (released ? Json(true) : Json(nullptr));
        }
        if (exec.at("hardware_counters").get<bool>()) {
            const auto evidence = package_worker_counter_wire::decode(
                row.at("hardware_counters"), core::WorkerId{static_cast<std::uint32_t>(rank)},
                trial.at("begin_ns").get<U64>(), trial.at("end_ns").get<U64>(),
                scope == "supplied");
            if (!evidence)
                return std::unexpected{evidence.error()};
        } else if (!row.at("hardware_counters").is_null())
            return std::unexpected{invalid("Unrequested hardware counter evidence")};
        const auto start = trial.at("begin_ns").get<U64>();
        const auto end = trial.at("end_ns").get<U64>();
        if (start < previous_end)
            valid = false;
        latest_start = std::max(latest_start, start);
        earliest_end = std::min(earliest_end, end);
        intervals[r].include(start, end);
    }
    if (valid)
        for (const auto& [id, group] : collectives) {
            const auto i = by_role.at(id);
            U64 start{}, finish = std::numeric_limits<U64>::max();
            for (std::size_t r = 0; r < roles.at(i).at("cpus").size(); ++r) {
                const auto& t = seen.at({i, r})->at("trial");
                start = std::max(start, t.at("begin_ns").get<U64>());
                finish = std::min(finish, t.at("end_ns").get<U64>());
            }
            if (tasks[i]->collective_finalizer) {
                if (!group->contains("finalization")) {
                    valid = false;
                    continue;
                }
                const auto& phase = group->at("finalization");
                if (!phase.at("error").is_null() || phase.at("execute_status") != 0 ||
                    phase.at("actual") != group->at("actual") ||
                    !unsigned_value(phase.at("release_ns")) ||
                    !unsigned_value(phase.at("scan_end_ns")) ||
                    !unsigned_value(phase.at("begin_ns")) || !unsigned_value(phase.at("end_ns")) ||
                    phase.at("release_ns").get<U64>() < previous_end ||
                    phase.at("release_ns").get<U64>() > intervals[i].begin ||
                    intervals[i].end > phase.at("scan_end_ns").get<U64>() ||
                    phase.at("scan_end_ns").get<U64>() > phase.at("begin_ns").get<U64>() ||
                    phase.at("begin_ns").get<U64>() >= phase.at("end_ns").get<U64>()) {
                    valid = false;
                    continue;
                }
                intervals[i] = {phase.at("release_ns").get<U64>(), phase.at("end_ns").get<U64>()};
            } else if (group->contains("finalization"))
                return std::unexpected{invalid("Unexpected collective finalization phase")};
            valid &=
                group->at("cancelled") == false && group->at("error").is_null() &&
                group->at("verify_status") == 0 && group->at("expected") == group->at("actual") &&
                group->at("expected").at("repetitions") == measured_repetitions &&
                group->at("expected").at("work_units").get<U64>() > 0 &&
                group->at("begin_ns") == intervals[i].begin &&
                group->at("end_ns") == intervals[i].end && group->at("overlap_begin_ns") == start &&
                group->at("overlap_end_ns") == finish && start < finish;
            for (const auto& region : group->at("regions"))
                valid &= (region.at("kind") == EPYCBENCH_PACKAGE_SHARED ||
                          (region.at("kind") == EPYCBENCH_PACKAGE_SHARED_INPUT &&
                           region.at("before") == region.at("reference") &&
                           region.at("before") == region.at("after"))) &&
                         hex(region.at("before")) && hex(region.at("after")) &&
                         hex(region.at("reference"));
        }
    if (valid && mode == "mixed") {
        const bool overlap = latest_start < earliest_end;
        valid = (overlap ? runner_verified : retain_nonoverlap && !runner_verified) &&
                block.at("overlap_begin_ns") == latest_start && block.at("overlap_end_ns") == earliest_end;
    }
    if (warm && block.contains("warmup")) {
        const auto& rows = block.at("warmup");
        const auto limit = exec.at("warmup").at("maximum_trials").get<std::size_t>();
        if (!rows.is_array() || rows.size() > roles.size())
            return std::unexpected{invalid("Invalid warmup role population")};
        std::set<std::string> retained_roles;
        for (const auto& role : rows) {
            const auto id = role.at("role").get<std::string>();
            const auto it = by_role.find(id);
            const auto& attempts = role.at("attempts");
            if (it == by_role.end() || !retained_roles.insert(id).second || !attempts.is_array() ||
                attempts.empty() || attempts.size() > limit || !role.at("status").is_string())
                return std::unexpected{invalid("Malformed warmup role or attempt count")};
            const auto count = roles.at(it->second).at("cpus").size();
            std::vector<U64> history;
            const auto settings = control::warmup_settings(exec);
            for (const auto& attempt : attempts) {
                if (!attempt.is_array() || attempt.size() != count)
                    return std::unexpected{invalid("Malformed warmup rank count")};
                std::set<std::size_t> ids;
                Interval interval;
                bool successful = true;
                for (const auto& row : attempt) {
                    const auto rank = row.at("rank").get<std::size_t>();
                    const auto& trial = row.at("trial");
                    if (!unsigned_value(row.at("rank")) || rank >= count ||
                        !ids.insert(rank).second ||
                        !(row.at("error").is_null() || row.at("error").is_string()) ||
                        (!trial.is_null() && !trial_shape(trial)))
                        return std::unexpected{invalid("Malformed retained warmup rank")};
                    if (!row.at("error").is_null() || trial.is_null() ||
                        !verified(trial, measured_repetitions, false))
                        successful = false;
                    else
                        interval.include(trial.at("begin_ns").get<U64>(),
                                         trial.at("end_ns").get<U64>());
                }
                history.push_back(successful ? interval.end - interval.begin : 0U);
            }
            const auto decision = control::warmup_decision(settings, history);
            if (!decision || *decision == control::WarmupStatus::continue_trials ||
                role.at("status") != control::disposition(*decision))
                return std::unexpected{
                    invalid("Warmup disposition differs from the retained rank evidence")};
        }
    }
    if (valid && warm) {
        if (!block.contains("warmup") || !block.at("warmup").is_array() ||
            block.at("warmup").size() != roles.size())
            valid = false;
        else {
            const auto settings = control::warmup_settings(exec);
            std::set<std::size_t> warmed_roles;
            for (const auto& role : block.at("warmup")) {
                const auto it = by_role.find(role.at("role").get<std::string>());
                auto start = previous_end;
                if (it != by_role.end() && mode == "comparison" &&
                    block.at("ranks").front().at("role") != it->first)
                    start = std::max(start, intervals[1U - it->second].end);
                if (it == by_role.end() || !warmed_roles.insert(it->second).second ||
                    !warmup_role(role, roles.at(it->second), it->second, seen, measured_repetitions,
                                 start, settings))
                    valid = false;
            }
        }
    }
    if (valid)
        valid = replica_evidence(block, roles, seen) && linked(roles, seen) &&
                role_inputs(block, roles, seen, tasks, collectives, mode == "comparison");
    if (valid && mode == "pipeline")
        for (std::size_t i = 1; i < intervals.size(); ++i)
            valid &= intervals[i - 1].end <= intervals[i].begin;
    if (valid && mode == "comparison") {
        const auto& a = intervals.at(0);
        const auto& b_interval = intervals.at(1);
        valid = (a.end <= b_interval.begin || b_interval.end <= a.begin) &&
                same_problem(roles, seen, *tasks.front(), collectives);
        auto assigned = scenario::package_candidate_first(exec.value("order_seed", U64{}), ordinal);
        if (!assigned)
            return std::unexpected{assigned.error()};
        const bool reversed = *assigned;
        valid &= block.at("ranks").front().at("role") == roles[reversed ? 1U : 0U].at("id");
        if (scope == "native")
            valid &= reversed ? b_interval.end <= a.begin : a.end <= b_interval.begin;
    }
    std::vector<MeasuredRole> measured;
    if (!valid)
        return measured;
    for (std::size_t i = 0; i < roles.size(); ++i) {
        U64 units{};
        if (tasks[i]->collective)
            units = collectives.at(roles[i].at("id").get<std::string>())
                        ->at("actual")
                        .at("work_units")
                        .get<U64>();
        else
            for (std::size_t rank = 0; rank < roles[i].at("cpus").size(); ++rank) {
                const auto count =
                    seen.at({i, rank})->at("trial").at("expected").at("work_units").get<U64>();
                if (count > std::numeric_limits<U64>::max() - units)
                    return std::unexpected{invalid("Role work count overflow")};
                units += count;
            }
        measured.push_back({intervals[i], units});
    }
    return measured;
}

} // namespace epycbench::results::package_observation
