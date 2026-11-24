#include "experiment/resolved_population.hpp"
#include "epycbench/scenario/identifier.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/experiment/coordinator.hpp"

#include <limits>
#include <set>
#include <stdexcept>

namespace epycbench::experiment {
namespace {
using Json = nlohmann::json;
constexpr std::size_t rejection_pointer_bytes = 1024U;
struct Invalid {};
auto failure(std::string_view reason) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{.category=core::ErrorCategory::results,
        .code=core::StableErrorCode{"experiment.population.invalid"},
        .message="Cannot retain the finite resolved population", .operation="experiment.population",
        .os_error={}, .context={{"reason",reason}}}};
}
void require(bool condition) { if (!condition) throw Invalid{}; }
void fields(const Json& value, std::initializer_list<std::string_view> keys) {
    require(value.is_object() && value.size()==keys.size());
    for (const auto key:keys) require(value.contains(key));
}
auto text(const Json& value, std::size_t maximum, bool empty=false) -> const std::string& {
    require(value.is_string());
    const auto& result=value.get_ref<const std::string&>();
    require((empty || !result.empty()) && result.size()<=maximum && result.find('\0')==std::string::npos);
    return result;
}
auto number(const Json& value) -> std::size_t {
    require(value.is_number_unsigned());
    const auto result=value.get<std::uint64_t>();
    require(result<=std::numeric_limits<std::size_t>::max());
    return static_cast<std::size_t>(result);
}
void digest(const Json& value) {
    const auto& raw=text(value,64U);
    const auto parsed=core::Sha256Digest::parse(raw);
    require(parsed && parsed->to_hex()==raw);
}
void validate(const Json& value) {
    require(value.at("profile") == package_population_profile);
    fields(value, {"profile","space","topology","objective","candidate_count","axis_cardinalities",
                   "problem_sha256","entries","host_build_sha256"});
    digest(value.at("host_build_sha256"));
    fields(value.at("space"),{"format","sha256"});
    const auto& format=value.at("space").at("format");
    require(format=="yaml" || format=="json");
    digest(value.at("space").at("sha256"));
    fields(value.at("topology"),{"sha256","physical","effective"});
    digest(value.at("topology").at("sha256"));
    const auto& physical=text(value.at("topology").at("physical"),256U);
    const auto& effective=text(value.at("topology").at("effective"),256U);
    const auto parsed_physical=topology::PhysicalTopologyFingerprint::parse(physical);
    const auto parsed_effective=topology::EffectiveTopologyFingerprint::parse(effective);
    require(parsed_physical && parsed_physical->to_string()==physical);
    require(parsed_effective && parsed_effective->to_string()==effective);
    require(value.at("objective")=="tasks_per_second" || value.at("objective")=="seconds_per_completed_task");
    const auto count=number(value.at("candidate_count"));
    require(count>0U && count<=maximum_attempts);
    const auto& axes=value.at("axis_cardinalities");
    require(axes.is_array() && axes.size()<=maximum_attempts);
    std::size_t product=1U;
    for (const auto& axis:axes) {
        const auto cardinality=number(axis);
        require(cardinality>0U && cardinality<=maximum_attempts && product<=maximum_attempts/cardinality);
        product*=cardinality;
    }
    require(product==count);
    const auto& entries=value.at("entries");
    require(entries.is_array() && entries.size()==count);
    Json common_problem=nullptr;
    for (std::size_t ordinal{};ordinal<count;++ordinal) {
        const auto& row=entries.at(ordinal);
        require(number(row.at("ordinal"))==ordinal);
        if (row.at("state")=="rejected") {
            fields(row,{"ordinal","state","rejection"});
            fields(row.at("rejection"),{"code","pointer"});
            static_cast<void>(text(row.at("rejection").at("code"),1024U));
            static_cast<void>(text(row.at("rejection").at("pointer"),rejection_pointer_bytes,true));
            require(row.at("rejection").at("pointer").dump().size()<=rejection_pointer_bytes);
            continue;
        }
        require(row.at("state")=="resolved");
        fields(row,{"ordinal","state","input_sha256","problem_sha256","candidate_sha256","original_resolved_plan_digest","packages"});
        for (const auto* key:{"input_sha256","problem_sha256","candidate_sha256"}) digest(row.at(key));
        if (common_problem.is_null()) common_problem=row.at("problem_sha256");
        require(common_problem==row.at("problem_sha256"));
        const auto& resolved=text(row.at("original_resolved_plan_digest"),71U);
        require(resolved.size() == 71U && resolved.starts_with("sha256:"));
        digest(resolved.substr(7));
        const auto& selections=row.at("packages");

        require(selections.is_array() && !selections.empty() && selections.size() <= 64U);
        std::set<std::string_view> roles;
        for (const auto& selection : selections) {
            fields(selection, {"role", "task_manifest_sha256", "oracle_sha256",
                               "implementation_manifest_sha256", "module_sha256"});
            const auto& role = text(selection.at("role"), 128U);
            require(scenario::ScenarioIdentifier::parse(role).has_value() && roles.insert(role).second);
            for (const auto* key : {"task_manifest_sha256", "oracle_sha256",
                                    "implementation_manifest_sha256", "module_sha256"})
                digest(selection.at(key));
        }
    }
    require(value.at("problem_sha256")==common_problem);
}
auto bounded_pointer(std::string_view pointer) -> std::string {
    if (pointer.size()>rejection_pointer_bytes || Json(pointer).dump().size()>rejection_pointer_bytes) return {};
    return std::string{pointer};
}

