#include "runtime/finalized_evidence_wire_logical_codec.hpp"
#include "core/json_ownership.hpp"

#include "runtime/finalized_evidence_wire_schema.generated.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <charconv>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

namespace epycbench::runtime::wire_v1 {
namespace {

class ShapeSax final : public nlohmann::json_sax<nlohmann::json> {
  public:
    ShapeSax(std::uint64_t node_limit, std::uint64_t depth_limit) noexcept
        : node_limit_{node_limit}, depth_limit_{depth_limit} {}

    auto null() -> bool override { return scalar(); }
    auto boolean(bool) -> bool override { return scalar(); }
    auto number_integer(number_integer_t) -> bool override { return scalar(); }
    auto number_unsigned(number_unsigned_t) -> bool override { return scalar(); }
    auto number_float(number_float_t, const string_t&) -> bool override {
        failure_ = LogicalCodecErrorCode::unsupported_number;
        return false;
    }
    auto string(string_t&) -> bool override { return scalar(); }
    auto binary(binary_t&) -> bool override {
        failure_ = LogicalCodecErrorCode::malformed;
        return false;
    }
    auto key(string_t&) -> bool override { return true; }
    auto start_object(std::size_t) -> bool override { return start_container(); }
    auto end_object() -> bool override { return end_container(); }
    auto start_array(std::size_t) -> bool override { return start_container(); }
    auto end_array() -> bool override { return end_container(); }
    auto parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&)
        -> bool override {
        if (!failure_) {
            failure_ = LogicalCodecErrorCode::malformed;
        }
        return false;
    }

    [[nodiscard]] auto failure() const noexcept { return failure_; }
    [[nodiscard]] auto nodes() const noexcept { return nodes_; }
    [[nodiscard]] auto greatest_depth() const noexcept { return greatest_depth_; }

  private:
    auto add_node() -> bool {
        if (nodes_ == node_limit_) {
            failure_ = LogicalCodecErrorCode::node_limit;
            return false;
        }
        ++nodes_;
        return true;
    }
    auto scalar() -> bool {
        if (!add_node()) {
            return false;
        }
        if (depth_ == depth_limit_) {
            failure_ = LogicalCodecErrorCode::depth_limit;
            return false;
        }
        greatest_depth_ = std::max(greatest_depth_, depth_ + 1U);
        return true;
    }
    auto start_container() -> bool {
        if (!add_node()) {
            return false;
        }
        if (depth_ == depth_limit_) {
            failure_ = LogicalCodecErrorCode::depth_limit;
            return false;
        }
        ++depth_;
        greatest_depth_ = std::max(greatest_depth_, depth_);
        return true;
    }
    auto end_container() -> bool {
        if (depth_ == 0U) {
            failure_ = LogicalCodecErrorCode::malformed;
            return false;
        }
        --depth_;
        return true;
    }

