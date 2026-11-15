#include "runtime/finalized_evidence_wire_capacity.hpp"

#include <algorithm>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <vector>

#include "runtime/finalized_evidence_wire_schema.generated.hpp"

namespace epycbench::runtime::wire_v1 {
namespace {

using Result = std::expected<std::uint64_t, CapacityError>;

[[nodiscard]] auto overflow() noexcept -> std::unexpected<CapacityError> {
    return std::unexpected(
        CapacityError{CapacityErrorCode::arithmetic_overflow, CapacityField::input});
}

[[nodiscard]] auto add(std::uint64_t left, std::uint64_t right) noexcept -> Result {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return overflow();
    }
    return left + right;
}

[[nodiscard]] auto mul(std::uint64_t left, std::uint64_t right) noexcept -> Result {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return overflow();
    }
    return left * right;
}

template <typename... Rest>
[[nodiscard]] auto product(std::uint64_t first, Rest... rest) noexcept -> Result {
    Result result{first};
    ((result = result.and_then([rest](std::uint64_t value) { return mul(value, rest); })), ...);
    return result;
}

[[nodiscard]] auto sum_min(std::span<const PageCount> pages,
                           std::uint64_t domain_maxnode) noexcept -> Result {
    std::uint64_t result{};
    for (std::size_t index{}; index < pages.size(); ++index) {
        auto next = add(result, std::min(pages[index].value, domain_maxnode));
        if (!next) {
            return next;
        }
        result = *next;
    }
    return result;
}

[[nodiscard]] auto invalid(CapacityField field = CapacityField::input) noexcept
    -> std::unexpected<CapacityError> {
    return std::unexpected(CapacityError{CapacityErrorCode::invalid_input, field});
}

[[nodiscard]] auto count(Result value) noexcept -> std::expected<CapacityCount, CapacityError> {
    if (!value) {
        return std::unexpected(value.error());
    }
    return CapacityCount{*value};
}

} // namespace

auto normalize_capacity_parameters(const CapacityParameterCandidate& candidate) noexcept
    -> std::expected<NormalizedCapacityParameters, CapacityError> {
    try {
        const auto w = candidate.workers.value;
        if (w == 0U || candidate.maximum_blocks.value == 0U ||
            candidate.base_page_bytes.value == 0U || candidate.numa_domain_maxnode.value == 0U ||
            candidate.present_cpu_ids.empty()) {
            return invalid();
        }
        auto universe =
            std::vector<CpuId>(candidate.present_cpu_ids.begin(), candidate.present_cpu_ids.end());
        std::ranges::sort(universe, {}, &CpuId::value);
        if (std::ranges::adjacent_find(universe) != universe.end()) {
            return invalid(CapacityField::present_cpu_universe);
        }
        if (universe.back().value > candidate.maximum_cpu_index.value) {
            return std::unexpected(CapacityError{CapacityErrorCode::inconsistent_parameters,
                                                 CapacityField::present_cpu_universe,
                                                 CapacityCount{universe.back().value},
                                                 CapacityCount{candidate.maximum_cpu_index.value}});
        }
        const auto domain_count = std::min<std::uint64_t>(w, universe.size());
        if (candidate.domain_cpu_ids.size() != domain_count) {
            return invalid(CapacityField::cpu_partition);
        }
        std::vector<std::vector<CpuId>> domains;
        domains.reserve(candidate.domain_cpu_ids.size());
        std::vector<CpuId> domain_union;
        domain_union.reserve(universe.size());
        for (const auto domain : candidate.domain_cpu_ids) {
            if (domain.empty())
                return invalid(CapacityField::cpu_partition);
            domains.emplace_back(domain.begin(), domain.end());
            std::ranges::sort(domains.back(), {}, &CpuId::value);
            if (std::ranges::adjacent_find(domains.back()) != domains.back().end())
                return invalid(CapacityField::cpu_partition);
            domain_union.insert(domain_union.end(), domains.back().begin(), domains.back().end());
        }
        std::ranges::sort(domains);
        std::ranges::sort(domain_union, {}, &CpuId::value);
        if (domain_union != universe) {
            return std::unexpected(CapacityError{
                CapacityErrorCode::inconsistent_parameters, CapacityField::cpu_partition,
                CapacityCount{universe.size()}, CapacityCount{domain_union.size()}});
        }
        const auto valid_vectors =
            [w, nd = candidate.numa_domain_maxnode.value](std::span<const PageCount> pages,
                                                          std::span<const TargetCount> targets) {
                if (pages.size() != w || targets.size() != w)
                    return false;
                for (std::size_t index{}; index < pages.size(); ++index) {
                    if (pages[index].value == 0U || targets[index].value == 0U ||
                        targets[index].value > nd)
                        return false;
                }
                return true;
            };
        if (!valid_vectors(candidate.first_total_pages, candidate.first_target_counts))
            return invalid(CapacityField::first_page_vector);
        if (!valid_vectors(candidate.second_total_pages, candidate.second_target_counts))
            return invalid(CapacityField::second_page_vector);

        return NormalizedCapacityParameters(NormalizedCapacityParameterValue{
            candidate.workers,
            candidate.maximum_blocks,
            candidate.maximum_warmup_trials,
            candidate.base_page_bytes,
            candidate.maximum_cpu_index,
            candidate.numa_domain_maxnode,
            candidate.monitoring_rounds,
            candidate.channels,
            candidate.request_known_escaped_bytes,
            std::move(universe),
            std::move(domains),
            {candidate.first_total_pages.begin(), candidate.first_total_pages.end()},
            {candidate.second_total_pages.begin(), candidate.second_total_pages.end()},
            {candidate.first_target_counts.begin(), candidate.first_target_counts.end()},
            {candidate.second_target_counts.begin(), candidate.second_target_counts.end()}});
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CapacityError{CapacityErrorCode::allocation_failure, CapacityField::input});
    } catch (const std::length_error&) {
        return std::unexpected(
            CapacityError{CapacityErrorCode::allocation_failure, CapacityField::input});
    }
}