auto project_entry(const scenario::ConfigurationCandidate& candidate) -> std::expected<Json, core::Error> {
    Json row{{"ordinal", candidate.ordinal}};
    if (!candidate.accepted()) {
        const auto& diagnostics = candidate.diagnostics;
        row["state"] = "rejected";
        row["rejection"] = {{"code", diagnostics.empty() ? "experiment.plan.unresolved" : diagnostics.front().code.value()},
            {"pointer", diagnostics.empty() ? std::string{} : bounded_pointer(diagnostics.front().json_pointer)}};
        return row;
    }
    if (!candidate.input_identity || !candidate.problem_identity || !candidate.candidate_identity)
        return failure("resolved_identity_missing");
    row["state"] = "resolved";
    row["input_sha256"] = candidate.input_identity->to_hex();
    row["problem_sha256"] = candidate.problem_identity->to_hex();
    row["candidate_sha256"] = candidate.candidate_identity->to_hex();
    if (!candidate.package) return failure("package_plan_missing");
    const auto& plan = *candidate.package;
    row["original_resolved_plan_digest"] = "sha256:" + plan.resolved_digest.to_hex();
    auto& selections = row["packages"] = Json::array();
    for (const auto& role : plan.evidence.at("scenario").at("roles")) {
        const auto& id = role.at("id").get_ref<const std::string&>();
        const auto& binding = plan.evidence.at("bindings").at(id);
        Json entry{{"role", id}};
        for (const auto* key : {"task_manifest_sha256", "oracle_sha256",
                                "implementation_manifest_sha256", "module_sha256"})
            entry[key] = binding.at(key);
        selections.push_back(std::move(entry));
    }
    return row;
}
} // namespace

auto ResolvedPopulation::decode(std::string_view bytes) -> std::expected<ResolvedPopulation,core::Error> {
    if (bytes.size()>resolved_population_maximum_bytes) return failure("byte_bound");
    try {
        std::size_t nodes{};
        auto value=Json::parse(bytes,[&](int depth,Json::parse_event_t,Json&) {
            if (depth>16 || ++nodes>128U*maximum_attempts+256U) throw Invalid{};
            return true;
        });
        validate(value);
        auto canonical=core::canonicalize_json(value);
        if (!canonical) return std::unexpected{canonical.error()};
        if (*canonical!=bytes) return failure("canonical_encoding");
        return ResolvedPopulation{std::move(value),std::move(*canonical)};
    } catch (const Invalid&) { return failure("shape_or_identity"); }
      catch (const Json::exception&) { return failure("shape_or_identity"); }
}

auto ResolvedPopulation::resolve(const scenario::ScenarioSource& source,const topology::LoadedTopology& machine,
                                 BuildDigest host_build,
                                 const kernel::packages::Catalog* packages)
    -> std::expected<ResolvedPopulation,core::Error> {
    if (source.contents.size()>(1U<<20U) || machine.canonical_json.empty() || machine.canonical_json.size()>(8U<<20U)) return failure("input_bound");
    auto space=scenario::ConfigurationSpace::parse(source,maximum_attempts);
    if (!space) return std::unexpected{space.error()};
    if (space->varies_task_parameters()) return failure("variable_task_problem");
    Json value{{"profile",package_population_profile},
        {"space",{{"format",source.format==scenario::ScenarioFormat::yaml?"yaml":"json"},
                  {"sha256",core::sha256(source.contents).to_hex()}}},
        {"topology",{{"sha256",core::sha256(machine.canonical_json).to_hex()},
                     {"physical",machine.fingerprints.physical.to_string()},
                     {"effective",machine.fingerprints.effective.to_string()}}},
        {"objective",scenario::objective_token(space->objective())},{"candidate_count",space->size()},
        {"axis_cardinalities",space->axis_cardinalities()},{"problem_sha256",nullptr},{"entries",Json::array()}};
    {
        if (!host_build) return failure("host_build_required");
        auto build = host_build();
        if (!build) return std::unexpected{build.error()};
        value["host_build_sha256"] = build->to_hex();
    }
    const scenario::ResourceResolutionContext context{machine.topology,machine.stable_ids,machine.fingerprints,packages};
    std::size_t row_bytes{};
    for (std::size_t ordinal{};ordinal<space->size();++ordinal) {
        auto candidate=space->resolve(ordinal,context);
        if (!candidate) return std::unexpected{candidate.error()};
        auto row = project_entry(*candidate);
        if (!row) return std::unexpected{std::move(row.error())};
        if (candidate->accepted()) {
            if (value["problem_sha256"].is_null()) value["problem_sha256"] = row->at("problem_sha256");
            if (value["problem_sha256"] != row->at("problem_sha256")) return failure("different_task_problems");
        }
        const auto size=row->dump().size();
        if (size>resolved_population_maximum_bytes-row_bytes) return failure("byte_bound");
        row_bytes+=size;
        value["entries"].push_back(std::move(*row));
    }
    auto canonical=core::canonicalize_json(value);
    if (!canonical) return std::unexpected{canonical.error()};
    return decode(*canonical);
}

auto ResolvedPopulation::verify_resolution(std::size_t ordinal, const scenario::ConfigurationCandidate& candidate) const
    -> std::expected<void, core::Error> {
    const auto& entries = document_.at("entries");
    if (ordinal >= entries.size() || candidate.ordinal != ordinal) return failure("ordinal_mismatch");
    auto resolved = project_entry(candidate);
    if (!resolved) return std::unexpected{std::move(resolved.error())};
    if (*resolved != entries.at(ordinal)) return failure("resolution_changed");
    return {};
}

auto ResolvedPopulation::required_host_build() const -> std::optional<core::Sha256Digest> {
    const auto bound = document_.find("host_build_sha256");
    if (bound == document_.end()) return {};
    return *core::Sha256Digest::parse(bound->get_ref<const std::string&>());
}
} // namespace epycbench::experiment