    std::uint64_t node_limit_{};
    std::uint64_t depth_limit_{};
    std::uint64_t nodes_{};
    std::uint64_t depth_{};
    std::uint64_t greatest_depth_{};
    std::optional<LogicalCodecErrorCode> failure_;
};

auto error(LogicalCodecErrorCode code, std::uint64_t limit = 0U,
           std::uint64_t observed = 0U) -> LogicalCodecError {
    return {.code = code, .limit = {limit}, .observed = {observed}};
}

struct ValidationContext final {
    const NormalizedCapacityParameterValue* parameters{};
    enum class Side : std::uint8_t { none, first, second } side{Side::none};
    std::uint8_t monitoring_presence{};
    std::uint64_t owner_pages{}, owner_targets{}, domain_cpus{};
};

auto formula_value(std::uint32_t index, const ValidationContext& context) -> std::optional<std::uint64_t> {
    const auto& f = detail::formula_nodes[index];
    const auto* p = context.parameters;
    const auto variable = [&](std::string_view name) -> std::optional<std::uint64_t> {
        if (!p) return std::nullopt;
        if (name == "W") return p->workers.value;
        if (name == "B") return p->maximum_blocks.value;
        if (name == "T") return p->maximum_warmup_trials.value;
        if (name == "Bp") return p->base_page_bytes.value;
        if (name == "P") return p->present_cpu_ids.size();
        if (name == "K") return p->maximum_cpu_index.value;
        if (name == "N") return p->maximum_cpu_index.value + 1U;
        if (name == "Q") return p->maximum_cpu_index.value + 2U;
        if (name == "Nd") return p->numa_domain_maxnode.value;
        if (name == "C") return 4U * p->maximum_blocks.value;
        if (name == "D") return std::min<std::uint64_t>(p->workers.value, p->present_cpu_ids.size());
        if (name == "E") return (8U * p->base_page_bytes.value) / 16U;
        if (name == "R") return p->monitoring_rounds.value;
        if (name == "Xp") return p->channels.page;
        if (name == "Xc") return p->channels.context;
        if (name == "Xt") return p->channels.temperature;
        if (name == "Xf") return p->channels.frequency;
        if (name == "Xa") return p->channels.context || p->channels.temperature || p->channels.frequency;
        if (name == "domain_cpu_count") return context.domain_cpus;
        if (name == "owner_total_pages") return context.owner_pages;
        if (name == "owner_target_count") return context.owner_targets;
        return std::nullopt;
    };
    if (f.op == detail::FormulaOp::constant) return f.value;
    if (f.op == detail::FormulaOp::variable) return variable(f.name);
    if (f.op == detail::FormulaOp::reference) {
        if (!p) return std::nullopt;
#define EB_REF(field) if (f.name == "request_known_escaped_bytes." #field) return p->request_known_escaped_bytes.field.value
        EB_REF(scenario_profile); EB_REF(scenario_digest); EB_REF(resolved_plan_digest); EB_REF(workload_id);
        EB_REF(first_variant_id); EB_REF(second_variant_id); EB_REF(monitoring_implementation_ids);
        EB_REF(frequency_driver_ids); EB_REF(frequency_governor_ids); EB_REF(stable_numa_node_ids);
#undef EB_REF
        const bool first = f.name.starts_with("sum_i_min(first_");
        const bool second = f.name.starts_with("sum_i_min(second_");
        if (!first && !second) return std::nullopt;
        const auto& pages = first ? p->first_total_pages : p->second_total_pages;
        const auto& targets = first ? p->first_target_counts : p->second_target_counts;
        std::uint64_t total{};
        for (std::size_t i{}; i < pages.size(); ++i) total += std::min(pages[i].value, targets[i].value);
        return total;
    }
    if (f.op == detail::FormulaOp::conditional) {
        auto condition = variable(f.name); if (!condition || f.arg_count != 2U) return std::nullopt;
        return formula_value(detail::formula_args[f.arg_begin + (*condition ? 0U : 1U)], context);
    }
    std::optional<std::uint64_t> value;
    for (std::uint32_t i{}; i < f.arg_count; ++i) {
        auto next = formula_value(detail::formula_args[f.arg_begin + i], context); if (!next) return std::nullopt;
        if (!value) { value = *next; continue; }
        if (f.op == detail::FormulaOp::add) { if (*next > UINT64_MAX-*value) return std::nullopt; *value += *next; }
        else if (f.op == detail::FormulaOp::multiply) { if (*value && *next > UINT64_MAX / *value) return std::nullopt; *value *= *next; }
        else if (f.op == detail::FormulaOp::minimum) *value = std::min(*value, *next);
        else if (f.op == detail::FormulaOp::maximum) *value = std::max(*value, *next);
        else if (f.op == detail::FormulaOp::ceil_divide) { if (!*next) return std::nullopt; *value = *value / *next + (*value % *next != 0U); }
        else return std::nullopt;
    }
    return value.value_or(0U);
}

auto valid_decimal(std::string_view text, bool signed_value, bool signed32 = false) -> bool {
    if (text.empty() || (text.size() > 1U && text.front() == '0') || text == "-0") return false;
    if (signed_value) {
        std::int64_t parsed{};
        const auto [end, ec] = std::from_chars(text.data(), text.data()+text.size(), parsed);
        return ec == std::errc{} && end == text.data()+text.size() &&
               (!signed32 || (parsed >= std::numeric_limits<std::int32_t>::min() &&
                              parsed <= std::numeric_limits<std::int32_t>::max()));
    }
    std::uint64_t parsed{};
    const auto [end, ec] = std::from_chars(text.data(), text.data()+text.size(), parsed);
    return ec == std::errc{} && end == text.data()+text.size();
}

auto matches_context(std::string_view constraint, std::string_view reason,
                     const nlohmann::json& context) -> bool {
    const auto found = std::ranges::find_if(detail::error_context_constraints,
        [&](const auto& row) { return row.name == constraint; });
    if (found == detail::error_context_constraints.end() || !context.is_object() ||
        !context.contains("kind") || !core::detail::json_string_equal(context["kind"], found->kind))
        return false;
    const auto exact = [&](std::string_view prefix, std::string_view field) {
        if (!constraint.starts_with(prefix)) return true;
        auto expected = constraint.substr(prefix.size());
        if (expected == "abort") expected = "abort_capture";
        if (expected == "complete") expected = "complete_capture";
        return context.contains(field) && core::detail::json_string_equal(context[field], expected);
    };
    if (!exact("phase_", "phase") || !exact("channel_", "channel") ||
        !exact("missing_", "missing_operation")) return false;
    if (constraint.starts_with("kernel_")) {
        const auto family = constraint.starts_with("kernel_bandwidth") ? "bandwidth" :
                            constraint.starts_with("kernel_latency") ? "latency" :
                            context.at("family").get<std::string_view>();
        if (family != "bandwidth" && family != "latency") return false;
        if (!context.contains("family") ||
            !core::detail::json_string_equal(context["family"], family))
            return false;
        const auto implementation = family == std::string_view{"bandwidth"}
                                        ? "scalar_u64_sequential_v1"
                                        : "scalar_u64_random_cycle_v1";
        if (!core::detail::json_string_equal(context["implementation"], implementation))
            return false;
        const std::array phases{
            std::pair{"kernel_bandwidth_pass_count_identity", "pass_count_preparation"},
            std::pair{"kernel_bandwidth_accounting_identity", "work_accounting"},
            std::pair{"kernel_bandwidth_validation_identity", "execution_validation"},
            std::pair{"kernel_latency_validation_identity", "execution_validation"},
            std::pair{"kernel_latency_schedule_identity", "start_state_schedule"},
            std::pair{"kernel_latency_verification_identity", "terminal_verification_preparation"}};
        const auto phase = std::ranges::find(phases, constraint, &decltype(phases)::value_type::first);
        if (phase != phases.end() &&
            !core::detail::json_string_equal(context["kernel_phase"], phase->second))
            return false;
    }
    if (constraint == "memory_protection_state" &&
        !core::detail::json_string_equal(context["reason"], reason))
        return false;
    if (constraint == "mapping_protection" &&
        (core::detail::json_string_equal(context["protection"], "unspecified") ||
         !core::detail::json_string_equal(context["advice"], "none")))
        return false;
    if (constraint == "mapping_advice" &&
        (!core::detail::json_string_equal(context["protection"], "unspecified") ||
         !core::detail::json_string_equal(context["advice"], "no_huge_pages")))
        return false;
    if (constraint == "capacity_acquisition" &&
        context["uncertain"] != (reason == "close")) return false;
    if (constraint == "perf_loss") {
        const auto number = [](const nlohmann::json& value) -> std::optional<std::uint64_t> {
            if (!value.is_string()) return std::nullopt;
            const auto text = value.get<std::string_view>();
            std::uint64_t result{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
            return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
                ? std::optional{result} : std::nullopt;
        };
        const auto& record = context.at("record");
        const auto& ring = context.at("ring");
        const auto& drain = context.at("drain");
        const auto header = number(record.at("header_size"));
        const auto head = number(ring.at("captured_head"));
        const auto tail = number(ring.at("captured_tail"));
        const auto bytes = number(ring.at("ring_bytes"));
        const auto type = record.at("loss_type").get<std::string_view>();
        if (!header || !head || !tail || !bytes || *header < 8U || *header > 65535U ||
            *bytes == 0U || *head < *tail || *head - *tail > *bytes ||
            *header > *head - *tail || (type != "lost" && type != "lost_samples")) return false;
        const auto& count = record.at("lost_count");
        const auto covered = *header >= (type == "lost" ? 24U : 16U);
        if (count.is_null() == covered || (!count.is_null() && !number(count))) return false;
        const auto role = drain.at("role").get<std::string_view>();
        const auto& ordinal = drain.at("ordinal");
        const auto& start = drain.at("started_ns");
        const auto& previous = context.at("last_successful_drain_completed_ns");
        if (role == "unscoped") return ordinal.is_null() && start.is_null() && previous.is_null();
        const auto index = number(ordinal);
        const auto begun = number(start);
        if (!index || !begun) return false;
        if (role == "initial") return *index == 0U && previous.is_null();
        return (role == "scheduled" || role == "final") && *index != 0U &&
               (previous.is_null() || (number(previous) && *number(previous) <= *begun));
    }
    if (constraint.starts_with("nested_")) {
        const auto& phase = context.at("nested_phase");
        if (constraint == "nested_memory_placement_query" ||
            constraint == "nested_memory_placement_evaluation")
            return core::detail::json_string_equal(phase, "finalization");
        if (constraint == "nested_worker_operation_failure")
            return core::detail::json_string_equal(phase, "worker_callback");
        if (!phase.is_null() || !context.at("nested_platform_failure").is_null()) return false;
        const auto inner_code = context.at("failure").at("code").get<std::string_view>();
        const auto profile = std::ranges::find_if(detail::error_failure_rows,
            [&](const auto& row) { return row.code == inner_code; });
        if (profile == detail::error_failure_rows.end() || profile->subsystem != "platform")
            return false;
        if (constraint == "nested_platform_numa_bind_einval") {
            const auto& failure = context.at("failure");
            return core::detail::json_string_equal(failure.at("code"),
                                                   "platform_numa_bind_failed") &&
                   core::detail::json_string_equal(failure.at("operation"), "bind_memory") &&
                   core::detail::json_string_equal(failure.at("reason"), "invalid_bind_request") &&
                   core::detail::json_string_equal(failure.at("posix_errno"), "22");
        }
    }
    return true;
}

auto native_matches(std::string_view rule, const nlohmann::json& native) -> bool {
    if (rule == "forbidden") return native.is_null();
    if (native.is_null()) return rule == "optional_posix_errno";
    const auto& encoded = native.is_object() ? native.at("value") : native;
    if (!encoded.is_string()) return false;
    std::int32_t value{};
    const auto& text = encoded.get_ref<const std::string&>();
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return (rule == "required_posix_errno" || rule == "optional_posix_errno") &&
           ec == std::errc{} && end == text.data() + text.size() && value > 0;
}

auto validate_lower(std::string_view outer_code, const nlohmann::json& lower,
                    const nlohmann::json* effective_context = nullptr)
    -> std::optional<LogicalCodecError> {
    const auto code = lower.at("code").get<std::string_view>();
    const auto operation = lower.at("operation").get<std::string_view>();
    const auto reason = lower.at("reason").get<std::string_view>();
    const auto row = std::ranges::find_if(detail::lower_error_tuple_rows, [&](const auto& item) {
        return item.outer_code == outer_code && item.inner_code == code &&
               item.operation == operation && item.reason == reason;
    });
    if (row == detail::lower_error_tuple_rows.end() ||
        !matches_context(row->context_constraint, reason,
                         effective_context ? *effective_context : lower.at("context")) ||
        !native_matches(row->native_rule, lower.at("posix_errno")))
        return error(LogicalCodecErrorCode::registry_domain);
    return std::nullopt;
}

auto validate_platform_nested(const nlohmann::json& value) -> bool {
    const auto code = value.at("code").get<std::string_view>();
    const auto profile = std::ranges::find_if(detail::error_failure_rows,
        [&](const auto& row) { return row.code == code && row.subsystem == "platform"; });
    if (profile == detail::error_failure_rows.end()) return false;
    const auto operation = value.at("operation").get<std::string_view>();
    const auto reason = value.at("reason").get<std::string_view>();
    const auto tuple = std::ranges::find_if(detail::error_tuple_rows, [&](const auto& row) {
        return row.code == code && row.operation == operation && row.reason == reason;
    });
    return tuple != detail::error_tuple_rows.end() &&
           matches_context(tuple->context_constraint, reason, value.at("context")) &&
           native_matches(tuple->native_rule, value.at("posix_errno"));
}

auto validate_finalized_error(const nlohmann::json& value) -> std::optional<LogicalCodecError> {
    const auto code = value.at("code").get<std::string_view>();
    const auto failure = std::ranges::find_if(detail::error_failure_rows,
        [&](const auto& row) { return row.code == code; });
    if (failure == detail::error_failure_rows.end() ||
        !core::detail::json_string_equal(value.at("subsystem"), failure->subsystem) ||
        !core::detail::json_string_equal(value.at("stable_code"), failure->stable_code) ||
        !core::detail::json_string_equal(value.at("renderer"), failure->renderer) ||
        !core::detail::json_string_equal(value.at("category"), failure->category) ||
        !core::detail::json_string_equal(value.at("severity"), failure->severity) ||
        !core::detail::json_string_equal(value.at("continuation"), failure->continuation))
        return error(LogicalCodecErrorCode::registry_domain);
    const auto operation = value.at("operation").get<std::string_view>();
    const auto reason = value.at("reason").get<std::string_view>();
    const auto tuple = std::ranges::find_if(detail::error_tuple_rows, [&](const auto& row) {
        return row.code == code && row.operation == operation && row.reason == reason &&
            matches_context(row.context_constraint, reason, value.at("context")) &&
            native_matches(row.native_rule, value.at("os_error"));
    });
    if (tuple == detail::error_tuple_rows.end())
        return error(LogicalCodecErrorCode::registry_domain);
    const auto& context = value.at("context");
    if (core::detail::json_string_equal(context.at("kind"), "nested_failure")) {
        const auto& lower = context.at("failure");
        const auto relation = std::ranges::find_if(detail::lower_error_tuple_rows, [&](const auto& row) {
            return row.outer_code == tuple->context_constraint && row.inner_code == lower.at("code").get<std::string_view>() &&
                   row.operation == lower.at("operation").get<std::string_view>() &&
                   row.reason == lower.at("reason").get<std::string_view>();
        });
        if (relation == detail::lower_error_tuple_rows.end())
            return error(LogicalCodecErrorCode::registry_domain);
        const auto nested_kind = context.at("nested_platform_failure").is_null()
                                     ? std::string_view{"none"}
                                     : relation->context_constraint;
        const auto edge = std::ranges::find_if(detail::error_nesting_edges, [&](const auto& item) {
            return item.outer_code == tuple->context_constraint && item.inner_code == nested_kind;
        });
        if (edge == detail::error_nesting_edges.end())
            return error(LogicalCodecErrorCode::registry_domain);
        const nlohmann::json* effective_context = nullptr;
        nlohmann::json nested_context;
        core::detail::JsonCleanup nested_context_cleanup{nested_context};
        if (!context.at("nested_platform_failure").is_null()) {
            const auto& nested = context.at("nested_platform_failure");
            if (!core::detail::json_string_equal(lower.at("context").at("kind"), "none") ||
                !validate_platform_nested(nested))
                return error(LogicalCodecErrorCode::registry_domain);
            if (auto nested_relation = validate_lower(relation->context_constraint, nested))
                return nested_relation;
            nested_context = core::detail::json_object<nlohmann::json>([&](auto& object) {
                core::detail::set_json(object, "kind", "nested_failure");
                core::detail::set_json(object, "failure", nested);
                core::detail::set_json(object, "nested_phase", nullptr);
                core::detail::set_json(object, "nested_platform_failure", nullptr);
            });
            effective_context = &nested_context;
        }
        if (auto failure_result = validate_lower(tuple->context_constraint, lower, effective_context)) return failure_result;
    }
    return std::nullopt;
}

auto monitoring_activity_valid(const nlohmann::json& monitoring) -> bool {
    const auto count = [&](std::string_view key) {
        const auto& text = monitoring.at("activity").at(key).get_ref<const std::string&>();
        std::uint64_t value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
            ? std::optional{value} : std::nullopt;
    };
    const auto& raw = monitoring.at("raw");
    const auto drains = count("context_drains");
    const auto temperatures = count("temperature_attempts");
    const auto frequencies = count("frequency_attempts");
    if (!drains || !temperatures || !frequencies ||
        raw.at("context_switches").size() > *drains ||
        raw.at("temperature").size() != *temperatures) return false;
    for (const auto& sample : raw.at("temperature")) {
        const auto& payload = sample.at("payload");
        if (!core::detail::json_string_equal(sample.at("channel"), "temperature") ||
            !core::detail::json_string_equal(sample.at("phase"), "during_observation") ||
            !core::detail::json_string_equal(sample.at("implementation_id"), "linux-hwmon-k10temp-tctl") ||
            core::detail::json_string_equal(sample.at("status"), "available") != !payload.is_null() ||
            (!payload.is_null() && !core::detail::json_string_equal(payload.at("sensor_kind"), "cpu_control"))) return false;
    }
    std::uint64_t total{};
    std::optional<std::size_t> group_size;
    for (const auto& group : raw.at("frequency")) {
        const auto size = group.at("acquisitions").size();
        if ((group_size && *group_size != size) || size > UINT64_MAX - total) return false;
        group_size = size; total += size;
    }
    return total == *frequencies;
}

auto frequency_domains_valid(const nlohmann::json& monitoring,
                             const NormalizedCapacityParameterValue& parameters) -> bool {
    const auto& classified = monitoring.at("evidence").at("frequency");
    if (classified.is_null()) return true;
    const auto& raw = monitoring.at("raw").at("frequency");
    const auto& metadata = monitoring.at("raw").at("frequency_metadata");
    const auto& domains = classified.at("domains");
    const auto maximum = std::min<std::uint64_t>(parameters.workers.value,
                                               parameters.present_cpu_ids.size());
    if (raw.empty() || raw.size() > maximum || metadata.size() != raw.size() ||
        domains.size() != raw.size()) return false;
    std::vector<std::uint64_t> seen;
    seen.reserve(parameters.present_cpu_ids.size());
    std::optional<std::uint64_t> previous_first;
    for (std::size_t index{}; index < raw.size(); ++index) {
        const auto& domain = raw[index].at("domain_id");
        if (metadata[index].at("domain_id") != domain || domains[index].at("domain_id") != domain ||
            domains[index].at("source_kind") != metadata[index].at("source_kind")) return false;
        const auto& cpus = domain.at("cpus");
        if (cpus.empty() || cpus.size() > parameters.present_cpu_ids.size() - seen.size())
            return false;
        std::optional<std::uint64_t> previous;
        for (const auto& value : cpus) {
            const auto& text = value.get_ref<const std::string&>();
            std::uint64_t cpu{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), cpu);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                (previous && cpu <= *previous) ||
                std::ranges::none_of(parameters.present_cpu_ids,
                    [&](const auto present) { return present.value == cpu; }) ||
                std::ranges::find(seen, cpu) != seen.end()) return false;
            if (!previous && previous_first && cpu <= *previous_first) return false;
            if (!previous) previous_first = cpu;
            previous = cpu;
            seen.push_back(cpu);
        }
        for (const auto& sample : raw[index].at("acquisitions")) {
            const auto& payload = sample.at("payload");
            const auto expected_id = core::detail::json_string_equal(metadata[index].at("source_kind"), "hardware_current")
                ? "linux-cpufreq-cpuinfo-cur-freq" : "linux-cpufreq-scaling-cur-freq";
            if (!core::detail::json_string_equal(sample.at("channel"), "frequency") ||
                !core::detail::json_string_equal(sample.at("phase"), "during_observation") ||
                !core::detail::json_string_equal(sample.at("implementation_id"), expected_id) ||
                core::detail::json_string_equal(sample.at("status"), "available") != !payload.is_null() ||
                (!payload.is_null() && (payload.at("domain_id") != domain ||
                 payload.at("source_kind") != metadata[index].at("source_kind")))) return false;
        }
    }
    return true;
}

