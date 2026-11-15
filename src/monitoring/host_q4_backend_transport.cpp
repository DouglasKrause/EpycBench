#include "monitoring/host_q4_backend_transport.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "monitoring/host_q4_cpufreq_evidence_json.hpp"
#include "monitoring/host_running_build_attestation_value.hpp"
#include "monitoring/privacy_validation.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <set>
namespace epycbench::monitoring::qualification::detail {
namespace {
using Json = nlohmann::ordered_json;
enum class Validation : unsigned char { valid, invalid, terminal };
thread_local void (*transport_fault)(std::size_t){};
void boundary(std::size_t point) {
    if (transport_fault)
        transport_fault(point);
}
auto fail(std::string why) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q4.evidence_invalid"},
            .message = "Q4 backend transport invalid",
            .operation = "run_monitoring_host_q4_backend",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(why)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto phase(MonitoringHostQ4BackendPhase p) -> std::string_view {
    constexpr std::array names{"repository",
                               "collection_plan",
                               "predecessor",
                               "build_attestation",
                               "source_discovery",
                               "oracle",
                               "source_read",
                               "agreement",
                               "clock",
                               "deadline",
                               "capacity",
                               "finalization",
                               "evidence_construction"};
    const auto i = static_cast<std::size_t>(p);
    return i < names.size() ? names[i] : std::string_view{};
}
auto canonical(Json value, std::uint64_t cap) -> std::expected<std::string, core::Error> {
    auto bytes = core::canonicalize_json(value);
    if (!bytes || bytes->size() == std::numeric_limits<std::size_t>::max() ||
        bytes->size() + 1U > cap)
        return std::unexpected{fail("capacity")};
    bytes->push_back('\n');
    return bytes;
}
auto safe(const core::Error& e) -> Json {
    Json out{{"category", core::detail::enum_name(e.category, core::detail::error_category_names)},
             {"code", e.code.value()},
             {"operation", e.operation},
             {"severity", "error"},
             {"continuation", "abort_command"},
             {"context", Json::object()}};
    if (e.os_error && e.os_error->value > 0)
        out["os_error"] = e.os_error->value;
    return out;
}
auto valid_pointer(std::string_view p) -> bool {
    if (p.size() > 4096U)
        return false;
    for (std::size_t i{}; i < p.size(); ++i)
        if (p[i] == '~' && (i + 1U == p.size() || (p[i + 1U] != '0' && p[i + 1U] != '1')))
            return false;
    return p.empty() || p.front() == '/';
}
auto schema(const MonitoringQ4SourceReady& r, std::string_view bytes, MonitoringQ4SchemaKind kind)
    -> std::expected<bool, core::Error> {
    const auto& s = r.authority().governed().schemas();
    const auto held = kind == MonitoringQ4SchemaKind::backend_result  ? s.backend_result_schema()
                      : kind == MonitoringQ4SchemaKind::backend_error ? s.backend_error_schema()
                                                                      : s.backend_authored_schema();
    return validate_monitoring_q4_resolved_schema_instance_typed(bytes, held);
}
auto canonical_input(std::string_view b) -> std::optional<Json> {
    if (b.empty() || b.back() != '\n' || b.find('\0') != std::string_view::npos)
        return {};
    auto j = Json::parse(b.begin(), b.end(), nullptr, false);
    if (j.is_discarded() || !privacy_safe(j))
        return {};
    auto c = core::canonicalize_json(j);
    if (!c || *c + "\n" != b)
        return {};
    return j;
}
auto stage_failure(MonitoringHostQ4BackendPhase p, std::string_view code, int native = 0)
    -> MonitoringHostQ4BackendFailure {
    MonitoringHostQ4BackendFailure out;
    out.phase = p;
    out.underlying_code = code;
    if (native > 0)
        out.native_error = native;
    return out;
}
auto valid_stage_code(MonitoringHostQ4BackendPhase p, std::string_view code) noexcept -> bool {
    if (code.empty()) return true;
    using P=MonitoringHostQ4BackendPhase;
    switch(p){
    case P::repository:return code=="monitoring.q4.repository_invalid";
    case P::collection_plan:return code=="monitoring.q4.collection_plan_invalid";
    case P::predecessor:return code=="monitoring.q4.predecessor_invalid";
    case P::build_attestation:return code=="monitoring.q4.build_attestation_invalid";
    case P::source_discovery:return code=="monitoring.q4.operations_invalid"||code=="linux.cpu_frequency.discovery_failed"||code=="linux.cpu_frequency.ambiguous_policy"||code=="linux.cpu_frequency.read_failed"||code=="linux.cpu_frequency.invalid_value"||code=="monitoring.q4.technical_token_invalid_utf8";
    case P::oracle:return code=="monitoring.q4.oracle_open_failed"||code=="monitoring.q4.oracle_read_failed"||code=="monitoring.q4.oracle_identity_changed"||code=="monitoring.q4.oracle_invalid_value";
    case P::source_read:return code=="linux.cpu_frequency.read_failed"||code=="linux.cpu_frequency.invalid_value";
    case P::agreement:return code=="monitoring.q4.production_parser_rejected"||code=="monitoring.q4.parser_disagreement";
    case P::clock:return code=="monitoring.q4.clock_failed";
    case P::deadline:return code=="monitoring.q4.deadline";
    case P::capacity:return monitoring_q4_capacity_code_is_valid(code);
    case P::finalization:return code=="monitoring.q4.finalization_failed";
    case P::evidence_construction:return code=="monitoring.q4.evidence_invalid";
    }
    return false;
}
auto valid_repository_classification(std::string_view value) noexcept -> bool {
    constexpr std::array allowed{"repository_authority","git_executable","git_identity","git_metadata","git_config","git_result","git_poll","git_read","git_output_limit","git_setup","git_exit","git_spawn","repository_dirty","tracked_entry"};
    return std::ranges::find(allowed,value)!=allowed.end();
}
auto valid_token(std::string_view value,std::size_t maximum) noexcept->bool{
    if(value.empty()||value.size()>maximum||value.front()<'a'||value.front()>'z')return false;
    return std::ranges::all_of(value,[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';});
}
auto valid_failure_value(const MonitoringHostQ4BackendFailure& v) noexcept -> bool {
    if(v.cleanup_count>v.cleanup.size()||(v.cleanup_overflowed&&v.cleanup_count!=31U))return false;
    if(v.usage)return !v.terminal_containment&&v.underlying_code.empty()&&!v.repository_classification&&!v.native_error&&v.cleanup_count==0&&!v.cleanup_overflowed;
    if(v.terminal_containment){if(!v.underlying_code.empty()||v.repository_classification||v.native_error)return false;}
    else if(!valid_stage_code(v.phase,v.underlying_code)||
       (v.native_error&&(*v.native_error<1))||
       (v.repository_classification&&
        (v.phase!=MonitoringHostQ4BackendPhase::repository||!valid_repository_classification(*v.repository_classification))))return false;
    for(std::size_t i{};i<v.cleanup_count;++i){if(!v.cleanup[i])return false;const auto&e=*v.cleanup[i];
        if(!std::ranges::any_of(core::detail::error_category_names,[&](const auto&item){return item.first==e.category;})||
           !valid_token(e.code.value(),128U)||e.code.value()=="monitoring.q4.cleanup_overflow"||
           !valid_token(e.operation,64U)||e.severity!=core::ErrorSeverity::error||
           e.continuation!=core::ContinuationPolicy::abort_command||
           (e.os_error&&e.os_error->value<1))return false;}
    return true;
}
auto valid_authored_value(const MonitoringHostQ4BackendAuthored& v) noexcept -> bool {
    constexpr std::array allowed{"monitoring.q4.plan.schema","monitoring.q4.plan.structure","monitoring.q4.plan.value","monitoring.q4.plan.canonical","monitoring.q4.evidence.structure","monitoring.q4.evidence.value","monitoring.q4.evidence.canonical","monitoring.q4.evidence.privacy"};
    if(v.diagnostics.empty()||v.diagnostics.size()>32U)return false;
    for(const auto&d:v.diagnostics)if(std::ranges::find(allowed,d.code)==allowed.end()||!valid_pointer(d.json_pointer)||(d.byte_offset.has_value()!=d.line.has_value())||(d.line.has_value()!=d.column.has_value())||(d.line&&(*d.line==0U||*d.column==0U)))return false;
    return true;
}
auto contained_failure(MonitoringHostQ4BackendPhase p) -> MonitoringHostQ4BackendFailure {
    MonitoringHostQ4BackendFailure out;
    out.phase = p;
    return out;
}
using CapacityReason = MonitoringQ4CapacityReason;
using CapacityFamily = MonitoringQ4CapacityReasonFamily;
auto malformed_reason() -> MonitoringHostQ4BackendFailure {
    MonitoringHostQ4BackendFailure value;
    value.terminal_containment = true;
    return value;
}
auto capacity_projection(CapacityReason reason, std::initializer_list<CapacityFamily> families,
                         int native = 0) -> MonitoringHostQ4BackendFailure {
    if (!monitoring_q4_capacity_reason_matches(reason, families)) return malformed_reason();
    return stage_failure(MonitoringHostQ4BackendPhase::capacity,
                         *monitoring_q4_capacity_reason_code(reason), native);
}
auto inventory_reason_valid(const MonitoringQ4CpuFreqInventoryFailure& f) noexcept -> bool {
    using K = MonitoringQ4CpuFreqInventoryFailureKind;
    using R = CapacityReason;
    const bool capacity = f.primary == K::capacity || f.primary == K::allocation ||
        f.primary == K::root_name_limit || f.primary == K::root_entry_limit ||
        f.primary == K::policy_limit || f.primary == K::byte_limit;
    if (!capacity) return f.reason == R::none;
    if (f.reason == R::unspecified) return true;
    const auto* entry = monitoring_q4_capacity_reason_entry(f.reason);
    if (!entry || entry->family != CapacityFamily::inventory) return false;
    switch (f.primary) {
    case K::capacity:
        return f.reason == R::inventory_cleanup_formula || f.reason == R::inventory_policies_size ||
               f.reason == R::inventory_contents_size || f.reason == R::inventory_names_size || f.reason == R::inventory_cleanup_size;
    case K::allocation: return entry->code.ends_with(".allocation");
    case K::root_name_limit: return f.reason == R::inventory_root_names || f.reason == R::inventory_unrelated_name;
    case K::root_entry_limit: return f.reason == R::inventory_root_entries;
    case K::policy_limit: return f.reason == R::inventory_policy_slots;
    case K::byte_limit: return f.reason == R::inventory_attribute_bytes;
    default: return false;
    }
}
auto own_reason_valid(CapacityReason reason, bool allocation,
                      std::initializer_list<CapacityFamily> families) noexcept -> bool {
    if (!monitoring_q4_capacity_reason_matches(reason, families)) return false;
    if (reason == CapacityReason::unspecified) return true;
    return monitoring_q4_capacity_reason_entry(reason)->code.ends_with(".allocation") == allocation;
}
auto inventory_projection(const MonitoringQ4CpuFreqInventoryFailure& f)
    -> MonitoringHostQ4BackendFailure {
    using K = MonitoringQ4CpuFreqInventoryFailureKind;
    if (!inventory_reason_valid(f)) return malformed_reason();
    switch (f.primary) {
    case K::capacity:
    case K::allocation:
    case K::root_name_limit:
    case K::root_entry_limit:
    case K::policy_limit:
    case K::byte_limit:
        return capacity_projection(f.reason, {CapacityFamily::inventory}, f.native_error);
    case K::open_root:
    case K::enumerate:
    case K::policy_name:
    case K::open_policy:
    case K::open_attribute:
    case K::held_stat:
    case K::named_stat:
        return stage_failure(MonitoringHostQ4BackendPhase::oracle,
                             "monitoring.q4.oracle_open_failed", f.native_error);
    case K::identity:
        return stage_failure(MonitoringHostQ4BackendPhase::oracle,
                             "monitoring.q4.oracle_identity_changed", f.native_error);
    case K::read:
    case K::read_contract:
        return stage_failure(MonitoringHostQ4BackendPhase::oracle,
                             "monitoring.q4.oracle_read_failed", f.native_error);
    case K::close:
        return stage_failure(MonitoringHostQ4BackendPhase::finalization,
                             "monitoring.q4.finalization_failed", f.native_error);
    case K::contained:
        return contained_failure(MonitoringHostQ4BackendPhase::oracle);
    case K::inactive: {
        MonitoringHostQ4BackendFailure out;
        out.terminal_containment = true;
        return out;
    }
    case K::policy_callback:
        {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return out;
        }
    }
    MonitoringHostQ4BackendFailure terminal;
    terminal.terminal_containment = true;
    return terminal;
}
auto semantic_projection(const MonitoringQ4CpuFreqSemanticFailure& f)
    -> MonitoringHostQ4BackendFailure {
    using K = MonitoringQ4CpuFreqSemanticFailureKind;
    if (f.kind == K::hardware_access) {
        if (f.native_error != EACCES || f.reason != CapacityReason::none ||
            f.field != MonitoringQ4CpuFreqSemanticField::source_kind || f.ordinal == 0 ||
            f.production != MonitoringQ4CpuFreqProductionFailure::none)
            return malformed_reason();
        return stage_failure(MonitoringHostQ4BackendPhase::oracle,
                             "monitoring.q4.oracle_open_failed", f.native_error);
    }
    if (f.native_error != 0) return malformed_reason();
    if (f.kind != K::capacity && f.reason != CapacityReason::none) return malformed_reason();
    if (f.kind == K::capacity)
        return capacity_projection(f.reason, {CapacityFamily::semantic});
    if (f.kind == K::technical_token_invalid_utf8)
        return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                             "monitoring.q4.technical_token_invalid_utf8");
    if (f.kind == K::production) {
        switch (f.production) {
        case MonitoringQ4CpuFreqProductionFailure::ambiguous_policy:
            return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                 "linux.cpu_frequency.ambiguous_policy");
        case MonitoringQ4CpuFreqProductionFailure::invalid_value:
            return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                 "linux.cpu_frequency.invalid_value");
        case MonitoringQ4CpuFreqProductionFailure::contained:
            return contained_failure(MonitoringHostQ4BackendPhase::source_discovery);
        case MonitoringQ4CpuFreqProductionFailure::discovery:
            return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                 "linux.cpu_frequency.discovery_failed");
        case MonitoringQ4CpuFreqProductionFailure::none: {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return out;
        }
        }
    }
    if (f.kind == K::oracle)
        return stage_failure(MonitoringHostQ4BackendPhase::oracle,
                             "monitoring.q4.oracle_invalid_value");
    if (f.kind == K::contained)
        return contained_failure(MonitoringHostQ4BackendPhase::agreement);
    if (f.kind == K::binding || f.kind == K::inactive) {
        MonitoringHostQ4BackendFailure out;
        out.terminal_containment = true;
        return out;
    }
    if (f.kind == K::overlap || f.kind == K::conflict || f.kind == K::agreement)
        return stage_failure(MonitoringHostQ4BackendPhase::agreement,
                             "monitoring.q4.parser_disagreement");
    MonitoringHostQ4BackendFailure terminal;
    terminal.terminal_containment = true;
    return terminal;
}
enum class SourceSite : unsigned char { preparation, raw, typed };
auto source_projection(const MonitoringQ4PreparedSourceFailure& f, SourceSite site)
    -> MonitoringHostQ4BackendFailure {
    using K = MonitoringQ4PreparedSourceFailureKind;
    if (f.kind == K::allocation || f.kind == K::byte_limit) {
        if (!own_reason_valid(f.reason, f.kind == K::allocation, {CapacityFamily::source})) return malformed_reason();
    } else if (f.reason != CapacityReason::none) return malformed_reason();
    if (site == SourceSite::preparation) {
        if (f.kind == K::contained)
            return contained_failure(MonitoringHostQ4BackendPhase::source_discovery);
        if (f.kind == K::inactive) {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return out;
        }
        if (f.kind == K::inventory) {
            if (!f.inventory) {
                MonitoringHostQ4BackendFailure out;
                out.terminal_containment = true;
                return out;
            }
            if (!inventory_reason_valid(*f.inventory)) return malformed_reason();
            using IK = MonitoringQ4CpuFreqInventoryFailureKind;
            switch (f.inventory->primary) {
            case IK::capacity:
            case IK::allocation:
            case IK::root_name_limit:
            case IK::root_entry_limit:
            case IK::policy_limit:
            case IK::byte_limit:
                return capacity_projection(f.inventory->reason, {CapacityFamily::inventory}, f.inventory->native_error);
            case IK::open_attribute:
            case IK::held_stat:
            case IK::named_stat:
            case IK::identity:
            case IK::read:
            case IK::read_contract:
                return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                     "linux.cpu_frequency.read_failed", f.inventory->native_error);
            case IK::close:
                return stage_failure(MonitoringHostQ4BackendPhase::finalization,
                                     "monitoring.q4.finalization_failed",
                                     f.inventory->native_error);
            case IK::contained:
                return contained_failure(MonitoringHostQ4BackendPhase::source_discovery);
            case IK::inactive: {
                MonitoringHostQ4BackendFailure out;
                out.terminal_containment = true;
                return out;
            }
            case IK::open_root:
            case IK::enumerate:
            case IK::policy_name:
            case IK::open_policy:
                return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                     "linux.cpu_frequency.discovery_failed",
                                     f.inventory->native_error);
            case IK::policy_callback: {
                MonitoringHostQ4BackendFailure out;
                out.terminal_containment = true;
                return out;
            }
            default: {
                MonitoringHostQ4BackendFailure out;
                out.terminal_containment = true;
                return out;
            }
            }
        }
        if (f.kind == K::allocation || f.kind == K::byte_limit)
            return capacity_projection(f.reason, {CapacityFamily::source}, f.native_error);
        if (f.kind == K::ambiguous_policy)
            return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                 "linux.cpu_frequency.ambiguous_policy", f.native_error);
        if (f.kind == K::semantic_overlap || f.kind == K::semantic_conflict ||
            f.kind == K::semantic_agreement) {
            if (f.semantic) {
                const bool matches =
                    (f.kind == K::semantic_overlap &&
                     f.semantic->kind == MonitoringQ4CpuFreqSemanticFailureKind::overlap) ||
                    (f.kind == K::semantic_conflict &&
                     f.semantic->kind == MonitoringQ4CpuFreqSemanticFailureKind::conflict) ||
                    (f.kind == K::semantic_agreement &&
                     f.semantic->kind != MonitoringQ4CpuFreqSemanticFailureKind::overlap &&
                     f.semantic->kind != MonitoringQ4CpuFreqSemanticFailureKind::conflict);
                if (matches)
                    return semantic_projection(*f.semantic);
            }
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return out;
        }
        if (f.kind == K::close)
            return stage_failure(MonitoringHostQ4BackendPhase::finalization,
                                 "monitoring.q4.finalization_failed", f.native_error);
        if (f.kind == K::open_attribute || f.kind == K::held_stat || f.kind == K::named_stat ||
            f.kind == K::identity || f.kind == K::read || f.kind == K::read_contract)
            return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                 "linux.cpu_frequency.read_failed", f.native_error);
        if (f.kind == K::production_parser || f.kind == K::oracle_parser ||
            f.kind == K::parser_disagreement)
            return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                 "linux.cpu_frequency.invalid_value", f.native_error);
        if (f.kind == K::production_semantics)
            return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                 "linux.cpu_frequency.discovery_failed", f.native_error);
        MonitoringHostQ4BackendFailure out;
        out.terminal_containment = true;
        return out;
    }
    const auto malformed_site = [&] {
        MonitoringHostQ4BackendFailure out;
        out.terminal_containment = true;
        return out;
    };
    if (f.inventory || f.semantic)
        return malformed_site();
    if (f.kind == K::cpu_not_admitted || f.kind == K::inventory ||
        f.kind == K::ambiguous_policy || f.kind == K::production_semantics ||
        f.kind == K::semantic_overlap || f.kind == K::semantic_conflict ||
        f.kind == K::semantic_agreement ||
        (site == SourceSite::typed &&
         (f.kind == K::oracle_parser || f.kind == K::parser_disagreement)))
        return malformed_site();
    if (f.kind == K::contained)
        return contained_failure(site == SourceSite::typed
                                     ? MonitoringHostQ4BackendPhase::source_read
                                     : MonitoringHostQ4BackendPhase::agreement);
    if (f.kind == K::inactive) {
        MonitoringHostQ4BackendFailure out;
        out.terminal_containment = true;
        return out;
    }
    switch (f.kind) {
    case K::allocation:
    case K::byte_limit:
        return capacity_projection(f.reason, {CapacityFamily::source}, f.native_error);
    case K::open_attribute:
    case K::held_stat:
    case K::named_stat:
        return site == SourceSite::typed
                   ? stage_failure(MonitoringHostQ4BackendPhase::source_read,
                                   "linux.cpu_frequency.read_failed", f.native_error)
                   : stage_failure(MonitoringHostQ4BackendPhase::oracle,
                                   "monitoring.q4.oracle_open_failed", f.native_error);
    case K::identity:
        return site == SourceSite::typed
                   ? stage_failure(MonitoringHostQ4BackendPhase::source_read,
                                   "linux.cpu_frequency.read_failed", f.native_error)
                   : stage_failure(MonitoringHostQ4BackendPhase::oracle,
                                   "monitoring.q4.oracle_identity_changed", f.native_error);
    case K::read:
    case K::read_contract:
        return site == SourceSite::typed
                   ? stage_failure(MonitoringHostQ4BackendPhase::source_read,
                                   "linux.cpu_frequency.read_failed", f.native_error)
                   : stage_failure(MonitoringHostQ4BackendPhase::oracle,
                                   "monitoring.q4.oracle_read_failed", f.native_error);
    case K::oracle_parser:
        return stage_failure(MonitoringHostQ4BackendPhase::oracle,
                             "monitoring.q4.oracle_invalid_value", f.native_error);
    case K::production_parser:
        return site == SourceSite::typed
                   ? stage_failure(MonitoringHostQ4BackendPhase::source_read,
                                   "linux.cpu_frequency.invalid_value", f.native_error)
                   : stage_failure(MonitoringHostQ4BackendPhase::agreement,
                                   "monitoring.q4.production_parser_rejected", f.native_error);
    case K::parser_disagreement:
        return stage_failure(MonitoringHostQ4BackendPhase::agreement,
                             "monitoring.q4.parser_disagreement", f.native_error);
    case K::close:
        return stage_failure(MonitoringHostQ4BackendPhase::finalization,
                             "monitoring.q4.finalization_failed", f.native_error);
    case K::ambiguous_policy:
        return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                             "linux.cpu_frequency.ambiguous_policy", f.native_error);
    case K::production_semantics:
        return stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                             "linux.cpu_frequency.discovery_failed", f.native_error);
    case K::semantic_overlap:
    case K::semantic_conflict:
    case K::semantic_agreement:
        return stage_failure(MonitoringHostQ4BackendPhase::agreement,
                             "monitoring.q4.parser_disagreement", f.native_error);
    default:
        return malformed_site();
    }
}
struct CleanupIdentity final {
    unsigned role{};
    std::uint64_t policy{}, attribute{};
    int native{};
    bool uncertain{};
};
auto append_cleanup(MonitoringHostQ4BackendFailure& out,
                    const MonitoringQ4CpuFreqCleanupLedger& ledger,
                    std::optional<CleanupIdentity> primary = std::nullopt,
                    std::size_t primary_begin = 0U,
                    std::size_t primary_end = std::numeric_limits<std::size_t>::max()) -> bool {
    const auto events = ledger.events();
    if (primary_end == std::numeric_limits<std::size_t>::max())
        primary_end = events.size();
    if (primary_begin > primary_end || primary_end > events.size())
        return false;
    std::optional<std::size_t> primary_index;
    if (primary)
        for (std::size_t index = primary_begin; index < primary_end; ++index) {
            const auto& e = events[index];
            if (static_cast<unsigned>(e.role) == primary->role &&
                e.policy_ordinal == primary->policy && e.attribute_ordinal == primary->attribute &&
                e.native_error == primary->native && e.uncertain == primary->uncertain)
                primary_index = index;
        }
    if (primary && !primary_index && !ledger.overflowed())
        return false;
    for (std::size_t occurrence{}; occurrence < events.size(); ++occurrence) {
        const auto& e = events[occurrence];
        if (e.native_error == 0 && !e.uncertain)
            continue;
        if (primary_index && occurrence == *primary_index) {
            continue;
        }
        if (out.cleanup_count == out.cleanup.size()) {
            out.cleanup_overflowed = true;
            continue;
        }
        const auto role = static_cast<unsigned>(e.role);
        constexpr std::array<std::string_view, 7> operations{
            "retire_monitoring_q4_unknown",         "retire_monitoring_q4_attribute",
            "retire_monitoring_q4_policy",          "retire_monitoring_q4_root",
            "retire_monitoring_q4_fresh_attribute", "retire_monitoring_q4_selected_policy",
            "retire_monitoring_q4_retained_policy"};
        out.cleanup[out.cleanup_count++] = core::Error{
            .category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{e.uncertain ? "monitoring.q4.cleanup_uncertain"
                                                      : "monitoring.q4.cleanup_failed"},
            .message = {},
            .operation = std::string{role < operations.size() ? operations[role] : operations[0]},
            .os_error = e.native_error > 0
                            ? std::optional{core::OsError{e.native_error, "errno", ""}}
                            : std::nullopt,
            .context = {{"role", role},
                        {"policy_ordinal", e.policy_ordinal},
                        {"attribute_ordinal", e.attribute_ordinal},
                        {"uncertain", e.uncertain}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
    }
    return true;
}
} // namespace
static auto validate_result_typed(const MonitoringQ4SourceReady&, std::string_view) noexcept
    -> Validation;
void monitoring_host_q4_backend_serialization_test_fault(void (*value)(std::size_t)) {
    transport_fault = value;
}
void monitoring_host_q4_backend_serialization_test_fault_reset() noexcept {
    transport_fault = nullptr;
}
auto assemble_monitoring_host_q4_backend_result(const MonitoringQ4SourceReady& r,
                                                std::string_view evidence) noexcept
    -> std::expected<std::string, core::Error> {
    try {
        boundary(0U);
        auto accepted = validate_monitoring_q4_cpufreq_evidence_json(r, evidence);
        if (!accepted && accepted.error().code.value() == "monitoring.q4.terminal_containment")
            return std::unexpected{std::move(accepted.error())};
        if (!accepted)
            return std::unexpected{fail("evidence")};
        auto e = Json::parse(evidence.begin(), evidence.end());
        auto b = Json::parse(r.authority().build_attestation());
        boundary(1U);
        Json root{{"schema", "epycbench.monitoring-host-q4-backend-result/v1"},
                  {"capacity",
                   {{"identity", "epycbench.monitoring-q4-collection-capacity/v1"},
                    {"evidence_bytes", r.capacity().evidence_bytes().value},
                    {"stdout_bytes", r.capacity().success_stdout_bytes().value}}},
                  {"build_manifest", std::move(b)},
                  {"evidence", std::move(e)}};
        auto out = canonical(std::move(root), r.capacity().success_stdout_bytes().value);
        boundary(2U);
        if (!out)
            return std::unexpected{fail("result")};
        const auto validation = validate_result_typed(r, *out);
        if (validation == Validation::terminal)
            return std::unexpected{core::Error{}};
        if (validation == Validation::invalid)
            return std::unexpected{fail("result")};
        return out;
    } catch (...) {
        return std::unexpected{core::Error{}};
    }
}
static auto validate_result_typed(const MonitoringQ4SourceReady& r, std::string_view bytes) noexcept
    -> Validation {
    try {
        if (bytes.size() > r.capacity().success_stdout_bytes().value)
            return Validation::invalid;
        auto root = canonical_input(bytes);
        boundary(3U);
        if (!root)
            return Validation::invalid;
        auto held = schema(r, bytes, MonitoringQ4SchemaKind::backend_result);
        if (!held)
            return Validation::terminal;
        if (!*held)
            return Validation::invalid;
        auto evidence = core::canonicalize_json(root->at("evidence"));
        auto input = core::canonicalize_json(root->at("build_manifest"));
        if (!evidence || !input)
            return Validation::terminal;
        if (input->size() == std::numeric_limits<std::size_t>::max() ||
            input->size() + 1U > r.authority().governed().plan().build_manifest_bytes().value)
            return Validation::invalid;
        evidence->push_back('\n');
        auto accepted = validate_monitoring_q4_cpufreq_evidence_json(r, *evidence);
        if (!accepted)
            return accepted.error().code.value() == "monitoring.q4.terminal_containment"
                       ? Validation::terminal
                       : Validation::invalid;
        auto build = attest_monitoring_running_build_value(root->at("build_manifest"),
                                                           accepted->binding.repository_commit);
        if (!build)
            return Validation::invalid;
        auto encoded = core::canonicalize_json(*build);
        if (!encoded)
            return Validation::terminal;
        return *encoded == *input &&
                       accepted->binding.build_identity ==
                           "sha256:" +
                               root->at("build_manifest").at("build_identity").get<std::string>()
                   ? Validation::valid
                   : Validation::invalid;
    } catch (...) {
        return Validation::terminal;
    }
}
auto validate_monitoring_host_q4_backend_result(const MonitoringQ4SourceReady& r,
                                                std::string_view bytes) noexcept -> bool {
    return validate_result_typed(r, bytes) == Validation::valid;
}
auto serialize_monitoring_host_q4_backend_failure(const MonitoringQ4SourceReady& r,
                                                  const MonitoringHostQ4BackendFailure& v) noexcept
    -> std::expected<std::string, core::Error> {
    try {
        boundary(10U);
        Json primary;
        if (v.usage) {
            if (v.cleanup_count || v.cleanup_overflowed || v.terminal_containment ||
                !v.underlying_code.empty() || v.repository_classification || v.native_error)
                return std::unexpected{fail("usage")};
            primary = {{"category", "usage"},
                       {"code", "monitoring.q4.backend.usage"},
                       {"operation", "run_monitoring_host_q4_backend"},
                       {"severity", "error"},
                       {"continuation", "abort_command"},
                       {"classification", "usage"},
                       {"context", Json::object()}};
        } else if (v.terminal_containment) {
            if (!v.underlying_code.empty() || v.repository_classification || v.native_error)
                return std::unexpected{fail("terminal")};
            primary = {{"category", "monitoring"},
                       {"code", "monitoring.q4.backend.terminal_containment"},
                       {"operation", "run_monitoring_host_q4_backend"},
                       {"severity", "error"},
                       {"continuation", "abort_command"},
                       {"classification", "finalization"},
                       {"context", Json::object()}};
        } else {
            const auto p = phase(v.phase);
            if (p.empty())
                return std::unexpected{fail("phase")};
            Json context = Json::object();
            if (!v.underlying_code.empty())
                context["underlying_code"] = v.underlying_code;
            if (v.repository_classification)
                context["repository_classification"] = *v.repository_classification;
            primary = {{"category", "monitoring"},
                       {"code", "monitoring.q4.backend.stage_failure"},
                       {"operation", "run_monitoring_host_q4_backend"},
                       {"severity", "error"},
                       {"continuation", "abort_command"},
                       {"classification", p},
                       {"context", std::move(context)}};
            if (v.native_error && *v.native_error > 0)
                primary["os_error"] = *v.native_error;
        }
        if (v.cleanup_count > v.cleanup.size())
            return std::unexpected{fail("cleanup")};
        Json cleanup = Json::array();
        for (std::size_t i{}; i < v.cleanup_count; ++i) {
            if (!v.cleanup[i])
                return std::unexpected{fail("cleanup")};
            cleanup.push_back(safe(*v.cleanup[i]));
        }
        if (v.cleanup_overflowed) {
            if (cleanup.size() != 31U)
                return std::unexpected{fail("overflow")};
            cleanup.push_back({{"category", "monitoring"},
                               {"code", "monitoring.q4.cleanup_overflow"},
                               {"operation", "run_monitoring_host_q4_backend"},
                               {"severity", "error"},
                               {"continuation", "abort_command"},
                               {"context", Json::object()}});
        }
        Json root{{"schema", "epycbench.monitoring-host-q4-backend-error/v1"},
                  {"failure", std::move(primary)},
                  {"secondary_failures", std::move(cleanup)}};
        auto out = canonical(std::move(root), r.capacity().backend_stderr_bytes().value);
        boundary(11U);
        if (!out)
            return std::unexpected{fail("error")};
        if (!validate_monitoring_host_q4_backend_failure(r, *out))
            return std::unexpected{core::Error{}};
        return out;
    } catch (...) {
        return std::unexpected{core::Error{}};
    }
}
auto validate_monitoring_host_q4_backend_failure(const MonitoringQ4SourceReady& r,
                                                 std::string_view bytes) noexcept -> bool {
    try {
        if (bytes.size() > r.capacity().backend_stderr_bytes().value || !canonical_input(bytes))
            return false;
        auto held = schema(r, bytes, MonitoringQ4SchemaKind::backend_error);
        return held && *held;
    } catch (...) {
        return false;
    }
}
namespace {
auto serialize_pre_source_failure_with_limit(
    const MonitoringHostQ4BackendFailure& v, std::uint64_t cap) noexcept -> std::expected<std::string, core::Error> {
    try {
        if(!valid_failure_value(v))return std::unexpected{fail("failure")};
        Json primary;
        if (v.usage) {
            if (v.cleanup_count || v.cleanup_overflowed || v.terminal_containment ||
                !v.underlying_code.empty() || v.repository_classification || v.native_error)
                return std::unexpected{fail("usage")};
            primary = {{"category", "usage"}, {"code", "monitoring.q4.backend.usage"},
                       {"operation", "run_monitoring_host_q4_backend"}, {"severity", "error"},
                       {"continuation", "abort_command"}, {"classification", "usage"},
                       {"context", Json::object()}};
        } else if (v.terminal_containment) {
            if (!v.underlying_code.empty() || v.repository_classification || v.native_error)
                return std::unexpected{fail("terminal")};
            primary = {{"category", "monitoring"},
                       {"code", "monitoring.q4.backend.terminal_containment"},
                       {"operation", "run_monitoring_host_q4_backend"}, {"severity", "error"},
                       {"continuation", "abort_command"}, {"classification", "finalization"},
                       {"context", Json::object()}};
        } else {
            const auto p = phase(v.phase);
            if (p.empty()) return std::unexpected{fail("phase")};
            Json context = Json::object();
            if (!v.underlying_code.empty()) context["underlying_code"] = v.underlying_code;
            if (v.repository_classification)
                context["repository_classification"] = *v.repository_classification;
            primary = {{"category", "monitoring"}, {"code", "monitoring.q4.backend.stage_failure"},
                       {"operation", "run_monitoring_host_q4_backend"}, {"severity", "error"},
                       {"continuation", "abort_command"}, {"classification", p},
                       {"context", std::move(context)}};
            if (v.native_error && *v.native_error > 0) primary["os_error"] = *v.native_error;
        }
        if (v.cleanup_count > v.cleanup.size()) return std::unexpected{fail("cleanup")};
        Json cleanup = Json::array();
        for (std::size_t i{}; i < v.cleanup_count; ++i) {
            if (!v.cleanup[i]) return std::unexpected{fail("cleanup")};
            cleanup.push_back(safe(*v.cleanup[i]));
        }
        if (v.cleanup_overflowed) {
            if (cleanup.size() != 31U) return std::unexpected{fail("overflow")};
            cleanup.push_back({{"category", "monitoring"},
                               {"code", "monitoring.q4.cleanup_overflow"},
                               {"operation", "run_monitoring_host_q4_backend"},
                               {"severity", "error"}, {"continuation", "abort_command"},
                               {"context", Json::object()}});
        }
        return canonical(Json{{"schema", "epycbench.monitoring-host-q4-backend-error/v1"},
                              {"failure", std::move(primary)},
                              {"secondary_failures", std::move(cleanup)}}, cap);
    } catch (...) { return std::unexpected{core::Error{}}; }
}
} // namespace
auto serialize_monitoring_host_q4_backend_failure_pre_source(
    const MonitoringHostQ4BackendFailure& value) noexcept -> std::expected<std::string, core::Error> {
    return serialize_pre_source_failure_with_limit(value, 792302U);
}
// Declared only by the focused test: no production caller can select a smaller ceiling.
auto monitoring_q4_capacity_reason_serialization_limit_test(
    const MonitoringHostQ4BackendFailure& value, std::uint64_t limit) noexcept
    -> std::expected<std::string, core::Error> {
    return serialize_pre_source_failure_with_limit(value, limit);
}
auto serialize_monitoring_host_q4_backend_authored(
    const MonitoringQ4SourceReady& r, const MonitoringHostQ4BackendAuthored& v) noexcept
    -> std::expected<std::string, core::Error> {
    try {
        boundary(20U);
        if (v.diagnostics.empty() || v.diagnostics.size() > 32U)
            return std::unexpected{fail("diagnostics")};
        Json values = Json::array();
        std::set<std::string> unique;
        for (const auto& d : v.diagnostics) {
            if (!valid_pointer(d.json_pointer) ||
                (d.byte_offset.has_value() != d.line.has_value()) ||
                (d.line.has_value() != d.column.has_value()))
                return std::unexpected{fail("diagnostic")};
            Json location = nullptr;
            if (d.byte_offset)
                location = {
                    {"byte_offset", *d.byte_offset}, {"line", *d.line}, {"column", *d.column}};
            Json item{{"code", d.code},
                      {"json_pointer", d.json_pointer},
                      {"location", std::move(location)},
                      {"safe_context", Json::object()}};
            auto key = core::canonicalize_json(item);
            if (!key || !unique.insert(*key).second)
                return std::unexpected{fail("diagnostic")};
            values.push_back(std::move(item));
        }
        Json root{{"schema", "epycbench.monitoring-host-q4-backend-authored/v1"},
                  {"diagnostics", std::move(values)}};
        auto out = canonical(std::move(root), r.capacity().backend_stderr_bytes().value);
        boundary(21U);
        if (!out)
            return std::unexpected{fail("authored")};
        if (!validate_monitoring_host_q4_backend_authored(r, *out))
            return std::unexpected{core::Error{}};
        return out;
    } catch (...) {
        return std::unexpected{core::Error{}};
    }
}
auto validate_monitoring_host_q4_backend_authored(const MonitoringQ4SourceReady& r,
                                                  std::string_view bytes) noexcept -> bool {
    try {
        auto root = canonical_input(bytes);
        auto held = schema(r, bytes, MonitoringQ4SchemaKind::backend_authored);
        if (!root || bytes.size() > r.capacity().backend_stderr_bytes().value || !held || !*held)
            return false;
        std::set<std::string> unique;
        for (const auto& d : root->at("diagnostics")) {
            const auto pointer = d.at("json_pointer").get<std::string>();
            auto key = core::canonicalize_json(d);
            if (!key || !valid_pointer(pointer) || !unique.insert(*key).second)
                return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}
auto serialize_monitoring_host_q4_backend_authored_pre_source(
    const MonitoringHostQ4BackendAuthored& v) noexcept -> std::expected<std::string, core::Error> {
    try {
        constexpr std::uint64_t cap = 792302U;
        if (!valid_authored_value(v))
            return std::unexpected{fail("diagnostics")};
        Json values = Json::array();
        std::set<std::string> unique;
        for (const auto& d : v.diagnostics) {
            if (!valid_pointer(d.json_pointer) ||
                (d.byte_offset.has_value() != d.line.has_value()) ||
                (d.line.has_value() != d.column.has_value()))
                return std::unexpected{fail("diagnostic")};
            Json location = nullptr;
            if (d.byte_offset)
                location = {{"byte_offset", *d.byte_offset}, {"line", *d.line},
                            {"column", *d.column}};
            Json item{{"code", d.code}, {"json_pointer", d.json_pointer},
                      {"location", std::move(location)}, {"safe_context", Json::object()}};
            auto key = core::canonicalize_json(item);
            if (!key || !unique.insert(*key).second)
                return std::unexpected{fail("diagnostic")};
            values.push_back(std::move(item));
        }
        return canonical(Json{{"schema", "epycbench.monitoring-host-q4-backend-authored/v1"},
                              {"diagnostics", std::move(values)}}, cap);
    } catch (...) { return std::unexpected{core::Error{}}; }
}
auto project_monitoring_host_q4_collection_failure(
    const MonitoringQ4CpuFreqCollectionFailure& f) noexcept -> MonitoringHostQ4BackendFailure {
    try {
        MonitoringHostQ4BackendFailure out;
        using K = MonitoringQ4CpuFreqCollectionFailureKind;
        if (f.kind == K::allocation || f.kind == K::capacity) {
            if (!own_reason_valid(f.reason, f.kind == K::allocation,
                                  {CapacityFamily::collector, CapacityFamily::ledger})) return malformed_reason();
        } else if (f.reason != CapacityReason::none) return malformed_reason();
        switch (f.kind) {
        case K::operations:
            out = stage_failure(MonitoringHostQ4BackendPhase::source_discovery,
                                "monitoring.q4.operations_invalid");
            break;
        case K::allocation:
        case K::capacity:
            out = capacity_projection(f.reason, {CapacityFamily::collector, CapacityFamily::ledger});
            break;
        case K::clock:
            out = stage_failure(MonitoringHostQ4BackendPhase::clock, "monitoring.q4.clock_failed",
                                f.clock && f.clock->os_error ? f.clock->os_error->value : 0);
            break;
        case K::deadline:
            out = stage_failure(MonitoringHostQ4BackendPhase::deadline, "monitoring.q4.deadline");
            break;
        case K::semantic_drift:
            out = stage_failure(MonitoringHostQ4BackendPhase::agreement,
                                "monitoring.q4.parser_disagreement");
            break;
        case K::contained:
            out.terminal_containment = true;
            break;
        case K::cycle:
            if (!f.cycle) {
                out.terminal_containment = true;
                break;
            }
            if (f.cycle->kind != MonitoringQ4CpuFreqCycleFailureKind::allocation &&
                f.cycle->kind != MonitoringQ4CpuFreqCycleFailureKind::capacity &&
                f.cycle->reason != CapacityReason::none) return malformed_reason();
            if (f.cycle->kind == MonitoringQ4CpuFreqCycleFailureKind::allocation) {
                const bool nested_reason = f.cycle->source || f.cycle->semantic || f.cycle->inventory;
                if (!(nested_reason && f.cycle->reason == CapacityReason::none) &&
                    !own_reason_valid(f.cycle->reason, true, {CapacityFamily::cycle}))
                    return malformed_reason();
            }
            if (f.cycle->kind == MonitoringQ4CpuFreqCycleFailureKind::capacity &&
                !(f.ledger && f.ledger->overflowed() && f.cycle->reason == CapacityReason::none) &&
                !own_reason_valid(f.cycle->reason, false, {CapacityFamily::cycle})) return malformed_reason();
            switch (f.cycle->kind) {
            case MonitoringQ4CpuFreqCycleFailureKind::initial_inventory:
            case MonitoringQ4CpuFreqCycleFailureKind::final_inventory:
                if(f.cycle->inventory)out=inventory_projection(*f.cycle->inventory);else out.terminal_containment=true;
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::initial_semantics:
            case MonitoringQ4CpuFreqCycleFailureKind::final_semantics:
                if(f.cycle->semantic)out=semantic_projection(*f.cycle->semantic);else out.terminal_containment=true;
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::preparation:
                if(f.cycle->source)out=source_projection(*f.cycle->source,SourceSite::preparation);else out.terminal_containment=true;
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::metadata:
                out = f.cycle->source ? source_projection(*f.cycle->source, SourceSite::preparation)
                                      : stage_failure(MonitoringHostQ4BackendPhase::agreement,
                                                      "monitoring.q4.parser_disagreement");
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::raw_read:
                if(f.cycle->source)out=source_projection(*f.cycle->source,SourceSite::raw);else out.terminal_containment=true;
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::typed_read:
                if(f.cycle->source)out=source_projection(*f.cycle->source,SourceSite::typed);else out.terminal_containment=true;
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::allocation:
                if(f.cycle->source)out=source_projection(*f.cycle->source,SourceSite::preparation);
                else if(f.cycle->semantic)out=semantic_projection(*f.cycle->semantic);
                else if(f.cycle->inventory)out=inventory_projection(*f.cycle->inventory);
                else out=capacity_projection(f.cycle->reason, {CapacityFamily::cycle});
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::capacity:
                out =
                    capacity_projection(f.cycle->reason, {CapacityFamily::cycle});
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::semantic_drift:
                out = stage_failure(MonitoringHostQ4BackendPhase::agreement,
                                    "monitoring.q4.parser_disagreement");
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::retirement:
                if (f.cycle->source && f.cycle->source->reason != CapacityReason::none)
                    return malformed_reason();
                if (!f.cycle->source) {
                    out.terminal_containment = true;
                    break;
                }
                switch (f.cycle->source->kind) {
                case MonitoringQ4PreparedSourceFailureKind::held_stat:
                case MonitoringQ4PreparedSourceFailureKind::identity:
                case MonitoringQ4PreparedSourceFailureKind::close:
                    out = stage_failure(MonitoringHostQ4BackendPhase::finalization,
                                        "monitoring.q4.finalization_failed",
                                        f.cycle->source->native_error);
                    break;
                default:
                    out.terminal_containment = true;
                    break;
                }
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::contained:
                out = contained_failure(MonitoringHostQ4BackendPhase::source_discovery);
                break;
            case MonitoringQ4CpuFreqCycleFailureKind::inactive:
                out.terminal_containment = true;
                break;
            default:
                out.terminal_containment = true;
                break;
            }
            break;
        default:
            out.terminal_containment = true;
            break;
        }
        std::optional<CleanupIdentity> primary_cleanup;
        const bool cleanup_ledger_primary =
            f.cycle && f.cycle->kind == MonitoringQ4CpuFreqCycleFailureKind::capacity && f.ledger &&
            f.ledger->overflowed();
        if (cleanup_ledger_primary)
            out = stage_failure(MonitoringHostQ4BackendPhase::finalization,
                                "monitoring.q4.finalization_failed");
        if (f.cycle && f.cycle->source &&
            f.cycle->source->kind == MonitoringQ4PreparedSourceFailureKind::close &&
            f.cycle->source->cleanup_count != 0U) {
            const auto& event = f.cycle->source->cleanup[0];
            primary_cleanup =
                CleanupIdentity{4U + static_cast<unsigned>(event.role), event.policy_ordinal,
                                event.attribute_ordinal, event.native_error, event.uncertain};
        } else if (f.cycle && f.cycle->inventory &&
                   f.cycle->inventory->primary == MonitoringQ4CpuFreqInventoryFailureKind::close) {
            const auto& value = *f.cycle->inventory;
            primary_cleanup = CleanupIdentity{
                static_cast<unsigned>(value.primary_role), value.primary_policy_ordinal,
                value.primary_attribute_ordinal, value.native_error, value.primary_uncertain};
        }
        if (f.ledger &&
            !append_cleanup(out, *f.ledger, primary_cleanup, f.cycle ? f.cycle->cleanup_begin : 0U,
                            f.cycle ? f.cycle->cleanup_end
                                    : std::numeric_limits<std::size_t>::max())) {
            MonitoringHostQ4BackendFailure terminal;
            terminal.terminal_containment = true;
            return terminal;
        }
        if (f.ledger && f.ledger->overflowed() && !cleanup_ledger_primary &&
            out.cleanup_count < out.cleanup.size()) {
            out.cleanup[out.cleanup_count++] =
                core::Error{.category = core::ErrorCategory::monitoring,
                            .code = core::StableErrorCode{"monitoring.q4.finalization_failed"},
                            .message = {},
                            .operation = "retain_monitoring_q4_cleanup_ledger",
                            .os_error = std::nullopt,
                            .context = nlohmann::ordered_json::object(),
                            .severity = core::ErrorSeverity::error,
                            .continuation = core::ContinuationPolicy::abort_command};
        } else if (f.ledger && f.ledger->overflowed() && !cleanup_ledger_primary)
            out.cleanup_overflowed = true;
        return out;
    } catch (...) {
        MonitoringHostQ4BackendFailure out;
        out.terminal_containment = true;
        return out;
    }
}
auto run_monitoring_host_q4_backend_adapter_with(
    MonitoringQ4SourceReady& r, MonitoringHostQ4BackendOperations operations) noexcept
    -> std::expected<std::string, MonitoringHostQ4BackendFailure> {
    auto rejected = [](MonitoringHostQ4BackendPhase phase, std::string code) {
        MonitoringHostQ4BackendFailure out;
        out.phase = phase;
        out.underlying_code = std::move(code);
        return std::expected<std::string, MonitoringHostQ4BackendFailure>{
            std::unexpected{std::move(out)}};
    };
    try {
        if (!operations.collect || !operations.build || !operations.encode ||
            !operations.assemble) {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return std::unexpected{std::move(out)};
        }
        auto collection = operations.collect(operations.context, r);
        if (!collection)
            return std::unexpected{
                project_monitoring_host_q4_collection_failure(collection.error())};
        auto evidence = operations.build(operations.context, r, *collection);
        if (!evidence && evidence.error().terminal == evidence.error().ordinary.has_value()) {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return std::unexpected{std::move(out)};
        }
        if (!evidence && evidence.error().terminal) {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return std::unexpected{std::move(out)};
        }
        if (!evidence)
            return rejected(MonitoringHostQ4BackendPhase::evidence_construction,
                            "monitoring.q4.evidence_invalid");
        auto bytes = operations.encode(operations.context, *evidence);
        if (!bytes && bytes.error().terminal == bytes.error().ordinary.has_value()) {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return std::unexpected{std::move(out)};
        }
        if (!bytes && bytes.error().terminal) {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return std::unexpected{std::move(out)};
        }
        if (!bytes)
            return rejected(MonitoringHostQ4BackendPhase::evidence_construction,
                            "monitoring.q4.evidence_invalid");
        auto result = operations.assemble(operations.context, r, *bytes);
        if (!result && result.error().terminal == result.error().ordinary.has_value()) {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return std::unexpected{std::move(out)};
        }
        if (!result && result.error().terminal) {
            MonitoringHostQ4BackendFailure out;
            out.terminal_containment = true;
            return std::unexpected{std::move(out)};
        }
        if (!result)
            return rejected(MonitoringHostQ4BackendPhase::evidence_construction,
                            "monitoring.q4.evidence_invalid");
        return std::move(*result);
    } catch (...) {
        MonitoringHostQ4BackendFailure out;
        out.terminal_containment = true;
        return std::unexpected{std::move(out)};
    }
}
auto run_monitoring_host_q4_backend_adapter(MonitoringQ4SourceReady& r) noexcept
    -> std::expected<std::string, MonitoringHostQ4BackendFailure> {
    struct Production final {
        static auto typed(core::Error error) {
            MonitoringHostQ4BackendOperations::Failure out;
            try {
                out.terminal = error.code.value().empty() ||
                               error.code.value() == "monitoring.q4.terminal_containment";
            } catch (...) {
                out.terminal = true;
            }
            if (!out.terminal)
                out.ordinary.emplace(std::move(error));
            return out;
        }
        static auto collect(void*, const MonitoringQ4SourceReady& ready) {
            return MonitoringQ4CpuFreqCollector::collect(ready);
        }
        static auto build(void*, const MonitoringQ4SourceReady& ready,
                          MonitoringQ4CpuFreqCollection& collection)
            -> std::expected<MonitoringQ4CpuFreqEvidenceValue,
                             MonitoringHostQ4BackendOperations::Failure> {
            auto value = MonitoringQ4CpuFreqEvidenceBuilder::build(ready, collection);
            if (!value)
                return std::unexpected{typed(std::move(value.error()))};
            return std::move(*value);
        }
        static auto encode(void*, const MonitoringQ4CpuFreqEvidenceValue& input)
            -> std::expected<std::string, MonitoringHostQ4BackendOperations::Failure> {
            auto value = encode_monitoring_q4_cpufreq_evidence_json(input);
            if (!value)
                return std::unexpected{typed(std::move(value.error()))};
            return std::move(*value);
        }
        static auto assemble(void*, const MonitoringQ4SourceReady& ready, std::string_view bytes)
            -> std::expected<std::string, MonitoringHostQ4BackendOperations::Failure> {
            auto value = assemble_monitoring_host_q4_backend_result(ready, bytes);
            if (!value)
                return std::unexpected{typed(std::move(value.error()))};
            return std::move(*value);
        }
    };
    return run_monitoring_host_q4_backend_adapter_with(r, {nullptr, Production::collect,
                                                           Production::build, Production::encode,
                                                           Production::assemble});
}
} // namespace epycbench::monitoring::qualification::detail
