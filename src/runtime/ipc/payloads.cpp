#include "epycbench/runtime/ipc/payloads.hpp"
#include "core/json_ownership.hpp"
#include "runtime/ipc/placement_request_construction_access.hpp"
#include "runtime/placement_provenance_access.hpp"
#include <algorithm>
#include <charconv>
#include <exception>
#include <limits>
#include <numeric>
#include <set>

namespace epycbench::runtime::ipc {
namespace {
void retain_parser_failure(std::optional<core::Error>* sink, core::Error& error) noexcept {
    if (sink && !sink->has_value())
        sink->emplace(std::move(error));
}

auto bad(std::string c) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.ipc.invalid_payload"},
            .message = "IPC payload is invalid",
            .operation = "parse_ipc_payload",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& value) {
                core::detail::set_json(value, "classification", nlohmann::json(std::move(c)));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto exact(const nlohmann::json& j, std::initializer_list<std::string_view> n) -> bool {
    if (!j.is_object() || j.size() != n.size())
        return false;
    for (auto x : n)
        if (!j.contains(x))
            return false;
    return true;
}
auto text64(const nlohmann::json& j, bool positive = false)
    -> std::expected<std::uint64_t, core::Error> {
    if (!j.is_string())
        return std::unexpected{bad("uint64_text_type")};
    auto& s = j.get_ref<const std::string&>();
    if (s.empty() || (s.size() > 1 && s[0] == '0'))
        return std::unexpected{bad("uint64_text_syntax")};
    std::uint64_t v{};
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (e != std::errc{} || p != s.data() + s.size() || (positive && !v))
        return std::unexpected{bad("uint64_text_range")};
    return v;
}
template <class T>
auto strong(const nlohmann::json& j, std::optional<core::Error>* raw_failure = nullptr)
    -> std::expected<T, core::Error> {
    if (!j.is_string())
        return std::unexpected{bad("strong_id_type")};
    auto v = T::parse(j.get_ref<const std::string&>());
    core::detail::ExpectedErrorCleanup v_cleanup{v};
    if (!v) {
        retain_parser_failure(raw_failure, v.error());
        return std::unexpected{bad("strong_id_invalid")};
    }
    return std::move(*v);
}
template <class T> auto linux_id(const nlohmann::json& j) -> std::expected<T, core::Error> {
    if (!j.is_number_unsigned())
        return std::unexpected{bad("linux_id_type")};
    auto v = j.get<std::uint64_t>();
    if (v > std::numeric_limits<typename T::representation_type>::max())
        return std::unexpected{bad("linux_id_range")};
    return T{static_cast<typename T::representation_type>(v)};
}
struct Ratio {
    std::uint64_t n, d;
};
auto ratio(const nlohmann::json& j) -> std::expected<Ratio, core::Error> {
    if (!exact(j, {"numerator", "denominator"}))
        return std::unexpected{bad("ratio_shape")};
    auto n = text64(j["numerator"]);
    core::detail::ExpectedErrorCleanup n_cleanup{n};
    if (!n)
        return std::unexpected{std::move(n.error())};
    auto d = text64(j["denominator"], true);
    core::detail::ExpectedErrorCleanup d_cleanup{d};
    if (!n)
        return std::unexpected{std::move(n.error())};
    if (!d)
        return std::unexpected{std::move(d.error())};
    if (*n > *d || std::gcd(*n, *d) != 1)
        return std::unexpected{bad("ratio_not_reduced_or_range")};
    return Ratio{*n, *d};
}
auto ratio_ge(Ratio a, Ratio b) -> bool {
    bool reverse = false;
    for (;;) {
        auto aq = a.n / a.d, bq = b.n / b.d;
        if (aq != bq)
            return reverse ? aq < bq : aq > bq;
        auto ar = a.n % a.d, br = b.n % b.d;
        if (!ar || !br) {
            if (!ar && !br)
                return true;
            return reverse ? !ar : !br;
        }
        a = {a.d, ar};
        b = {b.d, br};
        reverse = !reverse;
    }
}
struct Cpu {
    topology::StableProcessingUnitId pu;
    topology::StableCoreId core;
    core::LinuxCpuId linux;
};
auto cpu(const nlohmann::json& j, std::optional<core::Error>* raw_failure = nullptr)
    -> std::expected<Cpu, core::Error> {
    if (!exact(j, {"stable_pu_id", "stable_core_id", "linux_cpu"}))
        return std::unexpected{bad("cpu_shape")};
    auto p = strong<topology::StableProcessingUnitId>(j["stable_pu_id"], raw_failure);
    core::detail::ExpectedErrorCleanup p_cleanup{p};
    if (!p) {
        retain_parser_failure(raw_failure, p.error());
        return std::unexpected{bad("cpu_identity")};
    }
    auto c = strong<topology::StableCoreId>(j["stable_core_id"], raw_failure);
    core::detail::ExpectedErrorCleanup c_cleanup{c};
    if (!c) {
        retain_parser_failure(raw_failure, c.error());
        return std::unexpected{bad("cpu_identity")};
    }
    auto l = linux_id<core::LinuxCpuId>(j["linux_cpu"]);
    core::detail::ExpectedErrorCleanup l_cleanup{l};
    if (!l) {
        retain_parser_failure(raw_failure, l.error());
        return std::unexpected{bad("cpu_identity")};
    }
    if (!p || !c || !l)
        return std::unexpected{bad("cpu_identity")};
    return Cpu{std::move(*p), std::move(*c), *l};
}
struct Node {
    topology::StableNumaNodeId stable;
    core::LinuxNumaNodeId linux;
};
auto node(const nlohmann::json& j, bool pages = false,
          std::optional<core::Error>* raw_failure = nullptr) -> std::expected<Node, core::Error> {
    if (!exact(j, pages ? std::initializer_list<std::string_view>{"stable_numa_node_id",
                                                                  "linux_numa_node", "pages"}
                        : std::initializer_list<std::string_view>{"stable_numa_node_id",
                                                                  "linux_numa_node"}))
        return std::unexpected{bad("node_shape")};
    auto s = strong<topology::StableNumaNodeId>(j["stable_numa_node_id"], raw_failure);
    core::detail::ExpectedErrorCleanup s_cleanup{s};
    if (!s) {
        retain_parser_failure(raw_failure, s.error());
        return std::unexpected{bad("node_identity")};
    }
    auto l = linux_id<core::LinuxNumaNodeId>(j["linux_numa_node"]);
    core::detail::ExpectedErrorCleanup l_cleanup{l};
    if (!l) {
        retain_parser_failure(raw_failure, l.error());
        return std::unexpected{bad("node_identity")};
    }
    if (!s || !l)
        return std::unexpected{bad("node_identity")};
    return Node{std::move(*s), *l};
}
auto nodes(const nlohmann::json& a, bool page_records = false,
           std::optional<core::Error>* raw_failure = nullptr)
    -> std::expected<std::vector<Node>, core::Error> {
    if (!a.is_array() || (!page_records && a.empty()))
        return std::unexpected{bad("nodes_cardinality")};
    std::vector<Node> v;
    std::set<std::string> ss;
    std::set<std::uint32_t> ls;
    std::pair<std::uint32_t, std::string> last{};
    bool first = true;
    for (auto& j : a) {
        auto n = node(j, page_records, raw_failure);
        core::detail::ExpectedErrorCleanup n_cleanup{n};
        if (!n)
            return std::unexpected{std::move(n.error())};
        auto st = std::string{n->stable.value()};
        if (!ss.insert(st).second || !ls.insert(n->linux.value()).second)
            return std::unexpected{bad("duplicate_node_identity")};
        if (page_records) {
            auto key = std::pair{n->linux.value(), st};
            if (!first && key <= last)
                return std::unexpected{bad("page_nodes_order")};
            last = std::move(key);
            first = false;
            auto p = text64(j["pages"], true);
            core::detail::ExpectedErrorCleanup p_cleanup{p};
            if (!p)
                return std::unexpected{std::move(p.error())};
        }
        v.push_back(std::move(*n));
    }
    return v;
}
auto request_nodes(const nlohmann::json& values, std::optional<core::Error>* raw_failure = nullptr)
    -> std::expected<std::vector<Node>, core::Error> {
    if (!values.is_array())
        return std::unexpected{bad("nodes_cardinality")};
    std::vector<Node> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        auto parsed = node(value, false, raw_failure);
        core::detail::ExpectedErrorCleanup parsed_cleanup{parsed};
        if (!parsed)
            return std::unexpected{std::move(parsed.error())};
        result.push_back(std::move(*parsed));
    }
    return result;
}
struct Common {
    runtime::PlacementProvenance provenance;
    topology::PhysicalTopologyFingerprint pf;
    topology::EffectiveTopologyFingerprint ef;
    std::uint64_t index;
    scenario::ScenarioIdentifier id;
    scenario::ScenarioIdentifier workload;
    kernel::KernelId kernel;
};
auto parse_provenance(const nlohmann::json& value,
                      std::optional<core::Error>* raw_failure = nullptr)
    -> std::expected<runtime::PlacementProvenance, core::Error> {
    if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string())
        return std::unexpected{bad("provenance_kind_invalid")};
    if (value["kind"] == "scenario") {
        if (!exact(value,{"kind","resolved_scenario_plan_digest","scenario_digest"}))
            return std::unexpected{bad("provenance_invalid")};
        auto sd = strong<scenario::ScenarioDigest>(value["scenario_digest"], raw_failure);
        core::detail::ExpectedErrorCleanup sd_cleanup{sd};
        if (!sd) {
            retain_parser_failure(raw_failure, sd.error());
            return std::unexpected{bad("provenance_digest_invalid")};
        }
        auto rd = strong<scenario::ResolvedScenarioPlanDigest>(
            value["resolved_scenario_plan_digest"], raw_failure);
        core::detail::ExpectedErrorCleanup rd_cleanup{rd};
        if (!rd) {
            retain_parser_failure(raw_failure, rd.error());
            return std::unexpected{bad("provenance_digest_invalid")};
        }
        if(!sd||!rd) return std::unexpected{bad("provenance_digest_invalid")};
        return runtime::detail::PlacementProvenanceConstructionAccess::scenario(std::move(*sd),std::move(*rd));
    }
    if(value["kind"] != "host_qualification_plan")
        return std::unexpected{bad("provenance_kind_invalid")};
    if(!exact(value,{"environment_digest","host_qualification_plan_digest","kind","policy_identity","template_sha256"}) ||
       !value["policy_identity"].is_string() || !value["template_sha256"].is_string() ||
       !value["environment_digest"].is_string() || !value["host_qualification_plan_digest"].is_string())
        return std::unexpected{bad("provenance_invalid")};
    auto templ=core::Sha256Digest::parse(value["template_sha256"].get_ref<const std::string&>());
    core::detail::ExpectedErrorCleanup templ_cleanup{templ};
    if (!templ) {
        retain_parser_failure(raw_failure, templ.error());
        return std::unexpected{bad("provenance_digest_invalid")};
    }
    const auto prefixed_digest_valid = [raw_failure](const std::string& text) {
        if (!text.starts_with("sha256:"))
            return false;
        auto parsed = core::Sha256Digest::parse(std::string_view{text}.substr(7));
        core::detail::ExpectedErrorCleanup cleanup{parsed};
        if (!parsed)
            retain_parser_failure(raw_failure, parsed.error());
        return parsed.has_value();
    };
    if(!prefixed_digest_valid(value["environment_digest"].get_ref<const std::string&>()) ||
       !prefixed_digest_valid(value["host_qualification_plan_digest"].get_ref<const std::string&>()))
        return std::unexpected{bad("provenance_digest_invalid")};
    auto policy = runtime::detail::PlacementProvenanceConstructionAccess::policy_identity(
        value["policy_identity"].get_ref<const std::string&>());
    core::detail::ExpectedErrorCleanup policy_cleanup{policy};
    if (!policy) {
        retain_parser_failure(raw_failure, policy.error());
        return std::unexpected{bad("provenance_invalid")};
    }
    auto environment = runtime::detail::PlacementProvenanceConstructionAccess::environment_digest(
        value["environment_digest"].get_ref<const std::string&>());
    core::detail::ExpectedErrorCleanup environment_cleanup{environment};
    if (!environment) {
        retain_parser_failure(raw_failure, environment.error());
        return std::unexpected{bad("provenance_digest_invalid")};
    }
    auto plan = runtime::detail::PlacementProvenanceConstructionAccess::plan_digest(
        value["host_qualification_plan_digest"].get_ref<const std::string&>());
    core::detail::ExpectedErrorCleanup plan_cleanup{plan};
    if (!plan) {
        retain_parser_failure(raw_failure, plan.error());
        return std::unexpected{bad("provenance_digest_invalid")};
    }
    return runtime::detail::PlacementProvenanceConstructionAccess::host(
       std::move(*policy),*templ,std::move(*environment),std::move(*plan));
}
auto common(const nlohmann::json& j, bool v4 = false,
            std::optional<core::Error>* raw_failure = nullptr)
    -> std::expected<Common, core::Error> {
    if (!exact(j["topology"], {"physical_fingerprint", "effective_fingerprint"}) ||
        !exact(j["experiment"], {"index", "id", "workload_id", "kernel_id"}))
        return std::unexpected{bad("common_shape")};
    std::expected<runtime::PlacementProvenance, core::Error> provenance =
        [&]() -> std::expected<runtime::PlacementProvenance, core::Error> {
        if (v4)
            return parse_provenance(j["provenance"], raw_failure);
        auto sd = strong<scenario::ScenarioDigest>(j["scenario_digest"], raw_failure);
        core::detail::ExpectedErrorCleanup sd_cleanup{sd};
        if (!sd) {
            retain_parser_failure(raw_failure, sd.error());
            return std::unexpected{bad("common_value")};
        }
        auto rd = strong<scenario::ResolvedScenarioPlanDigest>(j["resolved_scenario_plan_digest"],
                                                               raw_failure);
        core::detail::ExpectedErrorCleanup rd_cleanup{rd};
        if (!rd) {
            retain_parser_failure(raw_failure, rd.error());
            return std::unexpected{bad("common_value")};
        }
        if (!sd || !rd)
            return std::unexpected{bad("common_value")};
        return runtime::detail::PlacementProvenanceConstructionAccess::scenario(std::move(*sd),std::move(*rd));
    }();
    core::detail::ExpectedErrorCleanup provenance_cleanup{provenance};
    if (!provenance)
        return std::unexpected{std::move(provenance.error())};
    auto pf = strong<topology::PhysicalTopologyFingerprint>(j["topology"]["physical_fingerprint"],
                                                            raw_failure);
    core::detail::ExpectedErrorCleanup pf_cleanup{pf};
    if (!pf) {
        retain_parser_failure(raw_failure, pf.error());
        return std::unexpected{bad("common_value")};
    }
    auto ef = strong<topology::EffectiveTopologyFingerprint>(j["topology"]["effective_fingerprint"],
                                                             raw_failure);
    core::detail::ExpectedErrorCleanup ef_cleanup{ef};
    if (!ef) {
        retain_parser_failure(raw_failure, ef.error());
        return std::unexpected{bad("common_value")};
    }
    auto id = strong<scenario::ScenarioIdentifier>(j["experiment"]["id"], raw_failure);
    core::detail::ExpectedErrorCleanup id_cleanup{id};
    if (!id) {
        retain_parser_failure(raw_failure, id.error());
        return std::unexpected{bad("common_value")};
    }
    auto w = strong<scenario::ScenarioIdentifier>(j["experiment"]["workload_id"], raw_failure);
    core::detail::ExpectedErrorCleanup w_cleanup{w};
    if (!w) {
        retain_parser_failure(raw_failure, w.error());
        return std::unexpected{bad("common_value")};
    }
    auto ix = text64(j["experiment"]["index"]);
    core::detail::ExpectedErrorCleanup ix_cleanup{ix};
    if (!ix) {
        retain_parser_failure(raw_failure, ix.error());
        return std::unexpected{bad("common_value")};
    }
    if (!provenance)
        return std::unexpected{std::move(provenance.error())};
    if (!pf || !ef || !id || !w || !ix || !j["experiment"]["kernel_id"].is_string())
        return std::unexpected{bad("common_value")};
    auto k = kernel::parse_kernel_id(j["experiment"]["kernel_id"].get_ref<const std::string&>());
    if (!k)
        return std::unexpected{bad("kernel_id")};
    return Common{std::move(*provenance),
                  std::move(*pf),
                  std::move(*ef),
                  *ix,
                  std::move(*id),
                  std::move(*w),
                  *k};
}
auto parse_cancel_reason_value(const nlohmann::json& j) -> std::optional<CancelReason> {
    if (!j.is_string())
        return std::nullopt;
    if (j == "user_interrupt")
        return CancelReason::user_interrupt;
    if (j == "controller_shutdown")
        return CancelReason::controller_shutdown;
    if (j == "controller_timeout")
        return CancelReason::controller_timeout;
    return std::nullopt;
}
} // namespace

