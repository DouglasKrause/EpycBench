#include "monitoring/host_production_authority.hpp"

#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"

#include <array>
#include <limits>
#include <new>
#include <stdexcept>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Json = nlohmann::json;
using Failure = HostProductionAuthorityFailure;
constexpr std::size_t original_limit = 10U * 1024U * 1024U;
constexpr std::size_t build_limit = 1024U * 1024U;
constexpr std::string_view schema = "epycbench.monitoring-production-authority/v2";
// Inputs have already passed Core UTF-8 validation. Count and emission share the
// byte decoder, but the independent Python literal oracle tests its semantics.
template<class Emit> void escaped(std::string_view input, bool json_body, Emit&& emit) {
    constexpr std::string_view hex = "0123456789abcdef";
    auto unit = [&](std::uint32_t value) {
        emit('\\'); emit('u');
        for (int shift : {12, 8, 4, 0}) emit(hex[(value >> shift) & 15U]);
    };
    for (std::size_t i = 0; i < input.size();) {
        const auto byte = static_cast<unsigned char>(input[i++]);
        if (byte < 128U) {
            if (byte == 127U || (!json_body && byte < 32U)) {
                if (!json_body && (byte == 8U || byte == 9U || byte == 10U || byte == 12U || byte == 13U)) {
                    emit('\\'); emit(byte == 8U ? 'b' : byte == 9U ? 't' : byte == 10U ? 'n' : byte == 12U ? 'f' : 'r');
                } else unit(byte);
            } else {
                if (!json_body && (byte == '"' || byte == '\\')) emit('\\');
                emit(static_cast<char>(byte));
            }
            continue;
        }
        unsigned int continuation{};
        std::uint32_t code{};
        if (byte >= 0xc2U && byte <= 0xdfU) { continuation = 1; code = byte & 31U; }
        else if (byte >= 0xe0U && byte <= 0xefU) { continuation = 2; code = byte & 15U; }
        else if (byte >= 0xf0U && byte <= 0xf4U) { continuation = 3; code = byte & 7U; }
        else throw std::invalid_argument{"UTF-8"};
        if (continuation > input.size() - i) throw std::invalid_argument{"UTF-8"};
        for (unsigned int j = 0; j < continuation; ++j) {
            const auto next = static_cast<unsigned char>(input[i++]);
            if ((next & 0xc0U) != 0x80U) throw std::invalid_argument{"UTF-8"};
            code = (code << 6U) | (next & 63U);
        }
        if ((continuation == 1 && code < 128U) || (continuation == 2 && code < 2048U) ||
            (continuation == 3 && code < 65536U) || code > 0x10ffffU || (code >= 0xd800U && code <= 0xdfffU))
            throw std::invalid_argument{"UTF-8"};
        if (code <= 0xffffU) unit(code);
        else { code -= 0x10000U; unit(0xd800U + (code >> 10U)); unit(0xdc00U + (code & 1023U)); }
    }
}
auto count(std::string_view text, bool body) -> std::size_t {
    std::size_t size{};
    escaped(text, body, [&](char) {
        if (size == std::numeric_limits<std::size_t>::max()) throw std::length_error{"capacity"};
        ++size;
    });
    return size;
}
auto canonical(const Json& value) -> std::string {
    auto result = core::canonicalize_json(value);
    core::detail::ExpectedErrorCleanup cleanup{result};
    if (!result) throw std::invalid_argument{"canonical JSON"};
    std::string ascii;
    ascii.reserve(count(*result, true) + 1U);
    escaped(*result, true, [&](char byte) { ascii.push_back(byte); });
    ascii.push_back('\n');
    return ascii;
}
auto strong(const core::Sha256Digest& value) -> std::string { return "sha256:" + value.to_hex(); }
auto pair(std::string_view first, std::string_view second, Json left, Json right) -> Json {
    core::detail::JsonCleanup left_cleanup{left}, right_cleanup{right};
    return core::detail::json_object<Json>([&](auto& value) {
        core::detail::set_json(value, first, std::move(left));
        core::detail::set_json(value, second, std::move(right));
    });
}
} // namespace