auto calculate_capacity_prerequisite(const NormalizedCapacityParameters& validated,
                                     LocalCapacityLimits limits) noexcept
    -> std::expected<CalculatedCapacityPrerequisite, CapacityError> {
    try {
        const auto& in = validated.value();
        const auto w = in.workers.value;
        const auto b = in.maximum_blocks.value;
        const auto bp = in.base_page_bytes.value;
        const auto p = static_cast<std::uint64_t>(in.present_cpu_ids.size());
        const auto k = in.maximum_cpu_index.value;
        const auto r = in.monitoring_rounds.value;
        const auto length_exceeds = [&](std::size_t length) {
            return static_cast<std::uint64_t>(length) > limits.size_max;
        };
        if (length_exceeds(in.present_cpu_ids.size()) || length_exceeds(in.domain_cpu_ids.size()) ||
            length_exceeds(in.first_total_pages.size()) ||
            length_exceeds(in.first_target_counts.size()) ||
            length_exceeds(in.second_total_pages.size()) ||
            length_exceeds(in.second_target_counts.size()) ||
            std::ranges::any_of(in.domain_cpu_ids, [&](const auto& domain) {
                return length_exceeds(domain.size());
            })) {
            return std::unexpected(
                CapacityError{CapacityErrorCode::size_unrepresentable, CapacityField::input});
        }
        const auto d = std::min(w, p);

        auto c_result = mul(4U, b);
        auto e_numerator = mul(8U, bp);
        auto n_result = add(k, 1U);
        if (!c_result || !e_numerator || !n_result)
            return overflow();
        const auto c = *c_result;
        const auto e = *e_numerator / 16U;
        const auto n = *n_result;
        auto q_result = add(n, 1U);
        if (!q_result)
            return overflow();
        const auto q = *q_result;
        const std::uint64_t xp = in.channels.page ? 1U : 0U;
        const std::uint64_t xc = in.channels.context ? 1U : 0U;
        const std::uint64_t xt = in.channels.temperature ? 1U : 0U;
        const std::uint64_t xf = in.channels.frequency ? 1U : 0U;
        const std::uint64_t xa = (xc | xt | xf);
        auto rp1 = add(r, 1U);
        auto rp3 = add(r, 3U);
        auto wp1 = add(w, 1U);
        auto cp2 = add(c, 2U);
        auto failed_each = add(std::max(w, std::uint64_t{2}), w);
        if (!rp1 || !rp3 || !wp1 || !cp2 || !failed_each)
            return overflow();
        failed_each = add(*failed_each, 1U);
        if (!failed_each)
            return overflow();

#define EB_TRY_COUNT(name, expression)                                                             \
    auto name##_result = count((expression));                                                      \
    if (!name##_result)                                                                            \
        return std::unexpected(name##_result.error());                                             \
    const auto name = *name##_result
        EB_TRY_COUNT(first_pages, sum_min(in.first_total_pages, in.numa_domain_maxnode.value));
        EB_TRY_COUNT(second_pages, sum_min(in.second_total_pages, in.numa_domain_maxnode.value));
        EB_TRY_COUNT(workers, product(c, w));
        EB_TRY_COUNT(failed_errors, product(b, *failed_each));
        auto seven_w = mul(7U, w);
        if (!seven_w)
            return overflow();
        auto terminal_errors_value = add(*seven_w, 8U);
        if (!terminal_errors_value)
            return overflow();
        terminal_errors_value = add(*terminal_errors_value, std::max(w, std::uint64_t{2}));
        EB_TRY_COUNT(terminal_errors, terminal_errors_value);
        EB_TRY_COUNT(raw_page, product(2U, c, xp, w));
        EB_TRY_COUNT(raw_context, product(c, xc, w, *rp1));
        EB_TRY_COUNT(context_events, product(c, xc, w, *rp1, e));
        EB_TRY_COUNT(context_offsets, product(c, xc, *wp1));
        EB_TRY_COUNT(raw_temperature, product(c, xt, r));
        EB_TRY_COUNT(freq_domains, product(c, xf, d));
        EB_TRY_COUNT(freq_acquisitions, product(c, xf, d, r));
        EB_TRY_COUNT(freq_memberships, product(c, xf, p, *rp3));
        EB_TRY_COUNT(validity_checks, product(40U, b));
        EB_TRY_COUNT(finalized_workers, product(2U, w));
        EB_TRY_COUNT(classified_page, product(c, xp, w));
        EB_TRY_COUNT(classified_context, product(c, xc, w));
        ComponentMaxima components{{b},
                                   {c},
                                   {b},
                                   {b},
                                   workers,
                                   workers,
                                   workers,
                                   validity_checks,
                                   finalized_workers,
                                   failed_errors,
                                   terminal_errors,
                                   raw_page,
                                   raw_context,
                                   context_events,
                                   context_offsets,
                                   raw_temperature,
                                   freq_domains,
                                   freq_acquisitions,
                                   freq_memberships,
                                   freq_domains,
                                   classified_page,
                                   classified_context,
                                   freq_domains,
                                   first_pages,
                                   second_pages};

        EB_TRY_COUNT(page_sources, product(xp, w));
        EB_TRY_COUNT(context_sources, product(xc, w));
        EB_TRY_COUNT(context_coverage, product(3U, xc, w));
        EB_TRY_COUNT(frequency_sources, product(xf, d));
        EB_TRY_COUNT(persistent_freq_cpus, product(2U, xf, p));
        EB_TRY_COUNT(context_scratch, product(4U, xc, w));
        EB_TRY_COUNT(temperature_scratch, product(xt, r));
        EB_TRY_COUNT(frequency_view_scratch, product(xf, r));
        EB_TRY_COUNT(whole_context, product(3U, xc, w));
        auto whole = add(1U, whole_context.value);
        if (!whole)
            return overflow();
        whole = add(*whole, xa);
        if (!whole)
            return overflow();
        EB_TRY_COUNT(prep_context, product(2U, xc, w));
        auto prep = add(1U, prep_context.value);
        if (!prep)
            return overflow();
        auto completion = add(1U, context_sources.value);
        if (!completion)
            return overflow();
        EB_TRY_COUNT(cpu_ops, product(7U, n));
        EB_TRY_COUNT(nq, product(n, q));
        auto hwmon_ops_value = add(q, nq.value);
        if (!hwmon_ops_value)
            return overflow();
        EB_TRY_COUNT(dynamic_raw, product(2U, bp, xf, d, *cp2));
        EB_TRY_COUNT(dynamic_escaped, product(4U, bp, xf, d, *cp2));
        MonitoringLowerStocks stocks{{w},
                                     page_sources,
                                     context_sources,
                                     context_coverage,
                                     {xt},
                                     frequency_sources,
                                     frequency_sources,
                                     persistent_freq_cpus,
                                     {4U},
                                     page_sources,
                                     context_scratch,
                                     temperature_scratch,
                                     frequency_view_scratch,
                                     frequency_sources,
                                     {*whole},
                                     {*prep},
                                     {*completion},
                                     {n},
                                     {q},
                                     cpu_ops,
                                     {q},
                                     {*hwmon_ops_value},
                                     dynamic_raw,
                                     dynamic_escaped};

        auto raw_dest_value = add(raw_page.value, raw_context.value);
        if (!raw_dest_value)
            return overflow();
        raw_dest_value = add(*raw_dest_value, raw_temperature.value);
        if (!raw_dest_value)
            return overflow();
        raw_dest_value = add(*raw_dest_value, freq_acquisitions.value);
        if (!raw_dest_value)
            return overflow();
        raw_dest_value = add(*raw_dest_value, freq_domains.value);
        if (!raw_dest_value)
            return overflow();
        auto classified_dest_value = add(classified_page.value, classified_context.value);
        if (!classified_dest_value)
            return overflow();
        classified_dest_value = add(*classified_dest_value, freq_domains.value);
        if (!classified_dest_value)
            return overflow();
        auto error_dest_value = add(failed_errors.value, terminal_errors.value);
        if (!error_dest_value)
            return overflow();
        EvidenceDestinations destinations{{1U},
                                          {4U},
                                          {1U},
                                          {1U},
                                          {b},
                                          {b},
                                          {1U},
                                          finalized_workers,
                                          {*raw_dest_value},
                                          {*classified_dest_value},
                                          {*error_dest_value},
                                          finalized_workers};
        const auto exceeds_component = [&](std::uint64_t value) { return value > limits.size_max; };
        const std::array component_values{b,
                                          c,
                                          b,
                                          b,
                                          workers.value,
                                          workers.value,
                                          workers.value,
                                          validity_checks.value,
                                          finalized_workers.value,
                                          failed_errors.value,
                                          terminal_errors.value,
                                          raw_page.value,
                                          raw_context.value,
                                          context_events.value,
                                          context_offsets.value,
                                          raw_temperature.value,
                                          freq_domains.value,
                                          freq_acquisitions.value,
                                          freq_memberships.value,
                                          freq_domains.value,
                                          classified_page.value,
                                          classified_context.value,
                                          freq_domains.value,
                                          first_pages.value,
                                          second_pages.value};
        if (std::ranges::any_of(component_values, exceeds_component)) {
            return std::unexpected(CapacityError{CapacityErrorCode::size_unrepresentable,
                                                 CapacityField::component_maxima});
        }
        const std::array stock_values{w,
                                      page_sources.value,
                                      context_sources.value,
                                      context_coverage.value,
                                      xt,
                                      frequency_sources.value,
                                      frequency_sources.value,
                                      persistent_freq_cpus.value,
                                      std::uint64_t{4},
                                      page_sources.value,
                                      context_scratch.value,
                                      temperature_scratch.value,
                                      frequency_view_scratch.value,
                                      frequency_sources.value,
                                      *whole,
                                      *prep,
                                      *completion,
                                      n,
                                      q,
                                      cpu_ops.value,
                                      q,
                                      *hwmon_ops_value,
                                      dynamic_raw.value,
                                      dynamic_escaped.value};
        if (std::ranges::any_of(stock_values, exceeds_component)) {
            return std::unexpected(CapacityError{CapacityErrorCode::size_unrepresentable,
                                                 CapacityField::monitoring_lower_stocks});
        }
        const std::array destination_values{std::uint64_t{1},
                                            std::uint64_t{4},
                                            std::uint64_t{1},
                                            std::uint64_t{1},
                                            b,
                                            b,
                                            std::uint64_t{1},
                                            finalized_workers.value,
                                            *raw_dest_value,
                                            *classified_dest_value,
                                            *error_dest_value,
                                            finalized_workers.value};
        if (std::ranges::any_of(destination_values, exceeds_component)) {
            return std::unexpected(CapacityError{CapacityErrorCode::size_unrepresentable,
                                                 CapacityField::evidence_destinations});
        }
        return CalculatedCapacityPrerequisite{in, components, stocks, destinations};
#undef EB_TRY_COUNT
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CapacityError{CapacityErrorCode::allocation_failure, CapacityField::input});
    } catch (const std::length_error&) {
        return std::unexpected(
            CapacityError{CapacityErrorCode::allocation_failure, CapacityField::input});
    }
}

auto validate_exact_capacity_prerequisite(const CalculatedCapacityPrerequisite& expected,
                                          const CalculatedCapacityPrerequisite& observed) noexcept
    -> std::expected<void, CapacityError> {
    const auto mismatch = [](CapacityField field) {
        return std::unexpected(CapacityError{CapacityErrorCode::mismatch, field});
    };
    if (expected.parameters != observed.parameters)
        return mismatch(CapacityField::input);
    if (expected.components != observed.components)
        return mismatch(CapacityField::component_maxima);
    if (expected.monitoring_lower_stocks != observed.monitoring_lower_stocks)
        return mismatch(CapacityField::monitoring_lower_stocks);
    if (expected.evidence_destinations != observed.evidence_destinations)
        return mismatch(CapacityField::evidence_destinations);
    return {};
}

namespace {

struct Shape final {
    std::uint64_t bytes{}, nodes{}, depth{};
};

enum class VariantSide : std::uint8_t { none, first, second };

struct ShapeContext final {
    const NormalizedCapacityParameterValue* parameters{};
    VariantSide side{VariantSide::none};
    std::uint64_t owner_total_pages{}, owner_target_count{}, domain_cpu_count{};
};

[[nodiscard]] auto shape_error() noexcept -> std::unexpected<CapacityError> {
    return std::unexpected(
        CapacityError{CapacityErrorCode::arithmetic_overflow, CapacityField::logical_shape});
}

[[nodiscard]] auto checked_add(std::uint64_t left, std::uint64_t right) noexcept
    -> std::expected<std::uint64_t, CapacityError> {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return shape_error();
    return left + right;
}

[[nodiscard]] auto checked_mul(std::uint64_t left, std::uint64_t right) noexcept
    -> std::expected<std::uint64_t, CapacityError> {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
        return shape_error();
    return left * right;
}

[[nodiscard]] auto escaped_reference(std::string_view name,
                                     const RequestKnownEscapedBytes& values) noexcept
    -> std::optional<std::uint64_t> {
#define EB_ESCAPED(field)                                                                          \
    if (name == "request_known_escaped_bytes." #field)                                             \
        return values.field.value
    EB_ESCAPED(scenario_profile);
    EB_ESCAPED(scenario_digest);
    EB_ESCAPED(resolved_plan_digest);
    EB_ESCAPED(workload_id);
    EB_ESCAPED(first_variant_id);
    EB_ESCAPED(second_variant_id);
    EB_ESCAPED(monitoring_implementation_ids);
    EB_ESCAPED(frequency_driver_ids);
    EB_ESCAPED(frequency_governor_ids);
    EB_ESCAPED(stable_numa_node_ids);
#undef EB_ESCAPED
    return std::nullopt;
}

[[nodiscard]] auto formula_value(std::uint32_t index, const ShapeContext& context) noexcept
    -> std::expected<std::uint64_t, CapacityError> {
    using detail::FormulaOp;
    const auto& formula = detail::formula_nodes[index];
    const auto* parameters = context.parameters;
    const auto variable = [&](std::string_view name) -> std::optional<std::uint64_t> {
        if (parameters == nullptr)
            return std::uint64_t{0};
        const auto& p = *parameters;
        if (name == "W") return p.workers.value;
        if (name == "B") return p.maximum_blocks.value;
        if (name == "T") return p.maximum_warmup_trials.value;
        if (name == "Bp") return p.base_page_bytes.value;
        if (name == "P") return p.present_cpu_ids.size();
        if (name == "K") return p.maximum_cpu_index.value;
        if (name == "N") return p.maximum_cpu_index.value + 1U;
        if (name == "Q") return p.maximum_cpu_index.value + 2U;
        if (name == "Nd") return p.numa_domain_maxnode.value;
        if (name == "C") return 4U * p.maximum_blocks.value;
        if (name == "D") return std::min<std::uint64_t>(p.workers.value, p.present_cpu_ids.size());
        if (name == "E") return (8U * p.base_page_bytes.value) / 16U;
        if (name == "R") return p.monitoring_rounds.value;
        if (name == "Xp") return p.channels.page;
        if (name == "Xc") return p.channels.context;
        if (name == "Xt") return p.channels.temperature;
        if (name == "Xf") return p.channels.frequency;
        if (name == "Xa") return p.channels.context || p.channels.temperature || p.channels.frequency;
        if (name == "domain_cpu_count") return context.domain_cpu_count;
        if (name == "owner_total_pages") return context.owner_total_pages;
        if (name == "owner_target_count") return context.owner_target_count;
        return std::nullopt;
    };
    if (formula.op == FormulaOp::constant)
        return formula.value;
    if (formula.op == FormulaOp::variable) {
        const auto value = variable(formula.name);
        return value ? std::expected<std::uint64_t, CapacityError>{*value} : shape_error();
    }
    if (formula.op == FormulaOp::reference) {
        if (parameters != nullptr) {
            if (auto escaped = escaped_reference(formula.name,
                                                 parameters->request_known_escaped_bytes))
                return *escaped;
            const auto sum_side = [&](bool first) {
                const auto& pages = first ? parameters->first_total_pages
                                          : parameters->second_total_pages;
                std::uint64_t result{};
                for (std::size_t i{}; i < pages.size(); ++i) {
                    const auto value = checked_add(
                        result, std::min(pages[i].value,
                                         parameters->numa_domain_maxnode.value));
                    if (!value) return std::expected<std::uint64_t, CapacityError>{std::unexpected(value.error())};
                    result = *value;
                }
                return std::expected<std::uint64_t, CapacityError>{result};
            };
            if (formula.name == "sum_i_min(first_total_pages[i],Nd)")
                return sum_side(true);
            if (formula.name == "sum_i_min(second_total_pages[i],Nd)")
                return sum_side(false);
        }
        return shape_error();
    }
    if (formula.op == FormulaOp::conditional) {
        const auto condition = variable(formula.name);
        if (!condition || formula.arg_count != 2U) return shape_error();
        return formula_value(detail::formula_args[formula.arg_begin + (*condition ? 0U : 1U)], context);
    }
    std::optional<std::uint64_t> accumulated;
    for (std::uint32_t offset{}; offset < formula.arg_count; ++offset) {
        auto value = formula_value(detail::formula_args[formula.arg_begin + offset], context);
        if (!value) return value;
        if (!accumulated) { accumulated = *value; continue; }
        if (formula.op == FormulaOp::add) {
            auto next = checked_add(*accumulated, *value); if (!next) return next; accumulated = *next;
        } else if (formula.op == FormulaOp::multiply) {
            auto next = checked_mul(*accumulated, *value); if (!next) return next; accumulated = *next;
        } else if (formula.op == FormulaOp::minimum) accumulated = std::min(*accumulated, *value);
        else if (formula.op == FormulaOp::maximum) accumulated = std::max(*accumulated, *value);
        else if (formula.op == FormulaOp::ceil_divide) {
            if (*value == 0U) return shape_error();
            accumulated = *accumulated / *value + (*accumulated % *value != 0U);
        } else return shape_error();
    }
    return accumulated.value_or(0U);
}

[[nodiscard]] auto evaluate_shape(std::uint32_t index, ShapeContext context,
                                  bool maximum) noexcept
    -> std::expected<Shape, CapacityError> {
    using detail::SchemaKind;
    const auto& node = detail::schema_nodes[index];
    if (node.kind == SchemaKind::scalar) {
        if (!maximum) return Shape{node.minimum_bytes, 1U, 1U};
        auto bytes = formula_value(node.maximum_formula, context);
        if (!bytes) return std::unexpected(bytes.error());
        return Shape{*bytes, 1U, 1U};
    }
    if (node.kind == SchemaKind::option) {
        if (!maximum) return Shape{4U, 1U, 1U};
        auto child = evaluate_shape(node.child, context, true);
        if (!child) return child;
        return Shape{std::max<std::uint64_t>(4U, child->bytes),
                     std::max<std::uint64_t>(1U, child->nodes),
                     std::max<std::uint64_t>(1U, child->depth)};
    }
    if (node.kind == SchemaKind::array) {
        if (!maximum) return Shape{2U, 1U, 1U};
        auto count = formula_value(node.cardinality_formula, context);
        if (!count) return std::unexpected(count.error());
        Shape result{2U, 1U, 1U};
        if (*count != 0U) {
            auto commas = checked_add(result.bytes, *count - 1U);
            if (!commas) return std::unexpected(commas.error());
            result.bytes = *commas;
        }
        for (std::uint64_t i{}; i < *count; ++i) {
            auto child_context = context;
            if (node.special == detail::SchemaSpecial::finalized_workers) {
                const bool first = context.side == VariantSide::first;
                const auto& pages = first ? context.parameters->first_total_pages : context.parameters->second_total_pages;
                const auto& targets = first ? context.parameters->first_target_counts : context.parameters->second_target_counts;
                child_context.owner_total_pages = pages[static_cast<std::size_t>(i)].value;
                child_context.owner_target_count = targets[static_cast<std::size_t>(i)].value;
            } else if (node.special == detail::SchemaSpecial::frequency_domains) {
                child_context.domain_cpu_count = context.parameters->domain_cpu_ids[static_cast<std::size_t>(i)].size();
            }
            auto child = evaluate_shape(node.child, child_context, true);
            if (!child) return child;
            auto bytes = checked_add(result.bytes, child->bytes); if (!bytes) return std::unexpected(bytes.error()); result.bytes = *bytes;
            auto nodes = checked_add(result.nodes, child->nodes); if (!nodes) return std::unexpected(nodes.error()); result.nodes = *nodes;
            result.depth = std::max(result.depth, child->depth + 1U);
        }
        return result;
    }
    Shape result{node.kind == SchemaKind::object ? 2U : 0U,
                 node.kind == SchemaKind::object ? 1U : 0U, 1U};
    bool first_child = true;
    for (std::uint32_t offset{}; offset < node.edge_count; ++offset) {
        const auto& edge = detail::schema_edges[node.edge_begin + offset];
        auto child_context = context;
        if (node.special == detail::SchemaSpecial::finalization)
            child_context.side = offset == 0U ? VariantSide::first : VariantSide::second;
        auto child = evaluate_shape(edge.child, child_context, maximum);
        if (!child) return child;
        if (node.kind == SchemaKind::sum) {
            if (first_child) result = *child;
            else {
                result = Shape{
                    maximum ? std::max(result.bytes, child->bytes)
                            : std::min(result.bytes, child->bytes),
                    maximum ? std::max(result.nodes, child->nodes)
                            : std::min(result.nodes, child->nodes),
                    maximum ? std::max(result.depth, child->depth)
                            : std::min(result.depth, child->depth)};
            }
        } else {
            auto key = checked_add(edge.key_bytes, 3U); if (!key) return std::unexpected(key.error());
            auto bytes = checked_add(result.bytes, *key); if (!bytes) return std::unexpected(bytes.error()); result.bytes = *bytes;
            bytes = checked_add(result.bytes, child->bytes); if (!bytes) return std::unexpected(bytes.error()); result.bytes = *bytes;
            auto nodes = checked_add(result.nodes, child->nodes); if (!nodes) return std::unexpected(nodes.error()); result.nodes = *nodes;
            result.depth = std::max(result.depth, child->depth + 1U);
        }
        first_child = false;
    }
    if (node.kind == SchemaKind::object && node.edge_count > 0U) {
        auto commas = checked_add(result.bytes, node.edge_count - 1U); if (!commas) return std::unexpected(commas.error()); result.bytes = *commas;
    }
    return result;
}

[[nodiscard]] auto default_resource_probe(std::size_t bytes, std::size_t nodes) noexcept -> bool {
    try {
        std::vector<std::byte> byte_storage;
        std::vector<std::uint64_t> node_storage;
        byte_storage.reserve(bytes);
        node_storage.reserve(nodes);
        return byte_storage.capacity() >= bytes && node_storage.capacity() >= nodes;
    } catch (...) { return false; }
}

} // namespace

auto detail::evidence_component_node(EvidenceComponentKind kind) noexcept
    -> std::optional<std::uint32_t> {
    // Select by the generated root's actual named edges; never freeze numeric
    // registry indices or expose an arbitrary caller-selected node.
    constexpr auto child = [](std::uint32_t parent, std::string_view key) {
        const auto& node = detail::schema_nodes[parent];
        for (std::uint32_t i{}; i < node.edge_count; ++i) {
            const auto& edge = detail::schema_edges[node.edge_begin + i];
            if (edge.key == key) return edge.child;
        }
        return UINT32_MAX;
    };
    constexpr auto calibration = child(detail::evidence_root, "calibration");
    constexpr auto warmup = child(detail::evidence_root, "warmup");
    static_assert(calibration != UINT32_MAX && warmup != UINT32_MAX);
    constexpr auto finalization = child(detail::evidence_root, "finalization");
    static_assert(finalization != UINT32_MAX);
    constexpr auto stopping = child(detail::evidence_root, "stopping_evaluations");
    static_assert(stopping != UINT32_MAX);
    constexpr auto blocks = child(detail::evidence_root, "blocks");
    static_assert(blocks != UINT32_MAX);
    constexpr auto block = detail::schema_nodes[blocks].child;
    constexpr auto plan = child(block, "plan");
    constexpr auto facts = child(block, "observations");
    constexpr auto failed = child(block, "failed_observation");
    static_assert(plan != UINT32_MAX && facts != UINT32_MAX && failed != UINT32_MAX);
    constexpr auto terminal = child(detail::evidence_root, "terminal");
    constexpr auto monitoring = child(detail::schema_nodes[facts].child, "monitoring");
    static_assert(terminal != UINT32_MAX && monitoring != UINT32_MAX);
    constexpr auto operational = [] {
        const auto& node=detail::schema_nodes[terminal];
        for(std::uint32_t i{};i<node.edge_count;++i){const auto& edge=detail::schema_edges[node.edge_begin+i];
            if(edge.token=="operational")return edge.child;}
        return UINT32_MAX;
    }();
    static_assert(operational != UINT32_MAX);
    constexpr auto finalized_error_node = child(operational,"primary_error");
    static_assert(finalized_error_node != UINT32_MAX);
    std::uint32_t root{};
    switch (kind) {
    case EvidenceComponentKind::calibration:
        root = detail::schema_nodes[calibration].child; break;
    case EvidenceComponentKind::warmup:
        root = detail::schema_nodes[warmup].child; break;
    case EvidenceComponentKind::finalization:
        root = detail::schema_nodes[finalization].child; break;
    case EvidenceComponentKind::stopping_evaluation:
        root = detail::schema_nodes[stopping].child; break;
    case EvidenceComponentKind::block_plan: root = plan; break;
    case EvidenceComponentKind::factual_observation:
        root = detail::schema_nodes[facts].child; break;
    case EvidenceComponentKind::failed_observation:
        root = detail::schema_nodes[failed].child; break;
    case EvidenceComponentKind::monitoring_result:
        root = detail::schema_nodes[monitoring].child; break;
    case EvidenceComponentKind::terminal: root = terminal; break;
    case EvidenceComponentKind::finalized_error: root = finalized_error_node; break;
    default: return std::nullopt;
    }
    return root;
}

auto maximum_component_shape(EvidenceComponentKind kind, EvidenceComponentSide side,
    const CalculatedEvidenceCapacity& capacity) noexcept -> std::expected<LogicalShape, CapacityError> {
    const auto selected = detail::evidence_component_node(kind);
    if (!selected || (side != EvidenceComponentSide::first && side != EvidenceComponentSide::second))
        return invalid();
    const auto& p = capacity.prerequisite.parameters;
    const auto workers = p.workers.value;
    const auto vectors = [&](const auto& pages, const auto& targets) noexcept {
        if (pages.size() != workers || targets.size() != workers) return false;
        for (std::size_t i = 0; i < pages.size(); ++i)
            if (pages[i].value == 0 || targets[i].value == 0 || targets[i].value > p.numa_domain_maxnode.value)
                return false;
        return true;
    };
    // The caller supplies its calculated startup capacity. Still reject malformed
    // aggregate dimensions before any generated per-owner array indexing.
    if (workers == 0 || p.maximum_blocks.value == 0 || p.base_page_bytes.value == 0 ||
        p.numa_domain_maxnode.value == 0 || p.present_cpu_ids.empty() ||
        p.domain_cpu_ids.size() != std::min<std::uint64_t>(workers, p.present_cpu_ids.size()) ||
        !vectors(p.first_total_pages, p.first_target_counts) ||
        !vectors(p.second_total_pages, p.second_target_counts)) return invalid();
    for (const auto& domain : p.domain_cpu_ids) if (domain.empty()) return invalid(CapacityField::cpu_partition);
    if (p.maximum_blocks.value > UINT64_MAX / 4U || p.base_page_bytes.value > UINT64_MAX / 8U ||
        p.maximum_cpu_index.value > UINT64_MAX - 2U)
        return shape_error();
    const auto shape = evaluate_shape(*selected,
        {&p, side == EvidenceComponentSide::first ? VariantSide::first : VariantSide::second}, true);
    if (!shape) return std::unexpected{shape.error()};
    return LogicalShape{{shape->bytes}, {shape->nodes}, {shape->depth}};
}

auto minimum_logical_shape() noexcept -> std::expected<LogicalShape, CapacityError> {
    auto shape = evaluate_shape(detail::evidence_root, {}, false);
    if (!shape) return std::unexpected(shape.error());
    return LogicalShape{{shape->bytes}, {shape->nodes}, {shape->depth}};
}

auto calculate_evidence_capacity(const NormalizedCapacityParameters& parameters,
                                 LocalResourceLimits limits) noexcept
    -> std::expected<CalculatedEvidenceCapacity, CapacityError> {
    auto prerequisite = calculate_capacity_prerequisite(parameters, {limits.size_max});
    if (!prerequisite) return std::unexpected(prerequisite.error());
    const auto& values = parameters.value();
    const auto target_exceeds = [&](const auto& targets) {
        return std::ranges::any_of(targets,
                                   [&](TargetCount target) { return target.value > limits.size_max; });
    };
    if (values.maximum_warmup_trials.value > limits.size_max ||
        target_exceeds(values.first_target_counts) || target_exceeds(values.second_target_counts))
        return std::unexpected(CapacityError{CapacityErrorCode::size_unrepresentable,
                                             CapacityField::logical_shape});
    auto shape = evaluate_shape(detail::evidence_root, {&values}, true);
    if (!shape) return std::unexpected(shape.error());
    auto bytes_with_probe = checked_add(shape->bytes, 1U);
    if (!bytes_with_probe) return std::unexpected(bytes_with_probe.error());
    if (shape->bytes > limits.size_max || shape->nodes > limits.size_max ||
        shape->depth > limits.parser_depth_max || *bytes_with_probe > limits.byte_container_max ||
        shape->nodes > limits.node_container_max)
        return std::unexpected(CapacityError{CapacityErrorCode::size_unrepresentable,
                                             CapacityField::resource_plan});
    const auto probe = limits.probe == nullptr ? default_resource_probe : limits.probe;
    if (!probe(static_cast<std::size_t>(*bytes_with_probe), static_cast<std::size_t>(shape->nodes)))
        return std::unexpected(CapacityError{CapacityErrorCode::allocation_failure,
                                             CapacityField::resource_probe});
    const LogicalShape logical{{shape->bytes}, {shape->nodes}, {shape->depth}};
    const ResourcePlan plan{{shape->bytes}, {1U}, {shape->nodes}, prerequisite->evidence_destinations,
                            {shape->bytes}, {1U}, {shape->nodes}, prerequisite->evidence_destinations};
    return CalculatedEvidenceCapacity{std::move(*prerequisite), logical, plan};
}

auto validate_exact_evidence_capacity(const CalculatedEvidenceCapacity& expected,
                                      const CalculatedEvidenceCapacity& observed) noexcept
    -> std::expected<void, CapacityError> {
    if (auto result = validate_exact_capacity_prerequisite(expected.prerequisite,
                                                           observed.prerequisite); !result)
        return result;
    if (expected.logical_shape != observed.logical_shape)
        return std::unexpected(CapacityError{CapacityErrorCode::mismatch,
                                             CapacityField::logical_shape});
    if (expected.resource_plan != observed.resource_plan)
        return std::unexpected(CapacityError{CapacityErrorCode::mismatch,
                                             CapacityField::resource_plan});
    return {};
}

} // namespace epycbench::runtime::wire_v1