auto validate_node(std::uint32_t index, const nlohmann::json& value, ValidationContext context)
    -> std::optional<LogicalCodecError> {
    const auto& node = detail::schema_nodes[index];
    if (node.kind == detail::SchemaKind::scalar) {
        const auto cls = node.scalar_class;
        const auto maximum = formula_value(node.maximum_formula, context);
        const auto encoded = detail::count_wire_json_value(value);
        if (!encoded)
            return error(detail::logical_render_failure(encoded.error().code,
                                                        LogicalCodecErrorCode::registry_domain));
        const auto encoded_size = *encoded;
        if (!maximum || encoded_size < node.minimum_bytes || encoded_size > *maximum)
            return error(LogicalCodecErrorCode::registry_domain,
                         maximum.value_or(0U), encoded_size);
        if (cls == detail::ScalarClass::boolean) return value.is_boolean() ? std::nullopt : std::optional{error(LogicalCodecErrorCode::registry_domain)};
        if (!value.is_string()) return error(LogicalCodecErrorCode::registry_domain);
        const auto& text = value.get_ref<const std::string&>();
        if (cls == detail::ScalarClass::token) {
            const auto begin = detail::scalar_tokens.begin() + node.token_begin;
            if (std::find(begin, begin + node.token_count, text) == begin + node.token_count) return error(LogicalCodecErrorCode::registry_domain);
        } else if (cls == detail::ScalarClass::unsigned_decimal && !valid_decimal(text, false)) return error(LogicalCodecErrorCode::registry_domain);
        else if (cls == detail::ScalarClass::signed_decimal32 && !valid_decimal(text, true, true)) return error(LogicalCodecErrorCode::registry_domain);
        else if (cls == detail::ScalarClass::signed_decimal64 && !valid_decimal(text, true)) return error(LogicalCodecErrorCode::registry_domain);
        else if (cls == detail::ScalarClass::hex16 || cls == detail::ScalarClass::hex64) {
            const auto length = cls == detail::ScalarClass::hex16 ? 16U : 64U;
            if (text.size() != length || !std::ranges::all_of(text, [](char c){ return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return error(LogicalCodecErrorCode::registry_domain);
        }
        return std::nullopt;
    }
    if (node.kind == detail::SchemaKind::option) return value.is_null() ? std::nullopt : validate_node(node.child, value, context);
    if (node.kind == detail::SchemaKind::array) {
        if (!value.is_array()) return error(LogicalCodecErrorCode::registry_shape);
        const auto limit = node.cardinality_presence != 0U &&
                (context.monitoring_presence & node.cardinality_presence) == 0U
            ? std::optional<std::uint64_t>{0U}
            : formula_value(node.cardinality_formula, context);
        const auto& formula = detail::formula_nodes[node.cardinality_formula];
        const bool frequency_cpus = formula.op == detail::FormulaOp::variable &&
                                    formula.name == "domain_cpu_count";
        const bool frequency_domains = node.special == detail::SchemaSpecial::frequency_domains;
        const bool frequency_bound = frequency_cpus || frequency_domains;
        const bool round_bound = node.cardinality_presence == 2U ||
            node.cardinality_presence == 4U ||
            (formula.op == detail::FormulaOp::variable && formula.name == "R");
        if (!limit || (node.exact_cardinality && !frequency_bound && !round_bound
                          ? value.size() != *limit : value.size() > *limit) ||
            (frequency_bound && *limit != 0U && value.empty()))
            return error(LogicalCodecErrorCode::registry_cardinality,
                         limit.value_or(0U), value.size());
        for (std::size_t i{}; i < value.size(); ++i) {
            auto child_context = context;
            if (node.special == detail::SchemaSpecial::finalized_workers) {
                const bool first = context.side == ValidationContext::Side::first;
                const auto& pages = first ? context.parameters->first_total_pages : context.parameters->second_total_pages;
                const auto& targets = first ? context.parameters->first_target_counts : context.parameters->second_target_counts;
                if (i >= pages.size() || i >= targets.size()) return error(LogicalCodecErrorCode::registry_cardinality);
                child_context.owner_pages = pages[i].value; child_context.owner_targets = targets[i].value;
            } else if (node.special == detail::SchemaSpecial::frequency_domains) {
                if (i >= context.parameters->domain_cpu_ids.size()) return error(LogicalCodecErrorCode::registry_cardinality);
                // The synthetic partition sizes the envelope; actual policies
                // may share a domain or cover a strict subset of present CPUs.
                child_context.domain_cpus = context.parameters->present_cpu_ids.size();
            }
            if (auto failure = validate_node(node.child, value[i], child_context)) return failure;
        }
        return std::nullopt;
    }
    if (node.kind == detail::SchemaKind::sum) {
        if (!value.is_object() || !value.contains(node.discriminator) || !value[node.discriminator].is_string()) return error(LogicalCodecErrorCode::registry_shape);
        const auto token = value[node.discriminator].get<std::string_view>();
        for (std::uint32_t i{}; i < node.edge_count; ++i) { const auto& edge=detail::schema_edges[node.edge_begin+i]; if (edge.token == token) return validate_node(edge.child, value, context); }
        return error(LogicalCodecErrorCode::registry_domain);
    }
    if (!value.is_object() || value.size() != node.edge_count) return error(LogicalCodecErrorCode::registry_shape);
    for (std::uint32_t i{}; i < node.edge_count; ++i) {
        const auto& edge = detail::schema_edges[node.edge_begin+i];
        if (!value.contains(edge.key)) return error(LogicalCodecErrorCode::registry_shape);
        auto child_context = context;
        if (node.special == detail::SchemaSpecial::finalization) child_context.side = i == 0U ? ValidationContext::Side::first : ValidationContext::Side::second;
        if (auto failure = validate_node(edge.child, value[edge.key], child_context)) return failure;
        if (index == detail::monitoring_result_definition && edge.key == "evidence") {
            // Evidence was fully validated above; no access to unchecked members.
            context.monitoring_presence = 0U;
            constexpr std::array<std::string_view, 4> keys{
                "page_faults", "context_switches", "temperature", "frequency"};
            for (std::size_t channel{}; channel < keys.size(); ++channel)
                if (!value[edge.key][keys[channel]].is_null())
                    context.monitoring_presence |= static_cast<std::uint8_t>(1U << channel);
        }
    }
    if (index == detail::monitoring_result_definition &&
        (!monitoring_activity_valid(value) ||
         !frequency_domains_valid(value, *context.parameters)))
        return error(LogicalCodecErrorCode::registry_cardinality);
    if (index == detail::finalized_error) return validate_finalized_error(value);
    return std::nullopt;
}

} // namespace

LogicalBody::LogicalBody(detail::WireJson value, std::size_t nodes)
    : value_(std::move(value)), nodes_(nodes) {}
LogicalBody::LogicalBody(LogicalBody&&) noexcept = default;
auto LogicalBody::operator=(LogicalBody&&) noexcept -> LogicalBody& = default;
LogicalBody::~LogicalBody() = default;
auto LogicalBody::value() const noexcept -> const nlohmann::json& { return value_.value; }

auto decode_logical_body(std::string_view bytes,
                         const CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<LogicalBody, LogicalCodecError> {
    const auto byte_limit = capacity.resource_plan.controller_receive_bytes.value;
    if (bytes.size() > byte_limit) {
        return std::unexpected{error(LogicalCodecErrorCode::byte_limit, byte_limit, bytes.size())};
    }
    try {
        ShapeSax shape{capacity.resource_plan.controller_node_slots.value,
                       capacity.logical_shape.parser_depth.value};
        const auto shape_ok = nlohmann::json::sax_parse(bytes, &shape);
        if (!shape_ok) {
            const auto code = shape.failure().value_or(LogicalCodecErrorCode::malformed);
            const auto limit = code == LogicalCodecErrorCode::node_limit
                                   ? capacity.resource_plan.controller_node_slots.value
                               : code == LogicalCodecErrorCode::depth_limit
                                   ? capacity.logical_shape.parser_depth.value
                                   : 0U;
            const auto observed = code == LogicalCodecErrorCode::node_limit
                                      ? shape.nodes() + 1U
                                  : code == LogicalCodecErrorCode::depth_limit
                                      ? shape.greatest_depth() + 1U
                                      : 0U;
            return std::unexpected{error(code, limit, observed)};
        }

        auto parsed =
            detail::parse_wire_json(bytes, static_cast<std::size_t>(shape.greatest_depth()));
        if (!parsed) {
            return std::unexpected{error(parsed.error().code == detail::WireJsonFailure::duplicate
                                             ? LogicalCodecErrorCode::noncanonical
                                             : LogicalCodecErrorCode::malformed)};
        }
        const auto canonical =
            detail::render_wire_json(parsed->value, parsed->retirement.size(),
                                     static_cast<std::size_t>(shape.nodes()), bytes.size());
        if (!canonical)
            return std::unexpected{error(detail::logical_render_failure(
                canonical.error().code, LogicalCodecErrorCode::noncanonical))};
        if (*canonical != bytes)
            return std::unexpected{error(LogicalCodecErrorCode::noncanonical)};
        return LogicalBody{std::move(*parsed), static_cast<std::size_t>(shape.nodes())};
    } catch (const std::bad_alloc&) {
        return std::unexpected{error(LogicalCodecErrorCode::allocation_failure)};
    } catch (const std::length_error&) {
        return std::unexpected{error(LogicalCodecErrorCode::allocation_failure)};
    } catch (...) {
        return std::unexpected{error(LogicalCodecErrorCode::malformed)};
    }
}

auto encode_logical_body(const LogicalBody& body,
                         const CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<std::string, LogicalCodecError> {
    try {
        const auto limit = capacity.resource_plan.runner_encode_bytes.value;
        auto canonical = detail::render_wire_json(body.value(), body.value_.retirement.size(),
                                                  body.nodes_, static_cast<std::size_t>(limit));
        if (!canonical) {
            if (canonical.error().code == detail::WireJsonFailure::byte_limit)
                return std::unexpected{
                    error(LogicalCodecErrorCode::byte_limit, limit, canonical.error().observed)};
            return std::unexpected{error(detail::logical_render_failure(
                canonical.error().code, LogicalCodecErrorCode::unsupported_number))};
        }
        return std::move(*canonical);
    } catch (const std::bad_alloc&) {
        return std::unexpected{error(LogicalCodecErrorCode::allocation_failure)};
    } catch (const std::length_error&) {
        return std::unexpected{error(LogicalCodecErrorCode::allocation_failure)};
    } catch (...) {
        return std::unexpected{error(LogicalCodecErrorCode::malformed)};
    }
}

auto decode_evidence_component(EvidenceComponentKind kind, EvidenceComponentSide side,
                               std::string_view bytes,
                               const CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<LogicalBody, LogicalCodecError> {
    const auto selected = detail::evidence_component_node(kind);
    if (!selected) return std::unexpected{error(LogicalCodecErrorCode::registry_shape)};
    const auto root = *selected;
    ValidationContext context{&capacity.prerequisite.parameters};
    switch (side) {
    case EvidenceComponentSide::first: context.side = ValidationContext::Side::first; break;
    case EvidenceComponentSide::second: context.side = ValidationContext::Side::second; break;
    default: return std::unexpected{error(LogicalCodecErrorCode::registry_domain)};
    }
    auto body = decode_logical_body(bytes, capacity);
    if (!body) return std::unexpected{body.error()};
    try {
        if (auto failure = validate_node(root, body->value(), context))
            return std::unexpected{*failure};
        return body;
    } catch (const std::bad_alloc&) {
        return std::unexpected{error(LogicalCodecErrorCode::allocation_failure)};
    } catch (...) {
        return std::unexpected{error(LogicalCodecErrorCode::registry_shape)};
    }
}

auto decode_evidence_body(std::string_view bytes,
                          const CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<LogicalBody, LogicalCodecError> {
    auto body = decode_logical_body(bytes, capacity);
    if (!body) return std::unexpected(body.error());
    try {
        ValidationContext context{&capacity.prerequisite.parameters};
        if (auto failure = validate_node(detail::evidence_root, body->value(), context))
            return std::unexpected(*failure);
        return body;
    } catch (const std::bad_alloc&) {
        return std::unexpected{error(LogicalCodecErrorCode::allocation_failure)};
    } catch (...) {
        return std::unexpected{error(LogicalCodecErrorCode::registry_shape)};
    }
}

} // namespace epycbench::runtime::wire_v1