struct HostProductionAuthority::State final {
    std::string envelope, payload, common, association;
};
HostProductionAuthority::HostProductionAuthority(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
HostProductionAuthority::HostProductionAuthority(HostProductionAuthority&&) noexcept = default;
HostProductionAuthority::~HostProductionAuthority() noexcept = default;
auto HostProductionAuthority::view() const noexcept -> std::optional<HostProductionAuthorityView> {
    if (!state_) return {};
    return HostProductionAuthorityView{state_->envelope, state_->payload, state_->common, state_->association};
}
auto HostProductionAuthority::ascii_json(std::string_view input) -> std::string {
    std::string output;
    output.reserve(count(input, true) + 1U);
    escaped(input, true, [&](char byte) { output.push_back(byte); });
    output.push_back('\n'); return output;
}
auto HostProductionAuthority::binding_json(std::string_view payload_hash, const Json& association,
    void (*after_association_clone)() noexcept) -> Json {
    return core::detail::json_object<Json>([&](auto& binding) {
        auto value = core::detail::clone_json(association);
        core::detail::JsonCleanup cleanup{value};
        if (after_association_clone) after_association_clone();
        // Keep both parent and clone guarded before any fallible key or scalar
        // construction. A structured function argument has no caller-side guard.
        core::detail::set_json(binding, "association", std::move(value));
        core::detail::set_json(binding, "payload_sha256", Json(payload_hash));
    });
}
auto HostProductionAuthority::create(const HostFrozenObservationAdmission& admission,
    const MonitoringQ1ValidatedPublicValues& values, const MonitoringRunningBuildAuthority& build,
    std::size_t allowance) noexcept -> std::expected<HostProductionAuthority, Failure> {
    try {
        const auto view = admission.view();
        if (!view || !build.has_value() || values.qualification_id().empty() || values.policy_identity().empty())
            return std::unexpected{Failure::unavailable};
        if (allowance == 0 || allowance > hard_limit) return std::unexpected{Failure::capacity};
        const auto& original = view->original_facts.get();
        const auto& facts = original.json();
        const auto& q0 = values.q0();
        const bool bandwidth = view->family == HostQ0bFamily::bandwidth;
        const auto& family = q0.family_admissions[bandwidth ? 0U : 1U];
        const auto& request = view->request.get();
        const auto& placement = view->placement.get();
        const auto* provenance = std::get_if<runtime::HostQualificationPlanPlacementProvenance>(&request.provenance());
        const auto work = std::visit([](const auto& selected) { return selected.value(); }, view->work.get());
        if (!provenance || values.policy_identity() != provenance->policy_identity().text() ||
            q0.environment_digest != provenance->environment_digest().text() ||
            q0.qualification_plan_digest != provenance->host_qualification_plan_digest().text() ||
            values.profile_sha256() != view->profile_sha256.get().to_hex() ||
            values.profile_sha256() != values.plan().profile_sha256().to_hex() ||
            family.family != (bandwidth ? "sequential-read-bandwidth" : "dependent-load-latency") ||
            family.schema != "epycbench.monitoring-admitted-production-facts/v2" ||
            family.variant_role != "first" || family.sha256 != original.digest() ||
            family.base_environment_sha256 != q0.base_environment_sha256 ||
            q0.base_environment_schema != "epycbench.monitoring-q0b-base-environment/v2" ||
            q0.environment_schema != "epycbench.monitoring-host-environment/v2" ||
            q0.qualification_plan_schema != "epycbench.monitoring-host-qualification-plan/v2" ||
            work == 0 || work != q0.selected_work[bandwidth ? 0U : 1U] ||
            facts.at("admitted_placement_identity").at("sha256") != placement.placement_identity().digest().to_hex() ||
            facts.at("physical_fingerprint") != request.topology().physical_fingerprint().to_string() ||
            facts.at("effective_fingerprint") != request.topology().effective_fingerprint().to_string() ||
            facts.at("provenance").at("template_sha256") != provenance->template_sha256().to_hex())
            return std::unexpected{Failure::binding};
        const auto original_bytes = std::string_view{original.canonical_json()};
        if (original_bytes.empty() || original_bytes.back() != '\n' || original_bytes.size() > original_limit ||
            build.canonical().empty() || build.canonical().size() > build_limit)
            return std::unexpected{Failure::capacity};
        if (core::sha256(original_bytes) != original.digest()) return std::unexpected{Failure::binding};
        auto build_json = Json::parse(build.canonical());
        core::detail::JsonCleanup build_cleanup{build_json};
        const auto& revision = build_json.at("git").at("revision").get_ref<const std::string&>();
        if (revision.empty() || build_json.at("git").at("dirty") != false ||
            build_json.at("build_identity") != build.identity().to_hex()) return std::unexpected{Failure::binding};
        // Exact variable-stock lower bound precedes cloning the original child.
        auto variable = count(original_bytes.substr(0, original_bytes.size() - 1U), true);
        const auto add = [&](std::size_t bytes) {
            if (bytes > std::numeric_limits<std::size_t>::max() - variable) throw std::length_error{"capacity"};
            variable += bytes;
        };
        for (const auto& service : view->service.get().assignments()) {
            add(count(service.stable_core.value(), false)); add(count(service.stable_pu.value(), false));
        }
        add(count(revision, false));
        if (variable > allowance) return std::unexpected{Failure::capacity};
        auto state = std::make_unique<State>();
        auto payload = Json::object(); core::detail::JsonCleanup payload_cleanup{payload};
        const auto set = [&](std::string_view key, Json value) { core::detail::set_json(payload, key, std::move(value)); };
        set("qualification_id", Json(values.qualification_id()));
        set("policy", pair("identity", "sha256", Json(values.policy_identity()), Json(values.policy_sha256())));
        set("input_template", core::detail::json_object<Json>([&](auto& value) {
            core::detail::set_json(value, "sha256", Json(provenance->template_sha256().to_hex()));
        }));
        set("profile", core::detail::json_object<Json>([&](auto& value) {
            core::detail::set_json(value, "schema", Json("epycbench.monitoring-qualification-profile/v2"));
            core::detail::set_json(value, "identity", Json(values.policy_identity()));
            core::detail::set_json(value, "sha256", Json(values.profile_sha256()));
        }));
        set("environment_digest", Json(q0.environment_digest));
        set("host_qualification_plan_digest", Json(q0.qualification_plan_digest));
        set("physical_fingerprint", core::detail::clone_json_as<Json>(facts.at("physical_fingerprint")));
        set("effective_fingerprint", core::detail::clone_json_as<Json>(facts.at("effective_fingerprint")));
        set("baseline", core::detail::json_object<Json>([&](auto& value) {
            core::detail::set_json(value, "repository_commit", Json(q0.repository_commit));
            core::detail::set_json(value, "build_identity", Json(strong(q0.build_identity)));
            core::detail::set_json(value, "base_environment", pair("schema", "sha256", Json(q0.base_environment_schema), Json(q0.base_environment_sha256.to_hex())));
            core::detail::set_json(value, "family_admission", pair("schema", "sha256", Json(family.schema), Json(family.sha256.to_hex())));
        }));
        set("admitted_production_facts", core::detail::clone_json_as<Json>(facts));
        set("repository", pair("commit", "dirty", Json(revision), Json(false)));
        set("build", core::detail::json_object<Json>([&](auto& value) {
            core::detail::set_json(value, "identity", Json(strong(build.identity())));
        }));
        set("production_request", core::detail::json_object<Json>([&](auto& value) {
            core::detail::set_json(value, "kernel_family", Json(family.family));
            core::detail::set_json(value, "fixed_work", Json(work));
            core::detail::set_json(value, "admitted_placement_identity", core::detail::clone_json_as<Json>(facts.at("admitted_placement_identity")));
            core::detail::set_json(value, "execution_inputs", core::detail::json_object<Json>([&](auto& execution) {
                core::detail::set_json(execution, "kernel", core::detail::clone_json_as<Json>(request.wire().at("execution").at("kernel")));
                core::detail::set_json(execution, "measurement", core::detail::clone_json_as<Json>(request.wire().at("execution").at("measurement")));
            }));
        }));
        constexpr std::array names{"C0", "C1", "C2", "C3", "C4"};
        set("monitoring_configuration", Json(names.at(static_cast<std::size_t>(view->configuration))));
        auto association = Json::object(); core::detail::JsonCleanup association_cleanup{association};
        core::detail::set_json(association, "request_id", Json(std::to_string(placement.request_id().value())));
        auto activation = Json::array(); core::detail::JsonCleanup activation_cleanup{activation};
        for (const auto& service : view->service.get().assignments()) core::detail::append_object_json(activation, [&](auto& value) {
            core::detail::set_json(value, "role", Json(service.role == runtime::ServiceRole::control_monitor ? "control_monitor" : "measurement_monitor"));
            core::detail::set_json(value, "stable_core_id", Json(service.stable_core.value()));
            core::detail::set_json(value, "stable_pu_id", Json(service.stable_pu.value()));
            core::detail::set_json(value, "linux_cpu", Json(service.linux_cpu.value()));
        });
        core::detail::set_json(association, "service_activation", std::move(activation));
        state->payload = canonical(payload); state->association = canonical(association);
        const auto payload_hash = core::sha256(state->payload).to_hex();
        auto binding = binding_json(payload_hash, association);
        core::detail::JsonCleanup binding_cleanup{binding};
        const auto binding_hash = core::sha256(canonical(binding)).to_hex();
        auto envelope = core::detail::json_object<Json>([&](auto& value) {
            core::detail::set_json(value, "schema", Json(schema));
            core::detail::set_json(value, "payload", core::detail::clone_json(payload));
            core::detail::set_json(value, "sha256", Json(payload_hash));
            core::detail::set_json(value, "association", core::detail::clone_json(association));
            core::detail::set_json(value, "binding_sha256", Json(binding_hash));
        });
        core::detail::JsonCleanup envelope_cleanup{envelope};
        state->envelope = canonical(envelope);
        if (state->envelope.size() > allowance) return std::unexpected{Failure::capacity};
        payload.erase("monitoring_configuration"); state->common = canonical(payload);
        return HostProductionAuthority{std::move(state)};
    } catch (const std::bad_alloc&) { return std::unexpected{Failure::allocation}; }
      catch (const std::length_error&) { return std::unexpected{Failure::capacity}; }
      catch (...) { return std::unexpected{Failure::contained}; }
}
} // namespace epycbench::monitoring::qualification::detail