namespace detail {

struct PlacementRequestParser {
    static auto exact_ratio(const nlohmann::json& j,
                            [[maybe_unused]] std::optional<core::Error>* raw_failure = nullptr)
        -> std::expected<RatioValues, core::Error> {
        if (!exact(j, {"numerator", "denominator"}))
            return std::unexpected{bad("ratio_shape")};
        auto numerator = text64(j["numerator"]);
        core::detail::ExpectedErrorCleanup numerator_cleanup{numerator};
        if (!numerator)
            return std::unexpected{std::move(numerator.error())};
        auto denominator = text64(j["denominator"]);
        core::detail::ExpectedErrorCleanup denominator_cleanup{denominator};
        if (!numerator)
            return std::unexpected{std::move(numerator.error())};
        if (!denominator)
            return std::unexpected{std::move(denominator.error())};
        return RatioValues{*numerator, *denominator};
    }

    static auto parse_measurement(const nlohmann::json& j,
                                  std::optional<core::Error>* raw_failure = nullptr)
        -> std::expected<MeasurementValues, core::Error> {
        if (!exact(j, {"observation", "calibration", "warmup", "repetition", "stopping",
                       "comparison_order", "root_seed"}) ||
            !exact(j["observation"], {"target_duration_ns", "minimum_duration_ns",
                                      "maximum_duration_ns", "maximum_harness_overhead"}) ||
            !exact(j["calibration"], {"enabled", "strategy"}) ||
            !exact(j["warmup"],
                   {"minimum", "maximum", "stability_window", "stability_threshold"}) ||
            !exact(j["repetition"], {"minimum_blocks", "maximum_blocks",
                                     "minimum_valid_observations_per_variant"}) ||
            !exact(j["stopping"], {"enabled", "confidence", "ratio_interval_relative_half_width"}))
            return std::unexpected{bad("measurement_shape")};

        auto target = text64(j["observation"]["target_duration_ns"]);
        core::detail::ExpectedErrorCleanup target_cleanup{target};
        if (!target) {
            retain_parser_failure(raw_failure, target.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto minimum = text64(j["observation"]["minimum_duration_ns"]);
        core::detail::ExpectedErrorCleanup minimum_cleanup{minimum};
        if (!minimum) {
            retain_parser_failure(raw_failure, minimum.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto maximum = text64(j["observation"]["maximum_duration_ns"]);
        core::detail::ExpectedErrorCleanup maximum_cleanup{maximum};
        if (!maximum) {
            retain_parser_failure(raw_failure, maximum.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto overhead = exact_ratio(j["observation"]["maximum_harness_overhead"], raw_failure);
        core::detail::ExpectedErrorCleanup overhead_cleanup{overhead};
        if (!overhead) {
            retain_parser_failure(raw_failure, overhead.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto warmup_minimum = text64(j["warmup"]["minimum"]);
        core::detail::ExpectedErrorCleanup warmup_minimum_cleanup{warmup_minimum};
        if (!warmup_minimum) {
            retain_parser_failure(raw_failure, warmup_minimum.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto warmup_maximum = text64(j["warmup"]["maximum"]);
        core::detail::ExpectedErrorCleanup warmup_maximum_cleanup{warmup_maximum};
        if (!warmup_maximum) {
            retain_parser_failure(raw_failure, warmup_maximum.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto warmup_window = text64(j["warmup"]["stability_window"]);
        core::detail::ExpectedErrorCleanup warmup_window_cleanup{warmup_window};
        if (!warmup_window) {
            retain_parser_failure(raw_failure, warmup_window.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto warmup_threshold = exact_ratio(j["warmup"]["stability_threshold"], raw_failure);
        core::detail::ExpectedErrorCleanup warmup_threshold_cleanup{warmup_threshold};
        if (!warmup_threshold) {
            retain_parser_failure(raw_failure, warmup_threshold.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto repetition_minimum = text64(j["repetition"]["minimum_blocks"]);
        core::detail::ExpectedErrorCleanup repetition_minimum_cleanup{repetition_minimum};
        if (!repetition_minimum) {
            retain_parser_failure(raw_failure, repetition_minimum.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto repetition_maximum = text64(j["repetition"]["maximum_blocks"]);
        core::detail::ExpectedErrorCleanup repetition_maximum_cleanup{repetition_maximum};
        if (!repetition_maximum) {
            retain_parser_failure(raw_failure, repetition_maximum.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto minimum_valid = text64(j["repetition"]["minimum_valid_observations_per_variant"]);
        core::detail::ExpectedErrorCleanup minimum_valid_cleanup{minimum_valid};
        if (!minimum_valid) {
            retain_parser_failure(raw_failure, minimum_valid.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto confidence = exact_ratio(j["stopping"]["confidence"], raw_failure);
        core::detail::ExpectedErrorCleanup confidence_cleanup{confidence};
        if (!confidence) {
            retain_parser_failure(raw_failure, confidence.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto precision =
            exact_ratio(j["stopping"]["ratio_interval_relative_half_width"], raw_failure);
        core::detail::ExpectedErrorCleanup precision_cleanup{precision};
        if (!precision) {
            retain_parser_failure(raw_failure, precision.error());
            return std::unexpected{bad("measurement_value")};
        }
        auto root_seed = text64(j["root_seed"]);
        core::detail::ExpectedErrorCleanup root_seed_cleanup{root_seed};
        if (!root_seed) {
            retain_parser_failure(raw_failure, root_seed.error());
            return std::unexpected{bad("measurement_value")};
        }
        if (!target || !minimum || !maximum || !overhead || !warmup_minimum || !warmup_maximum ||
            !warmup_window || !warmup_threshold || !repetition_minimum || !repetition_maximum ||
            !minimum_valid || !confidence || !precision || !root_seed)
            return std::unexpected{bad("measurement_value")};
        if (!j["calibration"]["enabled"].is_boolean() ||
            j["calibration"]["strategy"] != "geometric-refine" ||
            !j["stopping"]["enabled"].is_boolean() || !j["comparison_order"].is_string())
            return std::unexpected{bad("measurement_input_inconsistent")};

        scenario::MatrixComparisonOrder order;
        if (j["comparison_order"] == "randomized-abba")
            order = scenario::MatrixComparisonOrder::randomized_abba;
        else if (j["comparison_order"] == "randomized-baab")
            order = scenario::MatrixComparisonOrder::randomized_baab;
        else if (j["comparison_order"] == "randomized-balanced")
            order = scenario::MatrixComparisonOrder::randomized_balanced;
        else
            return std::unexpected{bad("measurement_input_inconsistent")};

        return MeasurementValues{core::Duration::from_base_units(*target),
                                 core::Duration::from_base_units(*minimum),
                                 core::Duration::from_base_units(*maximum),
                                 *overhead,
                                 j["calibration"]["enabled"].get<bool>(),
                                 PlacementCalibrationStrategy::geometric_refine,
                                 *warmup_minimum,
                                 *warmup_maximum,
                                 *warmup_window,
                                 *warmup_threshold,
                                 *repetition_minimum,
                                 *repetition_maximum,
                                 *minimum_valid,
                                 j["stopping"]["enabled"].get<bool>(),
                                 *confidence,
                                 *precision,
                                 order,
                                 *root_seed};
    }

    static auto parse_kernel(const nlohmann::json& j,
                             std::optional<core::Error>* raw_failure = nullptr)
        -> std::expected<KernelValues, core::Error> {
        if (!j.is_object() || !j.contains("family") || !j.contains("configuration") ||
            !j["family"].is_string())
            return std::unexpected{bad("kernel_configuration_missing")};
        if (j["family"] == "sequential-read-bandwidth") {
            if (!exact(j, {"family", "configuration"}) ||
                !exact(j["configuration"], {"working_set_per_worker_bytes"}))
                return std::unexpected{bad("kernel_configuration_mismatch")};
            auto bytes = text64(j["configuration"]["working_set_per_worker_bytes"], true);
            core::detail::ExpectedErrorCleanup bytes_cleanup{bytes};
            if (!bytes) {
                retain_parser_failure(raw_failure, bytes.error());
                return std::unexpected{bad("kernel_configuration_mismatch")};
            }
            auto configuration = kernel::SequentialReadBandwidthConfig::create(
                core::ByteCount::from_base_units(*bytes));
            core::detail::ExpectedErrorCleanup configuration_cleanup{configuration};
            if (!configuration) {
                retain_parser_failure(raw_failure, configuration.error());
                return std::unexpected{bad("kernel_configuration_mismatch")};
            }
            return KernelValues{
                BandwidthValues{std::move(*configuration), std::nullopt, std::nullopt}};
        }
        if (j["family"] != "dependent-load-latency" ||
            !exact(j, {"family", "configuration", "resolved_permutation_seed", "seed_origin"}) ||
            !exact(j["configuration"], {"working_set_bytes", "pointer_cycle_seed_intent"}))
            return std::unexpected{bad("kernel_configuration_mismatch")};

        auto bytes = text64(j["configuration"]["working_set_bytes"], true);
        core::detail::ExpectedErrorCleanup bytes_cleanup{bytes};
        if (!bytes) {
            retain_parser_failure(raw_failure, bytes.error());
            return std::unexpected{bad("latency_seed_missing")};
        }
        auto seed = text64(j["resolved_permutation_seed"]);
        core::detail::ExpectedErrorCleanup seed_cleanup{seed};
        if (!seed) {
            retain_parser_failure(raw_failure, seed.error());
            return std::unexpected{bad("latency_seed_missing")};
        }
        const auto& intent = j["configuration"]["pointer_cycle_seed_intent"];
        if (!bytes || !seed || !j["seed_origin"].is_string() || !intent.is_object() ||
            !intent.contains("mode") || !intent["mode"].is_string())
            return std::unexpected{bad("latency_seed_missing")};

        kernel::PointerCycleSeedIntent seed_intent =
            kernel::PointerCycleSeedIntent::derive_per_experiment();
        scenario::PointerCycleSeedOrigin origin{};
        if (intent["mode"] == "explicit-value") {
            if (!exact(intent, {"mode", "value"}))
                return std::unexpected{bad("resolved_execution_input_mismatch")};
            auto explicit_seed = text64(intent["value"]);
            core::detail::ExpectedErrorCleanup explicit_seed_cleanup{explicit_seed};
            if (!explicit_seed) {
                retain_parser_failure(raw_failure, explicit_seed.error());
                return std::unexpected{bad("resolved_execution_input_mismatch")};
            }
            seed_intent = kernel::PointerCycleSeedIntent::explicit_value(*explicit_seed);
        } else if (intent["mode"] == "derive-per-experiment") {
            if (!exact(intent, {"mode"}))
                return std::unexpected{bad("resolved_execution_input_mismatch")};
        } else {
            return std::unexpected{bad("resolved_execution_input_mismatch")};
        }
        if (j["seed_origin"] == "explicit-value")
            origin = scenario::PointerCycleSeedOrigin::explicit_value;
        else if (j["seed_origin"] == "derived-value")
            origin = scenario::PointerCycleSeedOrigin::derived_value;
        else
            return std::unexpected{bad("resolved_execution_input_mismatch")};
        auto configuration = kernel::DependentLoadLatencyConfig::create(
            core::ByteCount::from_base_units(*bytes), seed_intent);
        core::detail::ExpectedErrorCleanup configuration_cleanup{configuration};
        if (!configuration) {
            retain_parser_failure(raw_failure, configuration.error());
            return std::unexpected{bad("kernel_configuration_mismatch")};
        }
        return KernelValues{LatencyValues{std::move(*configuration), *seed, origin}};
    }

    static auto parse_execution(const nlohmann::json& j,
                                std::optional<core::Error>* raw_failure = nullptr)
        -> std::expected<std::pair<KernelValues, MeasurementValues>, core::Error> {
        if (!j.is_object() || !j.contains("kernel") || !j.contains("measurement"))
            return std::unexpected{bad("execution_input_invariant_failed")};
        auto kernel_inputs = parse_kernel(j["kernel"], raw_failure);
        core::detail::ExpectedErrorCleanup kernel_inputs_cleanup{kernel_inputs};
        if (!kernel_inputs)
            return std::unexpected{std::move(kernel_inputs.error())};
        auto measurement = parse_measurement(j["measurement"], raw_failure);
        core::detail::ExpectedErrorCleanup measurement_cleanup{measurement};
        if (!kernel_inputs)
            return std::unexpected{std::move(kernel_inputs.error())};
        if (!measurement)
            return std::unexpected{std::move(measurement.error())};
        return std::pair{std::move(*kernel_inputs), std::move(*measurement)};
    }

    static auto parse_monitoring(const nlohmann::json& j,
                                 std::optional<core::Error>* raw_failure = nullptr)
        -> std::expected<MonitoringValues, core::Error> {
        if (!exact(j, {"required", "optional", "sampling", "unavailable_optional_source"}) ||
            !j["required"].is_array() || !j["optional"].is_array() ||
            !exact(j["sampling"],
                   {"before_observation", "after_observation", "during_observation"}) ||
            !j["sampling"]["before_observation"].is_boolean() ||
            !j["sampling"]["after_observation"].is_boolean() ||
            !j["sampling"]["during_observation"].is_object() ||
            !j["sampling"]["during_observation"].contains("enabled") ||
            !j["sampling"]["during_observation"]["enabled"].is_boolean())
            return std::unexpected{bad("monitoring_shape")};
        const bool during = j["sampling"]["during_observation"]["enabled"].get<bool>();
        if (!exact(j["sampling"]["during_observation"],
                   during ? std::initializer_list<std::string_view>{"enabled", "interval"}
                          : std::initializer_list<std::string_view>{"enabled"}))
            return std::unexpected{bad("monitoring_interval_presence")};
        std::optional<core::Duration> interval;
        if (during) {
            auto parsed = text64(j["sampling"]["during_observation"]["interval"]);
            core::detail::ExpectedErrorCleanup parsed_cleanup{parsed};
            if (!parsed) {
                retain_parser_failure(raw_failure, parsed.error());
                return std::unexpected{bad("monitoring_interval")};
            }
            interval = core::Duration::from_base_units(*parsed);
        }
        const auto parse_list = [](const nlohmann::json& values)
            -> std::expected<std::vector<PlacementMonitoringChannel>, core::Error> {
            std::vector<PlacementMonitoringChannel> result;
            for (const auto& value : values) {
                if (!value.is_string())
                    return std::unexpected{bad("monitoring_channel")};
                PlacementMonitoringChannel channel;
                if (value == "page_faults")
                    channel = PlacementMonitoringChannel::page_faults;
                else if (value == "context_switches")
                    channel = PlacementMonitoringChannel::context_switches;
                else if (value == "temperature")
                    channel = PlacementMonitoringChannel::temperature;
                else if (value == "frequency")
                    channel = PlacementMonitoringChannel::frequency;
                else
                    return std::unexpected{bad(value == "throttling" ? "retired_monitoring_channel"
                                                                     : "monitoring_channel")};
                result.push_back(channel);
            }
            return result;
        };
        auto required = parse_list(j["required"]);
        core::detail::ExpectedErrorCleanup required_cleanup{required};
        if (!required)
            return std::unexpected{std::move(required.error())};
        auto optional = parse_list(j["optional"]);
        core::detail::ExpectedErrorCleanup optional_cleanup{optional};
        if (!required || !optional)
            return std::unexpected{std::move(optional.error())};
        PlacementOptionalUnavailableSourcePolicy unavailable;
        if (j["unavailable_optional_source"] == "fail")
            unavailable = PlacementOptionalUnavailableSourcePolicy::fail;
        else if (j["unavailable_optional_source"] == "warn")
            unavailable = PlacementOptionalUnavailableSourcePolicy::warn;
        else if (j["unavailable_optional_source"] == "disable")
            unavailable = PlacementOptionalUnavailableSourcePolicy::disable;
        else
            return std::unexpected{bad("monitoring_optional_policy")};
        return MonitoringValues{std::move(*required),
                                std::move(*optional),
                                j["sampling"]["before_observation"].get<bool>(),
                                j["sampling"]["after_observation"].get<bool>(),
                                during,
                                interval,
                                unavailable};
    }

    static auto parse_region(const nlohmann::json& j,
                             std::optional<core::Error>* raw_failure = nullptr)
        -> std::expected<RegionValues, core::Error> {
        if (!exact(j, {"bytes", "policy", "nodes", "page_mode", "page_size_bytes",
                       "placement_threshold", "prefault", "verify_before", "verify_after"}))
            return std::unexpected{bad("region_shape")};
        auto bytes = text64(j["bytes"]);
        core::detail::ExpectedErrorCleanup bytes_cleanup{bytes};
        if (!bytes) {
            retain_parser_failure(raw_failure, bytes.error());
            return std::unexpected{bad("region_value")};
        }
        auto page_size = text64(j["page_size_bytes"]);
        core::detail::ExpectedErrorCleanup page_size_cleanup{page_size};
        if (!page_size) {
            retain_parser_failure(raw_failure, page_size.error());
            return std::unexpected{bad("region_value")};
        }
        auto threshold = exact_ratio(j["placement_threshold"], raw_failure);
        core::detail::ExpectedErrorCleanup threshold_cleanup{threshold};
        if (!threshold) {
            retain_parser_failure(raw_failure, threshold.error());
            return std::unexpected{bad("region_value")};
        }
        auto parsed_nodes = request_nodes(j["nodes"], raw_failure);
        core::detail::ExpectedErrorCleanup parsed_nodes_cleanup{parsed_nodes};
        if (!parsed_nodes) {
            retain_parser_failure(raw_failure, parsed_nodes.error());
            return std::unexpected{bad("region_value")};
        }
        if (!bytes || !page_size || !threshold || !parsed_nodes)
            return std::unexpected{bad("region_value")};
        if (j["policy"] != "bind" || j["page_mode"] != "base" || !j["prefault"].is_boolean() ||
            !j["verify_before"].is_boolean() || !j["verify_after"].is_boolean())
            return std::unexpected{bad("region_invariant")};

        std::vector<NumaValues> targets;
        targets.reserve(parsed_nodes->size());
        for (auto& parsed : *parsed_nodes)
            targets.push_back(NumaValues{std::move(parsed.stable), parsed.linux});
        return RegionValues{core::ByteCount::from_base_units(*bytes),
                            PlacementPolicy::bind,
                            std::move(targets),
                            PlacementPageMode::base,
                            core::ByteCount::from_base_units(*page_size),
                            *threshold,
                            j["prefault"].get<bool>(),
                            j["verify_before"].get<bool>(),
                            j["verify_after"].get<bool>()};
    }

    static auto parse(const nlohmann::json& j, std::optional<core::Error>* raw_failure = nullptr)
        -> std::expected<PlacementRequest, core::Error> {
        const bool v4 = j.is_object() && j.contains("provenance");
        if (!(v4 ? exact(j, {"provenance", "topology", "experiment", "execution",
                            "reserved_service", "workers"})
                 : exact(j, {"scenario_digest", "resolved_scenario_plan_digest", "topology", "experiment",
                             "execution", "reserved_service", "workers"})))
            return std::unexpected{bad("request_shape")};
        auto parsed_common = common(j, v4, raw_failure);
        core::detail::ExpectedErrorCleanup parsed_common_cleanup{parsed_common};
        if (!parsed_common)
            return std::unexpected{std::move(parsed_common.error())};
        const bool v3 = !v4 && j["execution"].is_object() && j["execution"].contains("monitoring");
        if (v4 && (!j["execution"].is_object() || !j["execution"].contains("monitoring")))
            return std::unexpected{bad("execution_input_invariant_failed")};
        if (!exact(
                j["execution"],
                (v3 || v4) ? std::initializer_list<std::string_view>{"kernel", "measurement", "monitoring"}
                   : std::initializer_list<std::string_view>{"kernel", "measurement"}))
            return std::unexpected{bad("execution_input_invariant_failed")};
        auto execution = parse_execution(j["execution"], raw_failure);
        core::detail::ExpectedErrorCleanup execution_cleanup{execution};
        if (!execution)
            return std::unexpected{std::move(execution.error())};
        std::optional<MonitoringValues> monitoring;
        if (v3 || v4) {
            auto parsed = parse_monitoring(j["execution"]["monitoring"], raw_failure);
            core::detail::ExpectedErrorCleanup parsed_cleanup{parsed};
            if (!parsed)
                return std::unexpected{std::move(parsed.error())};
            monitoring = std::move(*parsed);
        }
        if (!exact(j["reserved_service"], {"linux_cpus", "stable_cores"}) ||
            !j["reserved_service"]["linux_cpus"].is_array() ||
            !j["reserved_service"]["stable_cores"].is_array() || !j["workers"].is_array() ||
            j["workers"].empty())
            return std::unexpected{bad("request_collection")};

        std::vector<core::LinuxCpuId> service_cpus;
        for (const auto& value : j["reserved_service"]["linux_cpus"]) {
            auto id = linux_id<core::LinuxCpuId>(value);
            core::detail::ExpectedErrorCleanup id_cleanup{id};
            if (!id) {
                retain_parser_failure(raw_failure, id.error());
                return std::unexpected{bad("service_cpu_order")};
            }
            service_cpus.push_back(*id);
        }

        std::vector<topology::StableCoreId> service_cores;
        for (const auto& value : j["reserved_service"]["stable_cores"]) {
            auto id = strong<topology::StableCoreId>(value, raw_failure);
            core::detail::ExpectedErrorCleanup id_cleanup{id};
            if (!id) {
                retain_parser_failure(raw_failure, id.error());
                return std::unexpected{bad("service_core_order")};
            }
            service_cores.push_back(std::move(*id));
        }

        std::vector<WorkerValues> workers;
        workers.reserve(j["workers"].size());
        for (const auto& worker : j["workers"]) {
            if (!exact(worker, {"cpu", "region"}))
                return std::unexpected{bad("worker_shape")};
            auto parsed_cpu = cpu(worker["cpu"], raw_failure);
            core::detail::ExpectedErrorCleanup parsed_cpu_cleanup{parsed_cpu};
            if (!parsed_cpu) {
                retain_parser_failure(raw_failure, parsed_cpu.error());
                return std::unexpected{bad("worker_identity_or_region")};
            }
            auto parsed_region = parse_region(worker["region"], raw_failure);
            core::detail::ExpectedErrorCleanup parsed_region_cleanup{parsed_region};
            if (!parsed_region) {
                retain_parser_failure(raw_failure, parsed_region.error());
                return std::unexpected{bad("worker_identity_or_region")};
            }
            if (!parsed_cpu || !parsed_region)
                return std::unexpected{bad("worker_identity_or_region")};
            workers.push_back(WorkerValues{std::move(parsed_cpu->pu), std::move(parsed_cpu->core),
                                           parsed_cpu->linux, std::move(*parsed_region)});
        }

        PlacementRequestConstructionValues values{v4 ? PlacementRequestTransportVersion::v4
                                                      : v3 ? PlacementRequestTransportVersion::v3
                                                           : PlacementRequestTransportVersion::v2,
                                                  std::move(parsed_common->provenance),
                                                  std::move(parsed_common->pf),
                                                  std::move(parsed_common->ef),
                                                  parsed_common->index,
                                                  std::move(parsed_common->id),
                                                  std::move(parsed_common->workload),
                                                  parsed_common->kernel,
                                                  std::move(execution->first),
                                                  std::move(execution->second),
                                                  std::move(monitoring),
                                                  std::move(service_cpus),
                                                  std::move(service_cores),
                                                  std::move(workers)};
        auto constructed = PlacementRequestConstructionAccess::construct(std::move(values));
        detail::HostPlacementRequestResultCleanup constructed_cleanup{constructed};
        if (!constructed) {
            const core::Error* underlying = &constructed.error();
            if (raw_failure) {
                if (!*raw_failure)
                    raw_failure->emplace(std::move(constructed.error()));
                underlying = &**raw_failure;
            }
            const auto& context = underlying->context;
            const auto classification = context.at("classification").get<std::string_view>();
            const auto field = context.value("field", std::string{});
            if (classification == "ratio_invalid") {
                return std::unexpected{bad(field.starts_with("measurement.")
                                               ? "measurement_value"
                                               : "worker_identity_or_region")};
            }
            if (classification == "quantity_not_positive") {
                if (field.starts_with("measurement."))
                    return std::unexpected{bad("measurement_value")};
                if (field == "monitoring.interval")
                    return std::unexpected{bad("monitoring_interval")};
                return std::unexpected{bad("worker_identity_or_region")};
            }
            if (classification == "measurement_order_invalid" ||
                (classification == "enum_invalid" && field.starts_with("measurement.")))
                return std::unexpected{bad("measurement_input_inconsistent")};
            if (classification == "monitoring_invalid") {
                if (field == "monitoring.channel_order")
                    return std::unexpected{bad("monitoring_channel_order_or_duplicate")};
                if (field == "monitoring.channel_overlap")
                    return std::unexpected{bad("monitoring_required_optional_overlap")};
                return std::unexpected{bad("monitoring_interval_presence")};
            }
            if (classification == "kernel_alternative_inconsistent" ||
                classification == "bandwidth_seed_present")
                return std::unexpected{bad("kernel_configuration_mismatch")};
            if (classification == "latency_seed_inconsistent")
                return std::unexpected{bad("resolved_execution_input_mismatch")};
            if (classification == "worker_or_numa_collection_invalid")
                return std::unexpected{bad(field == "workers.empty" ? "request_collection"
                                                                    : "worker_identity_or_region")};
            if (classification == "service_order_invalid")
                return std::unexpected{bad(field == "service.linux_cpus" ? "service_cpu_order"
                                                                         : "service_core_order")};
            if (classification == "region_divisibility_invalid" ||
                (classification == "enum_invalid" && field.starts_with("worker.")))
                return std::unexpected{bad("worker_identity_or_region")};
            return std::unexpected{bad("request_collection")};
        }
        return decltype(constructed)(std::move(constructed));
    }
};

struct CancelRequestParser {
    static auto parse(const Envelope& envelope) -> std::expected<CancelRequest, core::Error> {
        if (envelope.type() != MessageType::cancel_request)
            return std::unexpected{bad("cancel_message_type")};
        auto valid = validate_payload(envelope.type(), envelope.payload());
        if (!valid)
            return std::unexpected{std::move(valid.error())};
        const auto reason = parse_cancel_reason_value(envelope.payload()["reason"]);
        if (!reason)
            return std::unexpected{bad("cancel_shape")};
        return CancelRequest{*reason};
    }
};

} // namespace detail

namespace {
auto region(const nlohmann::json& j) -> std::expected<void, core::Error> {
    auto parsed = detail::PlacementRequestParser::parse_region(j);
    if (!parsed)
        return std::unexpected{parsed.error()};
    return {};
}
auto validate_phase(const nlohmann::json& j, const nlohmann::json& request,
                    std::string_view expected) -> std::expected<void, core::Error> {
    if (!exact(j, {"phase", "pages_examined", "nodes", "unresolved_pages", "placement_ratio",
                   "threshold_passed"}) ||
        j["phase"] != expected || !j["threshold_passed"].is_boolean())
        return std::unexpected{bad("phase_shape")};
    auto examined = text64(j["pages_examined"], true);
    auto unresolved = text64(j["unresolved_pages"]);
    auto observed = nodes(j["nodes"], true);
    auto pr = ratio(j["placement_ratio"]);
    auto threshold = ratio(request["placement_threshold"]);
    if (!examined || !unresolved || !observed || !pr || !threshold)
        return std::unexpected{bad("phase_value")};
    std::uint64_t sum = *unresolved, target = 0;
    for (auto& record : j["nodes"]) {
        auto pages = text64(record["pages"], true);
        if (!pages || *pages > std::numeric_limits<std::uint64_t>::max() - sum)
            return std::unexpected{bad("page_sum")};
        sum += *pages;
        bool requested = false;
        for (auto& rn : request["nodes"])
            if (rn["stable_numa_node_id"] == record["stable_numa_node_id"] &&
                rn["linux_numa_node"] == record["linux_numa_node"])
                requested = true;
        if (requested) {
            if (*pages > std::numeric_limits<std::uint64_t>::max() - target)
                return std::unexpected{bad("page_sum")};
            target += *pages;
        }
    }
    if (sum != *examined || pr->n != target || pr->d != *examined ||
        j["threshold_passed"].get<bool>() != ratio_ge(*pr, *threshold))
        return std::unexpected{bad("page_arithmetic")};
    return {};
}
auto validate_proof_json(const nlohmann::json& j) -> std::expected<Common, core::Error> {
    const bool v2=j.is_object() && j.contains("provenance");
    if (!(v2 ? exact(j,{"provenance","topology","experiment","workers"})
             : exact(j, {"scenario_digest", "resolved_scenario_plan_digest", "topology", "experiment",
                         "workers"})) ||
        !j["workers"].is_array() || j["workers"].empty())
        return std::unexpected{bad("proof_shape")};
    auto c = common(j,v2);
    if (!c)
        return std::unexpected{c.error()};
    for (auto& w : j["workers"]) {
        if (!exact(w, {"requested_cpu", "observed_cpu", "region"}))
            return std::unexpected{bad("proof_worker_shape")};
        auto cp = cpu(w["requested_cpu"]);
        auto& o = w["observed_cpu"];
        if (!cp || !exact(o, {"linux_tid", "affinity", "before", "after", "migration_status"}) ||
            o["migration_status"] != "not_migrated" || !o["affinity"].is_array() ||
            o["affinity"].size() != 1)
            return std::unexpected{bad("observed_cpu_shape")};
        auto tid = text64(o["linux_tid"], true);
        auto aff = linux_id<core::LinuxCpuId>(o["affinity"][0]);
        auto before = linux_id<core::LinuxCpuId>(o["before"]);
        auto after = linux_id<core::LinuxCpuId>(o["after"]);
        if (!tid || !aff || !before || !after || *aff != cp->linux || *before != cp->linux ||
            *after != cp->linux)
            return std::unexpected{bad("observed_cpu_mismatch")};
        auto& r = w["region"];
        if (!exact(r, {"request", "read_only_status", "verification"}) ||
            r["read_only_status"] != "applied" || !region(r["request"]))
            return std::unexpected{bad("proof_region")};
        auto& v = r["verification"];
        if (!exact(v, {"method", "profile", "phases"}) || v["method"] != "full" ||
            v["profile"] != "epycbench.page-placement-full/v1" || !v["phases"].is_array())
            return std::unexpected{bad("verification_shape")};
        std::size_t expected = 0;
        if (r["request"]["verify_before"].get<bool>())
            ++expected;
        if (r["request"]["verify_after"].get<bool>())
            ++expected;
        if (v["phases"].size() != expected)
            return std::unexpected{bad("phase_requiredness")};
        std::size_t i = 0;
        if (r["request"]["verify_before"].get<bool>()) {
            auto z = validate_phase(v["phases"][i++], r["request"], "before");
            if (!z)
                return std::unexpected{z.error()};
        }
        if (r["request"]["verify_after"].get<bool>()) {
            auto z = validate_phase(v["phases"][i], r["request"], "after");
            if (!z)
                return std::unexpected{z.error()};
        }
    }
    return c;
}
} // namespace
auto detail::HostPlacementRequestConstructionAccess::parse_fallible(
    const nlohmann::json& value, std::optional<core::Error>* raw_failure)
    -> std::expected<PlacementRequest, core::Error> {
    return detail::PlacementRequestParser::parse(value, raw_failure);
}

auto parse_placement_request(const nlohmann::json& j)
    -> std::expected<PlacementRequest, core::Error> {
    try {
        return detail::PlacementRequestParser::parse(j);
    } catch (...) {
        return std::unexpected{bad("contained_exception")};
    }
}
auto parse_placement_request(const Envelope& envelope)
    -> std::expected<PlacementRequest, core::Error> {
    if (envelope.type() != MessageType::placement_request)
        return std::unexpected{bad("placement_message_type")};
    auto parsed = parse_placement_request(envelope.payload());
    if (!parsed)
        return parsed;
    const auto expected = envelope.payload_profile()=="epycbench.placement-request/v4"
                            ? PlacementRequestTransportVersion::v4
                          : envelope.payload_profile()=="epycbench.placement-request/v3"
                            ? PlacementRequestTransportVersion::v3
                            : PlacementRequestTransportVersion::v2;
    if (expected != parsed->transport_version())
        return std::unexpected{bad("placement_profile_shape_mismatch")};
    return parsed;
}
auto parse_cancel_request(const Envelope& envelope) -> std::expected<CancelRequest, core::Error> {
    try {
        return detail::CancelRequestParser::parse(envelope);
    } catch (const std::exception&) {
        return std::unexpected{bad("contained_exception")};
    } catch (...) {
        return std::unexpected{bad("contained_unknown_failure")};
    }
}
auto parse_placement_proof(const nlohmann::json& j) -> std::expected<PlacementProof, core::Error> {
    try {
        auto c = validate_proof_json(j);
        if (!c)
            return std::unexpected{c.error()};
        return PlacementProof{j.contains("provenance") ? PlacementProofTransportVersion::v2
                                                       : PlacementProofTransportVersion::v1,
                              std::move(c->provenance),
                              std::move(c->pf), std::move(c->ef),
                              std::move(c->id), std::move(c->workload),
                              c->kernel,        j.get<nlohmann::ordered_json>()};
    } catch (...) {
        return std::unexpected{bad("contained_exception")};
    }
}
auto validate_placement_proof(const PlacementRequest& r, const PlacementProof& p)
    -> std::expected<void, core::Error> {
    try {
        if(p.version!=PlacementProofTransportVersion::v1 &&
           p.version!=PlacementProofTransportVersion::v2)
            return std::unexpected{bad("proof_request_mismatch")};
        const auto expected = r.transport_version() == PlacementRequestTransportVersion::v4
                                  ? PlacementProofTransportVersion::v2
                                  : PlacementProofTransportVersion::v1;
        if (p.version != expected || r.provenance() != p.provenance ||
            r.topology().physical_fingerprint() != p.physical ||
            r.topology().effective_fingerprint() != p.effective ||
            r.experiment().id() != p.experiment_id ||
            r.experiment().workload_id() != p.workload_id ||
            r.experiment().kernel_id() != p.kernel_id ||
            nlohmann::json(r.wire()["experiment"]) != nlohmann::json(p.wire["experiment"]) ||
            r.wire()["workers"].size() != p.wire["workers"].size())
            return std::unexpected{bad("proof_request_mismatch")};
        for (std::size_t i = 0; i < r.wire()["workers"].size(); ++i) {
            auto& rw = r.wire()["workers"][i];
            auto& pw = p.wire["workers"][i];
            if (nlohmann::json(rw["cpu"]) != nlohmann::json(pw["requested_cpu"]) ||
                nlohmann::json(rw["region"]) != nlohmann::json(pw["region"]["request"]))
                return std::unexpected{bad("proof_request_mismatch")};
        }
        return {};
    } catch (...) {
        return std::unexpected{bad("contained_exception")};
    }
}
auto validate_payload(MessageType t, const nlohmann::json& j) -> std::expected<void, core::Error> {
    if (t == MessageType::placement_request) {
        auto x = parse_placement_request(j);
        if (!x)
            return std::unexpected{x.error()};
        return {};
    }
    if (t == MessageType::placement_succeeded) {
        auto x = parse_placement_proof(j);
        if (!x)
            return std::unexpected{x.error()};
        return {};
    }
    if (t == MessageType::request_accepted) {
        if (!exact(j, {"accepted"}) || !j["accepted"].is_boolean() || !j["accepted"].get<bool>())
            return std::unexpected{bad("accepted_shape")};
        return {};
    }
    if (t == MessageType::cancel_request) {
        if (!exact(j, {"reason"}) || !parse_cancel_reason_value(j["reason"]))
            return std::unexpected{bad("cancel_shape")};
        return {};
    }
    if (t == MessageType::request_cancelled) {
        if (!exact(j, {"reason", "phase"}) || !parse_cancel_reason_value(j["reason"]) ||
            !j["phase"].is_string() ||
            (j["phase"] != "startup" && j["phase"] != "placement" && j["phase"] != "cleanup"))
            return std::unexpected{bad("cancelled_shape")};
        return {};
    }
    if (t == MessageType::request_failed) {
        auto x = parse_remote_failure(j);
        if (!x)
            return std::unexpected{x.error()};
        return {};
    }
    return std::unexpected{bad("message_type")};
}
auto PlacementEventState::accept(const Envelope& e) -> std::expected<void, core::Error> {
    auto payload = validate_payload(e.type(), e.payload());
    if (!payload)
        return payload;
    if (e.type() == MessageType::placement_succeeded) {
        auto proof = parse_placement_proof(e.payload());
        if (!proof)
            return std::unexpected{proof.error()};
        const auto expected_profile = proof->version == PlacementProofTransportVersion::v2
                                          ? "epycbench.placement-proof/v2"
                                          : "epycbench.placement-proof/v1";
        if(e.payload_profile()!=expected_profile)
            return std::unexpected{bad("placement_profile_shape_mismatch")};
        auto match = validate_placement_proof(request_, *proof);
        if (!match)
            return match;
    }
    return state_.accept(e);
}
} // namespace epycbench::runtime::ipc
