#include "scenario/retained_plan_projection_resources.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <set>

namespace epycbench::scenario::detail {
namespace projection_resource_detail {
auto checked_add(std::size_t& a, std::size_t b) noexcept -> bool {
    if (b > SIZE_MAX - a) return false;
    a += b;
    return true;
}
auto checked_multiply(std::size_t a, std::size_t b) noexcept -> std::optional<std::size_t> {
    if (a && b > SIZE_MAX / a) return {};
    return a * b;
}
auto ordered_member_reservation(std::size_t width, std::size_t maximum) noexcept
    -> std::optional<std::size_t> {
    if (width > maximum) return {};
    return width > maximum / 2 ? maximum : width * 2;
}
}
namespace {
using Q = ProjectionResourceUsage;
using Failure = ProjectionResourceFailure;
using Json = nlohmann::ordered_json;
constexpr std::array quantities{
    &Q::nodes, &Q::objects, &Q::arrays, &Q::object_members, &Q::string_values, &Q::key_bytes,
    &Q::string_bytes, &Q::depth, &Q::canonical_bytes, &Q::utf32_reserved_elements,
    &Q::utf16_elements, &Q::canonical_member_elements, &Q::quoted_key_bytes, &Q::path_bytes,
    &Q::index_decimal_bytes, &Q::ordered_staging_bytes, &Q::unordered_clone_bytes,
    &Q::projector_auxiliary_bytes, &Q::fixed_bytes, &Q::error_bytes, &Q::simultaneous_bytes,
    &Q::work_units};
void need(bool condition, Failure failure = Failure::capacity) {
    if (!condition) throw failure;
}
auto plus(std::size_t a, std::size_t b) -> std::size_t {
    need(projection_resource_detail::checked_add(a, b)); return a;
}
auto times(std::size_t a, std::size_t b) -> std::size_t {
    const auto result = projection_resource_detail::checked_multiply(a, b);
    need(result.has_value()); return *result;
}
struct Decimal {
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 3> buffer{};
    std::size_t size{};
    template<class T> explicit Decimal(T value) {
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        need(result.ec == std::errc{});
        size = static_cast<std::size_t>(result.ptr - buffer.data());
    }
    auto view() const -> std::string_view { return {buffer.data(), size}; }
};
struct TextStock { std::size_t quoted{2}, units{}; };
// Read UTF-8 only; no decoded string, canonical bytes or JSON is produced.
auto text_stock(std::string_view text) -> TextStock {
    TextStock out;
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::size_t width{}; std::uint32_t scalar{}, minimum{};
        if (first < 0x80U) { width = 1; scalar = first; }
        else if (first >= 0xc2U && first <= 0xdfU) { width = 2; scalar = first & 31U; minimum = 0x80U; }
        else if (first >= 0xe0U && first <= 0xefU) { width = 3; scalar = first & 15U; minimum = 0x800U; }
        else if (first >= 0xf0U && first <= 0xf4U) { width = 4; scalar = first & 7U; minimum = 0x10000U; }
        else throw Failure::malformed_input;
        need(width <= text.size() - i, Failure::malformed_input);
        for (std::size_t j = 1; j < width; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            need((next & 0xc0U) == 0x80U, Failure::malformed_input);
            scalar = (scalar << 6U) | (next & 63U);
        }
        need(scalar >= minimum && scalar <= 0x10ffffU &&
             (scalar < 0xd800U || scalar > 0xdfffU), Failure::malformed_input);
        out.units = plus(out.units, scalar > 0xffffU ? 2 : 1);
        const auto escaped = scalar == '"' || scalar == '\\' || scalar == '\b' ||
            scalar == '\t' || scalar == '\n' || scalar == '\f' || scalar == '\r';
        out.quoted = plus(out.quoted, escaped ? 2 : scalar < 0x20U ? 6 : width);
        i += width;
    }
    return out;
}
struct Walker {
    const ProjectionResourceLimits& limits;
    Q q{};
    std::size_t level{}, path{1}, ordered_payload{}, unordered_payload{}, member_reservations{};
    std::size_t maximum_text{}, auxiliary{};
    std::size_t discarded_payload{}, discarded_depth{}, discarded_nodes{}, staging_work_limit{};
    bool staging{};
    void count(std::size_t Q::*field, std::size_t amount = 1) {
        q.*field = plus(q.*field, amount);
        // Discarded semantic input is temporary projector storage, not part of
        // the final output shape. Its work and owned payload remain bounded.
        if (!staging) need(q.*field <= limits.*field);
        else if (field == &Q::work_units) need(q.*field <= staging_work_limit);
    }
    void work(std::size_t amount = 1) { count(&Q::work_units, amount); }
    void node() {
        count(&Q::nodes); count(&Q::path_bytes, path); work();
        q.depth = std::max(q.depth, level); need((staging || q.depth <= limits.depth) && level <= 64);
    }
    void payload(std::size_t ordered, std::size_t unordered) {
        ordered_payload = plus(ordered_payload, ordered);
        unordered_payload = plus(unordered_payload, unordered);
        if (staging) need(ordered_payload <= limits.ordered_staging_bytes);
    }
    auto text(std::string_view value, bool key) -> TextStock {
        count(key ? &Q::key_bytes : &Q::string_bytes, value.size());
        count(&Q::utf32_reserved_elements, value.size());
        work(value.size());
        const auto stock = text_stock(value);
        count(&Q::utf16_elements, stock.units);
        maximum_text = std::max(maximum_text, value.size());
        payload(value.size(), value.size());
        return stock;
    }
    void string(std::string_view value) {
        node(); count(&Q::string_values);
        payload(sizeof(Json::string_t), sizeof(nlohmann::json::string_t));
        count(&Q::canonical_bytes, text(value, false).quoted);
    }
    void decimal(std::uint64_t value) { const Decimal digits{value}; string(digits.view()); }
    void integer(std::uint64_t value) { node(); count(&Q::canonical_bytes, Decimal{value}.size); }
    void signed_integer(std::int64_t value) { node(); count(&Q::canonical_bytes, Decimal{value}.size); }
    void boolean(bool value) { node(); count(&Q::canonical_bytes, value ? 4 : 5); }
    void null() { node(); count(&Q::canonical_bytes, 4); }
    template<class F> void object(std::size_t width, F body) {
        node(); count(&Q::objects); count(&Q::object_members, width);
        count(&Q::canonical_member_elements, width);
        const auto reserve = projection_resource_detail::ordered_member_reservation(
            width, Json::object_t::Container{}.max_size());
        need(reserve.has_value()); need(width <= nlohmann::json::object_t{}.max_size());
        member_reservations = plus(member_reservations, *reserve);
        payload(plus(sizeof(Json::object_t), times(width, sizeof(Json::object_t::value_type))),
                plus(sizeof(nlohmann::json::object_t), times(width, sizeof(nlohmann::json::object_t::value_type))));
        count(&Q::canonical_bytes, plus(2, width ? width - 1 : 0));
        body();
    }
    template<class F> void member(std::string_view key, F body) {
        const auto stock = text(key, true);
        count(&Q::quoted_key_bytes, stock.quoted);
        count(&Q::canonical_bytes, plus(stock.quoted, 1));
        const auto prior = path; path = plus(path, plus(stock.quoted, 2)); ++level;
        body(); --level; path = prior;
    }
    template<class F> void array(std::size_t size, F element) {
        node(); count(&Q::arrays); need(size <= Json::array_t{}.max_size());
        need(size <= nlohmann::json::array_t{}.max_size());
        payload(plus(sizeof(Json::array_t), times(size, sizeof(Json))),
                plus(sizeof(nlohmann::json::array_t), times(size, sizeof(nlohmann::json))));
        count(&Q::canonical_bytes, plus(2, size ? size - 1 : 0));
        for (std::size_t i = 0; i < size; ++i) {
            const auto digits = Decimal{i}.size; count(&Q::index_decimal_bytes, digits);
            const auto prior = path; path = plus(path, plus(digits, 2)); ++level;
            element(i); --level; path = prior;
        }
    }
    void string_member(std::string_view key, std::string_view value) { member(key, [&] { string(value); }); }
    void decimal_member(std::string_view key, std::uint64_t value) { member(key, [&] { decimal(value); }); }
    void integer_member(std::string_view key, std::uint64_t value) { member(key, [&] { integer(value); }); }
    void bool_member(std::string_view key, bool value) { member(key, [&] { boolean(value); }); }
    void semantic(const Json& value, bool matrix_default = false) {
        if (value.is_object()) {
            const auto unit = value.find("unit");
            const auto tagged = unit != value.end() && unit->is_string() &&
                (unit->get_ref<const std::string&>() == "ratio" || unit->get_ref<const std::string&>() == "nanosecond");
            object(plus(value.size(), matrix_default ? 1 : 0), [&] {
                for (auto it = value.begin(); it != value.end(); ++it)
                    member(it.key(), [&] {
                        if (tagged && it.key() != "unit") decimal(it->get<std::uint64_t>());
                        else semantic(*it);
                    });
                if (matrix_default) member("memory", [&] { default_memory(); });
            });
        } else if (value.is_array()) array(value.size(), [&](std::size_t i) { semantic(value[i]); });
        else if (value.is_string()) string(value.get_ref<const std::string&>());
        else if (value.is_number_unsigned()) integer(value.get<std::uint64_t>());
        else if (value.is_number_integer()) signed_integer(value.get<std::int64_t>());
        else if (value.is_boolean()) boolean(value.get<bool>());
        else { need(value.is_null(), Failure::malformed_input); null(); }
    }
    void ratio(const ExactRatio& value) {
        object(2, [&] { decimal_member("numerator", value.numerator()); decimal_member("denominator", value.denominator()); });
    }
    void default_memory() {
        object(6, [&] {
            string_member("policy", "bind"); string_member("page_mode", "base");
            member("placement_threshold", [&] { object(3, [&] {
                string_member("unit", "ratio"); string_member("numerator", "999"); string_member("denominator", "1000");
            }); });
            bool_member("prefault", true); bool_member("verify_before", true); bool_member("verify_after", true);
        });
    }
    template<class Worker> void worker(const Worker& value) {
        object(3, [&] { string_member("stable_pu", value.stable_id.value()); string_member("stable_core", value.core_id.value()); integer_member("linux_cpu", value.linux_cpu.value()); });
    }
    void memory_node(const ResolvedMemoryNode& value) {
        object(2, [&] { string_member("stable_id", value.stable_id.value()); integer_member("linux_node", value.linux_node.value()); });
    }
    template<class Placement> void placement(const Placement& value) {
        object(8, [&] {
            string_member("policy", "bind");
            if constexpr (requires { value.nodes; })
                member("nodes", [&] { array(value.nodes.size(), [&](std::size_t i) { memory_node(value.nodes[i]); }); });
            else member("per_worker_nodes", [&] { array(value.worker_nodes.size(), [&](std::size_t i) {
                object(2, [&] { member("worker", [&] { worker(value.worker_nodes[i].worker); }); member("node", [&] { memory_node(value.worker_nodes[i].node); }); });
            }); });
            string_member("page_mode", "base"); decimal_member("page_size_bytes", value.page_size.value());
            member("placement_threshold", [&] { ratio(value.placement_threshold); });
            bool_member("prefault", value.prefault); bool_member("verify_before", value.verify_before); bool_member("verify_after", value.verify_after);
        });
    }
    void placement(const ComparisonMemoryPlacement& value) { std::visit([&](const auto& p) { placement(p); }, value); }
    void workload(const DispatchedWorkload& value) {
        object(2, [&] {
            string_member("kernel", kernel::kernel_id_token(value.kernel_id));
            member("parameters", [&] {
                const auto* bandwidth = std::get_if<kernel::SequentialReadBandwidthConfig>(&value.configuration);
                object(bandwidth ? 5 : 7, [&] {
                    decimal_member("working_set_bytes", bandwidth ? bandwidth->working_set_per_worker().value() : std::get<kernel::DependentLoadLatencyConfig>(value.configuration).working_set().value());
                    string_member("partition", "private-per-worker"); string_member("alignment_bytes", "64");
                    if (bandwidth) { string_member("access_pattern", "sequential"); string_member("element_size_bytes", "8"); }
                    else {
                        integer_member("chains_per_worker", 1); string_member("construction", "randomized-cycle"); string_member("node_size_bytes", "64");
                        member("chain_seed", [&] { object(2, [&] {
                            const auto& intent = std::get<kernel::DependentLoadLatencyConfig>(value.configuration).pointer_cycle_seed();
                            const auto explicit_seed = intent.mode() == kernel::PointerCycleSeedMode::explicit_value;
                            string_member("mode", explicit_seed ? "explicit" : "derived");
                            if (explicit_seed) decimal_member("value", *intent.value());
                            else string_member("profile", kernel::pointer_cycle_seed_profile);
                        }); });
                    }
                });
            });
        });
    }
    void seed(const ExperimentWorkloadBinding& value) {
        if (!value.pointer_cycle_seed) { null(); return; }
        object(2, [&] { decimal_member("value", *value.pointer_cycle_seed); string_member("origin",
            *value.pointer_cycle_seed_origin == PointerCycleSeedOrigin::explicit_value ? "explicit" : "derived"); });
    }
    static auto order(MatrixComparisonOrder value) -> std::string_view {
        switch (value) {
        case MatrixComparisonOrder::randomized_abba: return "randomized-abba";
        case MatrixComparisonOrder::randomized_baab: return "randomized-baab";
        case MatrixComparisonOrder::randomized_balanced: return "randomized-balanced";
        }
        throw Failure::malformed_input;
    }
    static auto origin(ValueOriginKind value) -> std::string_view {
        switch (value) {
        case ValueOriginKind::explicit_value: return "explicit-value";
        case ValueOriginKind::schema_default: return "schema-default";
        case ValueOriginKind::scenario_default: return "scenario-default";
        case ValueOriginKind::experiment_override: return "experiment-override";
        case ValueOriginKind::variant_override: return "variant-override";
        case ValueOriginKind::matrix_override: return "matrix-override";
        }
        throw Failure::malformed_input;
    }
    void cell_id(const MatrixCellId& value) {
        object(3, [&] {
            string_member("experiment", value.experiment_id);
            string_member("worker_node", value.worker_node.value());
            string_member("memory_node", value.memory_node.value());
        });
    }
    void discarded_semantic(const Json& value) {
        Walker discarded_walk{limits};
        discarded_walk.staging = true;
        discarded_walk.staging_work_limit = limits.work_units - q.work_units;
        discarded_walk.level = level; discarded_walk.path = path;
        discarded_walk.semantic(value);
        const auto reserved = times(discarded_walk.member_reservations - discarded_walk.q.object_members,
                                    sizeof(Json::object_t::value_type));
        discarded_payload = plus(discarded_payload, plus(discarded_walk.ordered_payload, reserved));
        need(discarded_payload <= limits.ordered_staging_bytes);
        discarded_depth = std::max(discarded_depth, discarded_walk.q.depth);
        discarded_nodes = plus(discarded_nodes, discarded_walk.q.nodes);
        work(discarded_walk.q.work_units);
    }
    void practical_effect_origin(const ValueOrigin& value) {
        object(2, [&] {
            string_member("kind", origin(value.kind));
            // Preserve the accepted initializer-list Json{value} shape.
            member("source_pointer", [&] { array(1, [&](std::size_t) {
                if (value.source_pointer) string(*value.source_pointer); else null();
            }); });
        });
    }
    void comparison(const ResolvedComparisonExperiment& value, const Json& intent,
                    const ExperimentWorkloadBinding& binding) {
        object(8, [&] {
            integer_member("experiment_index", value.experiment_index);
            string_member("experiment_id", value.experiment_id); string_member("type", "comparison");
            string_member("workload_id", binding.workload_id);
            member("pointer_cycle_seed", [&] { seed(binding); });
            member("workers", [&] { array(value.workers.size(), [&](std::size_t i) { worker(value.workers[i]); }); });
            member("variants", [&] { array(value.variants.size(), [&](std::size_t i) {
                const auto& variant = value.variants[i];
                object(3, [&] {
                    integer_member("variant_index", variant.variant_index); string_member("variant_id", variant.variant_id);
                    member("memory", [&] { placement(variant.memory_placement); });
                });
            }); });
            member("comparison", [&] {
                const auto& compared = intent.at("compare");
                const auto existing_origin = compared.find("practical_effect_origin");
                object(plus(compared.size(), existing_origin == compared.end() ? 1 : 0), [&] {
                    for (auto it = compared.begin(); it != compared.end(); ++it)
                        member(it.key(), [&] {
                            if (it == existing_origin) {
                                discarded_semantic(*it);
                                practical_effect_origin(value.practical_effect_origin);
                            } else semantic(*it);
                        });
                    if (existing_origin == compared.end())
                        member("practical_effect_origin", [&] { practical_effect_origin(value.practical_effect_origin); });
                });
            });
        });
    }
    void matrix(const ExpandedMatrixExperiment& value, const ExperimentWorkloadBinding& binding) {
        object(11, [&] {
            integer_member("experiment_index", value.experiment_index); string_member("experiment_id", value.experiment_id);
            string_member("type", "node-pair-matrix"); string_member("workload_id", binding.workload_id);
            member("pointer_cycle_seed", [&] { seed(binding); });
            member("worker_axis", [&] { array(value.worker_axis.size(), [&](std::size_t i) { memory_node(value.worker_axis[i]); }); });
            member("memory_axis", [&] { array(value.memory_axis.size(), [&](std::size_t i) { memory_node(value.memory_axis[i]); }); });
            member("worker_rows", [&] { array(value.worker_rows.size(), [&](std::size_t i) {
                const auto& row = value.worker_rows[i]; object(3, [&] {
                    string_member("worker_node", row.stable_node_id.value()); integer_member("linux_node", row.linux_node.value());
                    member("workers", [&] { array(row.workers.size(), [&](std::size_t j) { worker(row.workers[j]); }); });
                });
            }); });
            member("cells", [&] { array(value.cells.size(), [&](std::size_t i) {
                const auto& cell = value.cells[i]; object(5, [&] {
                    member("id", [&] { cell_id(cell.id); }); string_member("worker_row", cell.id.worker_node.value());
                    member("memory_node", [&] { object(2, [&] {
                        string_member("stable_id", cell.memory_node_stable_id.value()); integer_member("linux_node", cell.memory_node.value());
                    }); });
                    member("memory", [&] { placement(cell.memory_placement); });
                    member("distance", [&] { object(3, [&] {
                        string_member("meaning", "latency"); string_member("unit", "relative"); integer_member("value", cell.distance.value);
                    }); });
                });
            }); });
            member("comparison", [&] { object(4, [&] {
                member("practical_effect", [&] { ratio(value.comparison.practical_effect); });
                member("confidence", [&] { ratio(value.comparison.confidence); });
                string_member("order", order(value.comparison.order));
                member("order_seed", [&] { array(1, [&](std::size_t) {
                    if (value.comparison.order_seed) decimal(*value.comparison.order_seed); else null();
                }); });
            }); });
            member("comparison_edges", [&] { array(value.comparison_edges.size(), [&](std::size_t i) {
                const auto& edge = value.comparison_edges[i]; object(2, [&] {
                    member("baseline", [&] { cell_id(value.cells[edge.baseline_cell_index].id); });
                    member("candidate", [&] { cell_id(value.cells[edge.candidate_cell_index].id); });
                });
            }); });
        });
    }
    void service(const RetainedPlanProjectionView& view) {
        object(4, [&] {
            bool_member("dedicated_service_placement", view.dedicated_service_placement);
            member("pools", [&] { object(view.service_cpu_pools.size(), [&] {
                for (const auto& [name, pool] : view.service_cpu_pools) member(name, [&] {
                    object(1, [&] { member("workers", [&] {
                        array(pool.processing_units.size(), [&](std::size_t i) { worker(pool.processing_units[i]); });
                    }); });
                });
            }); });
            member("reserved_linux_cpus", [&] {
                auto it = view.reserved_service_cpus.begin();
                array(static_cast<std::size_t>(view.reserved_service_cpus.cardinality()), [&](std::size_t) { integer((*it).value()); ++it; });
            });
            member("reserved_stable_cores", [&] {
                // Child local admission establishes the unique retained set. Sorting
                // changes indexes but not string/canonical/path aggregate lengths.
                array(view.reserved_service_cores.size(), [&](std::size_t i) { string(view.reserved_service_cores[i].value()); });
            });
        });
    }
    void workloads(const RetainedPlanProjectionView& view) {
        object(view.workloads.size(), [&] { for (const auto& [name, value] : view.workloads)
            member(name, [&] { workload(value); }); });
    }
    void scenario(const RetainedPlanProjectionView& view) {
        const auto& source = view.effective;
        object(11, [&] {
            string_member("profile", scenario_digest_profile); string_member("schema", source.at("schema").get_ref<const std::string&>());
            string_member("scenario_id", source.at("metadata").at("id").get_ref<const std::string&>());
            decimal_member("random_seed", source.at("random").at("seed").get<std::uint64_t>());
            const auto resources = source.find("resources");
            const auto has_workers = resources != source.end() && resources->contains("worker_sets");
            const auto has_services = resources != source.end() && resources->contains("service_cpus");
            member("resources", [&] { object(has_workers ? 1 : 0, [&] {
                if (has_workers) member("worker_sets", [&] { semantic(resources->at("worker_sets")); });
            }); });
            member("service", [&] { object(has_services ? 1 : 0, [&] {
                if (has_services) member("service_cpus", [&] { semantic(resources->at("service_cpus")); });
            }); });
            member("workloads", [&] { workloads(view); });
            member("experiments", [&] { const auto& experiments = source.at("experiments");
                array(experiments.size(), [&](std::size_t i) {
                    const auto& experiment = experiments[i]; semantic(experiment,
                        experiment.at("type").get_ref<const std::string&>() == "node-pair-matrix" && !experiment.contains("memory"));
                });
            });
            for (const auto key : {"measurement", "monitoring", "requirements"}) member(key, [&] {
                const auto found = source.find(key); if (found == source.end()) object(0, [] {}); else semantic(*found);
            });
        });
    }
    void resolved(const RetainedPlanProjectionView& view) {
        object(7, [&] {
            string_member("profile", resolved_scenario_plan_digest_profile);
            // Every canonical typed digest/fingerprint is 71 unescaped ASCII bytes.
            constexpr std::string_view digest_shape = "sha256:0000000000000000000000000000000000000000000000000000000000000000";
            string_member("scenario_digest", digest_shape);
            member("topology", [&] { object(2, [&] {
                string_member("physical_fingerprint", digest_shape); string_member("effective_fingerprint", digest_shape);
            }); });
            member("profiles", [&] { object(6, [&] {
                string_member("resource_resolution", resource_resolution_profile); string_member("service_cpu_resolution", service_cpu_resolution_profile);
                string_member("memory_placement_resolution", memory_placement_resolution_profile); string_member("kernel_dispatch", kernel_dispatch_profile);
                string_member("matrix_expansion", matrix_expansion_profile); string_member("matrix_cell_identity", matrix_cell_profile);
            }); });
            member("workloads", [&] { workloads(view); }); member("service", [&] { service(view); });
            member("experiments", [&] {
                const auto& intents = view.effective.at("experiments"); array(intents.size(), [&](std::size_t i) {
                    const auto binding = std::ranges::find(view.experiment_workloads, i, &ExperimentWorkloadBinding::experiment_index);
                    need(binding != view.experiment_workloads.end(), Failure::malformed_input);
                    work(view.experiment_workloads.size());
                    if (intents[i].at("type").get_ref<const std::string&>() == "comparison") {
                        const auto found = std::ranges::find(view.comparison_experiments, i, &ResolvedComparisonExperiment::experiment_index);
                        need(found != view.comparison_experiments.end(), Failure::malformed_input); work(view.comparison_experiments.size());
                        comparison(*found, intents[i], *binding);
                    } else {
                        const auto found = std::ranges::find(view.matrix_experiments, i, &ExpandedMatrixExperiment::experiment_index);
                        need(found != view.matrix_experiments.end(), Failure::malformed_input); work(view.matrix_experiments.size());
                        matrix(*found, *binding);
                    }
                });
            });
        });
    }
};
// Logical payload roles in the accepted projector and JSON helper graph. Each
// role is bounded by the complete final tree. Recursive roles are charged at
// every admitted depth, separately from this nonrecursive union.
struct OrderedPayloadSlots {
    Json ratio_result, ratio_object, ratio_value;
    Json worker_result, worker_object, worker_value;
    Json service_worker_result, service_worker_object, service_worker_value;
    Json node_result, node_object, node_value;
    Json placement_nodes, placement_mappings, placement_mapping_object;
    Json placement_result, placement_object, placement_value, placement_visit_result;
    Json default_memory_result, default_memory_object, default_memory_ratio, default_memory_value;
    Json experiment_intent_result, experiment_memory_replacement;
    Json workload_projected, workload_result, kernel_parameters, kernel_seed, kernel_result;
    Json seed_result, seed_object, seed_value;
    Json cell_id_result, cell_id_object, cell_id_value;
    Json service_pools, service_workers, service_pool_object, service_cpus, service_cores;
    Json service_result, service_object, service_value;
    Json ordinary_workers, ordinary_variants, ordinary_variant_object, ordinary_variant_value;
    Json ordinary_comparison, ordinary_origin_object, ordinary_origin_value;
    Json ordinary_result, ordinary_object, ordinary_value;
    Json matrix_worker_axis, matrix_memory_axis, matrix_rows, matrix_row_object, matrix_workers;
    Json matrix_cells, matrix_cell_object, matrix_distance_object, matrix_edges, matrix_edge_object;
    Json matrix_comparison_object, matrix_result, matrix_object, matrix_value;
    Json scenario_workloads, scenario_projected, scenario_experiments, scenario_empty;
    Json scenario_resources, scenario_services, scenario_result, scenario_object, scenario_value;
    Json resolved_service, resolved_workloads, resolved_projected, resolved_experiments;
    Json resolved_fingerprints, resolved_profiles, resolved_result, resolved_object, resolved_value;
};
struct RecursivePayloadSlots {
    Json semantic_result, semantic_child, semantic_return, semantic_tagged_object, semantic_value;
    Json object_helper_result, object_helper_argument, object_helper_return;
    Json set_converted_value, set_assignment_value, set_old_object, set_replacement_object;
    Json append_value, append_return, clone_result, clone_child, clone_return;
};
struct DecodedStringMirror { std::vector<std::uint32_t> scalars; };
struct MemberMirror {
    const std::string* key; const nlohmann::json* value;
    DecodedStringMirror decoded; std::vector<std::uint16_t> ordering_key;
};
struct CanonicalFrameCells {
    const nlohmann::json* value;
    std::string_view path;
    std::string* output;
    bool jcs;
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 3> integer_buffer;
    std::to_chars_result integer_result;
    std::vector<MemberMirror> members;
    nlohmann::json::const_iterator begin, end, current;
    std::size_t index;
    std::expected<DecodedStringMirror, Failure> decoded;
    std::vector<std::uint16_t> units;
    MemberMirror member, member_argument;
    const MemberMirror *left, *right, *member_receiver;
    std::expected<void, core::Error> result, returned;
    std::unexpected<core::Error> unexpected;
    std::string path_parent, path_open, path_quoted, path_close, path_return, index_decimal;
    std::string_view child_parent, child_key;
    const DecodedStringMirror* child_decoded;
};
struct UnicodeCells {
    std::string_view input, escaped_input, hex;
    bool retain;
    DecodedStringMirror decoded, returned;
    std::expected<DecodedStringMirror, Failure> result;
    std::size_t index, length, continuation, escaped_index, byte_count;
    std::uint8_t first, byte;
    std::uint32_t scalar, minimum, adjusted;
    const DecodedStringMirror *units_receiver, *escaped_receiver;
    std::vector<std::uint16_t> units, units_return;
    std::string* escaped_output;
};
struct JsonHelperFrameCells {
    const Json* source;
    Json *object, *destination, *callback_argument;
    Json::const_iterator begin, end, current;
    Json::iterator found;
    std::string_view key;
    std::string key_copy;
    Json::object_t::Container old_members, replacement_members;
    core::detail::JsonCleanup<Json> source_cleanup;
    core::detail::JsonPointerCleanup<Json> result_cleanup;
    const nlohmann::json* unordered_source;
    nlohmann::json *unordered_destination, *unordered_callback_argument;
    nlohmann::json::const_iterator unordered_begin, unordered_end, unordered_current;
    nlohmann::json::iterator unordered_found;
    nlohmann::json unordered_result, unordered_child, unordered_return, unordered_assignment;
    core::detail::JsonCleanup<nlohmann::json> unordered_cleanup;
    core::detail::JsonPointerCleanup<nlohmann::json> unordered_result_cleanup;
    std::size_t member_count, reserve_count;
    Json *retire_value, *retire_child;
    Json::iterator retire_begin, retire_end;
    nlohmann::json *unordered_retire_value, *unordered_retire_child;
    nlohmann::json::iterator unordered_retire_begin, unordered_retire_end;
    core::detail::JsonCleanup<Json>* cleanup_receiver;
    core::detail::JsonPointerCleanup<Json>* pointer_cleanup_receiver;
    core::detail::JsonCleanup<nlohmann::json>* unordered_cleanup_receiver;
    core::detail::JsonPointerCleanup<nlohmann::json>* unordered_pointer_cleanup_receiver;
    core::detail::ExpectedErrorCleanup<std::expected<Json, core::Error>>* projected_error_cleanup_receiver;
    core::detail::ExpectedErrorCleanup<std::expected<std::string, core::Error>>* canonical_error_cleanup_receiver;
};
struct ProjectorCallCells {
    const RetainedPlanProjectionView *scenario, *binding_scenario, *reference_scenario, *profiles_scenario, *deferred_scenario;
    const Json *source, *intent, *variant_intents, *effect, *confidence, *order;
    const ExactRatio* ratio;
    const ResolvedProcessingUnit* worker;
    const ResolvedServiceCpu* service_worker;
    const ResolvedMemoryNode* node;
    const ResolvedMemoryPlacement* common;
    const ResolvedPerWorkerMemoryPlacement* per_worker;
    const ComparisonMemoryPlacement* alternative;
    const ResolvedPerWorkerMemoryNode* mapping;
    const DispatchedWorkload* workload;
    const ExperimentWorkloadBinding* binding;
    const ExperimentWorkerReference* reference;
    const ResolvedComparisonExperiment* comparison;
    const ExpandedMatrixExperiment* matrix;
    const MatrixCellId* cell_id;
    std::size_t index, variant_index, worker_index, row_index, cell_index, expected_row, expected_column;
    std::vector<ExperimentWorkloadBinding>::const_iterator binding_found, binding_end;
    std::vector<ExperimentWorkerReference>::const_iterator reference_found, reference_end;
    std::vector<ResolvedComparisonExperiment>::const_iterator comparison_found, comparison_end;
    std::vector<ExpandedMatrixExperiment>::const_iterator matrix_found, matrix_end;
    std::vector<ComparisonVariantMemoryPlacement>::const_iterator memory_found, memory_end;
    std::map<std::string, ResolvedWorkerSet>::const_iterator workers_found, workers_end;
    std::map<std::string, DispatchedWorkload>::const_iterator workload_found, workload_end;
    std::set<core::LinuxCpuId> worker_ids, service_union, pool_ids;
    std::set<core::LinuxNumaNodeId> node_ids;
    std::set<std::string_view> worker_stable_ids, node_stable_ids;
    std::set<std::string> service_cores;
    std::set<std::size_t> binding_indexes, seen_bindings;
    std::set<MatrixCellId> matrix_ids;
    std::vector<std::string> retained_cores, candidates;
    std::expected<Json, core::Error> workload_projection, service_projection, experiment_projection;
    std::expected<ExactRatio, core::Error> expected_effect, expected_confidence;
    std::string decimal_result, digest_hex, digest_text, physical_hex, physical_text, effective_hex, effective_text;
    std::string error_classification, error_phase, error_underlying;
    core::Error error_result;
    std::unexpected<core::Error> error_return;
    const kernel::SequentialReadBandwidthConfig* bandwidth;
    const kernel::DependentLoadLatencyConfig* latency;
    const kernel::PointerCycleSeedIntent* seed_intent;
    std::optional<std::uint64_t> seed_value;
    kernel::PointerCycleSeedMode seed_mode;
    core::ByteCount working_set;
    ResolvedMemoryNode temporary_node;
    Json error_context;
    core::detail::JsonCleanup<Json> error_cleanup;
};
// Exact captured reference types at the 28 existing json_target callback sites.
// Forwarding-reference and const invocation receivers are separate pointer roles.
struct ProjectorCallbackCells {
    const ExactRatio* json_target_0;
    const ResolvedProcessingUnit* json_target_1;
    const ResolvedServiceCpu* json_target_2;
    const ResolvedMemoryNode* json_target_3;
    const ResolvedMemoryPlacement* json_target_4_placement; Json* json_target_4_nodes;
    const ResolvedPerWorkerMemoryNode* json_target_5;
    const ResolvedPerWorkerMemoryPlacement* json_target_6_placement; Json* json_target_6_mappings;
    const Json *json_target_7, *json_target_8;
    std::byte json_target_9_empty, json_target_10_empty;
    const ExperimentWorkloadBinding* json_target_11;
    const MatrixCellId* json_target_12;
    Json* json_target_13_workers;
    const RetainedPlanProjectionView* json_target_14_scenario; Json *json_target_14_pools, *json_target_14_cpus, *json_target_14_cores;
    std::size_t* json_target_15_variant; const Json* json_target_15_intents;
    std::vector<ComparisonVariantMemoryPlacement>::const_iterator* json_target_15_found;
    std::vector<ResolvedComparisonExperiment>::const_iterator* json_target_16;
    std::size_t* json_target_17_index; const Json* json_target_17_intent;
    const ExperimentWorkloadBinding* json_target_17_binding; Json *json_target_17_workers, *json_target_17_variants, *json_target_17_comparison;
    const MatrixWorkerRow* json_target_18_row; Json* json_target_18_workers;
    const ResolvedMatrixCell* json_target_19_cell; const ResolvedMatrixCell* json_target_20_cell;
    const ExpandedMatrixExperiment* json_target_21_matrix; const MatrixComparisonEdge* json_target_21_edge;
    const ExpandedMatrixExperiment* json_target_22_matrix; std::size_t* json_target_22_index;
    const ExperimentWorkloadBinding* json_target_22_binding;
    Json *json_target_22_worker_axis, *json_target_22_memory_axis, *json_target_22_rows, *json_target_22_cells, *json_target_22_edges;
    const ExpandedMatrixExperiment* json_target_23;
    const Json *json_target_24_source, *json_target_24_empty;
    Json *json_target_24_resources, *json_target_24_services, *json_target_24_workloads, *json_target_24_experiments;
    const ScenarioDigest* json_target_25_digest;
    Json *json_target_25_workloads, *json_target_25_experiments;
    std::expected<Json, core::Error>* json_target_25_service;
    const topology::TopologyFingerprints* json_target_26;
    std::byte json_target_27_empty;
    // Each site has the same forwarding/receiver/reference representation but
    // does not invent the size of a compiler-generated anonymous closure.
    std::array<const ProjectorCallbackCells*, 28> forwarding_arguments, const_receivers;
    std::array<Json*, 28> json_arguments;
};
struct ShaCells {
    std::array<std::uint32_t, 8> state;
    std::array<std::uint32_t, 64> words;
    std::array<std::byte, 128> final;
    std::array<std::byte, 32> result;
    std::array<std::uint32_t, 8> working;
    std::array<std::uint32_t, 8>* compress_state;
    const std::byte* compress_block;
    std::uint32_t s0, s1, sum0, sum1, choice, majority, temporary1, temporary2;
    std::size_t block, schedule_index, schedule_offset, round, remainder, final_size, length_index, word, byte_index;
    std::uint64_t bit_length;
    std::span<const std::byte> input;
    std::string_view input_text, hex_digits;
    core::Sha256Digest returned;
    std::string hex, prefixed;
    std::byte byte;
    std::uint8_t hex_value;
    const std::byte *hex_begin, *hex_end;
    const core::Sha256Digest* hex_receiver;
    const ScenarioDigest* scenario_receiver;
    const topology::PhysicalTopologyFingerprint* physical_receiver;
    const topology::EffectiveTopologyFingerprint* effective_receiver;
};
struct ErrorCells {
    std::string scenario_classification, scenario_phase, scenario_underlying;
    std::string kernel_classification, core_code, core_message, core_path, core_classification;
    nlohmann::json scenario_context, kernel_context, core_context;
    core::detail::JsonCleanup<nlohmann::json> scenario_cleanup, kernel_cleanup, core_cleanup;
    core::Error scenario_return, kernel_return, core_return, strict_return;
    std::unexpected<core::Error> scenario_unexpected, kernel_unexpected, core_unexpected, strict_unexpected;
    const char* strict_code;
    bool unicode_scalar;
};
struct InspectorMirror {
    ProjectionInputLimits limits;
    ProjectionInputUsage usage;
    ProjectionInputFailure failure;
};
struct InspectionFrameCells {
    const Json *tree_value, *semantic_value, *effective_value, *member_object, *text_value, *unsigned_value, *ratio_value;
    const Json *metadata, *random, *experiments, *resources, *sets, *experiment, *variants, *order, *candidates, *seed, *unit;
    Json::const_iterator tree_begin, tree_end, tree_current, semantic_begin, semantic_end, semantic_current, found, end;
    std::string_view utf8_value, identifier_value, member_key, text_return, baseline, strategy;
    std::size_t tree_depth, utf8_index, comparison_i, comparison_j, unique_i, unique_j, count;
    unsigned utf8_length, continuation; unsigned char lead, next;
    std::uint32_t scalar, minimum; std::uint64_t numerator, denominator;
    InspectorMirror *tree_receiver, *string_receiver, *entries_receiver, *facts_receiver, *each_capture;
    const InspectorMirror *each_forwarding_receiver, *each_const_receiver;
    std::size_t *add_stock; std::size_t add_count, add_ceiling;
    bool projected, matrix;
    const RetainedPlanProjectionView* facts_view;
    const ResolvedProcessingUnit* worker_argument;
    const ResolvedServiceCpu* service_worker_argument;
    const ResolvedMemoryNode* node_argument;
    const ResolvedMemoryPlacement* common_argument;
    const ResolvedPerWorkerMemoryPlacement* per_worker_argument;
    const ComparisonMemoryPlacement* placement_argument;
    const ResolvedPerWorkerMemoryNode* mapping_argument;
    const DispatchedWorkload* workload_argument;
    const ExperimentWorkloadBinding* binding_argument;
    const ExperimentWorkerReference* reference_argument;
    const ResolvedComparisonExperiment* comparison_argument;
    const ResolvedComparisonVariant* variant_argument;
    const ExpandedMatrixExperiment* matrix_argument;
    const MatrixWorkerRow* row_argument;
    const ResolvedMatrixCell* cell_argument;
    const MatrixComparisonEdge* edge_argument;
    const core::CpuSet* cpus_argument;
    std::uint64_t cpu_count;
    const topology::StableCoreId* stable_core_argument;
    const topology::StableProcessingUnitId* stable_pu_argument;
    const topology::StableNumaNodeId* stable_node_argument;
    const kernel::PointerCycleSeedIntent* seed_argument;
    std::optional<std::uint64_t> seed_value;
    ValueOriginKind origin;
    std::size_t* variant_capture_size;
    const core::CpuSet* worker_domain_capture;
    const ResolvedMemoryPlacement* node_domain_capture;
    const ComparisonMemoryPlacement* placement_visitor_capture;
    const ResolvedComparisonExperiment* comparison_capture;
    const ExpandedMatrixExperiment* matrix_capture;
    std::map<std::string, ResolvedWorkerSet>::const_iterator workers_begin, workers_end;
    std::map<std::string, ResolvedServiceCpuPool>::const_iterator pools_begin, pools_end;
    std::map<std::string, DispatchedWorkload>::const_iterator workloads_begin, workloads_end, workload_found;
    std::vector<ResolvedProcessingUnit>::const_iterator pu_begin, pu_end;
    std::vector<ResolvedServiceCpu>::const_iterator service_begin, service_end;
    std::vector<ResolvedMemoryNode>::const_iterator nodes_begin, nodes_end;
    std::vector<ResolvedPerWorkerMemoryNode>::const_iterator mappings_begin, mappings_end;
    std::vector<topology::StableCoreId>::const_iterator cores_begin, cores_end;
    std::vector<ExperimentWorkloadBinding>::const_iterator bindings_begin, bindings_end;
    std::vector<ExperimentWorkerReference>::const_iterator references_begin, references_end;
    std::vector<ComparisonVariantMemoryPlacement>::const_iterator memory_begin, memory_end;
    std::vector<ResolvedComparisonExperiment>::const_iterator comparisons_begin, comparisons_end;
    std::vector<ResolvedComparisonVariant>::const_iterator variants_begin, variants_end;
    std::vector<std::string>::const_iterator candidates_begin, candidates_end;
    std::vector<ExpandedMatrixExperiment>::const_iterator matrices_begin, matrices_end;
    std::vector<MatrixWorkerRow>::const_iterator rows_begin, rows_end;
    std::vector<ResolvedMatrixCell>::const_iterator cells_begin, cells_end;
    std::vector<MatrixComparisonEdge>::const_iterator edges_begin, edges_end;
    std::vector<DeferredKernelConstraint>::const_iterator constraints_begin, constraints_end;
    std::expected<ProjectionInputUsage, ProjectionInputFailure> result;
    std::unexpected<ProjectionInputFailure> rejected;
};
struct WalkFrameCells {
    Walker *receiver, *callback_capture, *forwarding_receiver, *const_receiver;
    const Json *semantic_value, *source, *intent, *compared, *experiment;
    const RetainedPlanProjectionView* view;
    const ResolvedProcessingUnit* worker;
    const ResolvedServiceCpu* service_worker;
    const ResolvedMemoryNode* node;
    const ResolvedMemoryPlacement* common;
    const ResolvedPerWorkerMemoryPlacement* per_worker;
    const ComparisonMemoryPlacement* placement;
    const DispatchedWorkload* workload;
    const ExperimentWorkloadBinding* binding;
    const ResolvedComparisonExperiment* comparison;
    const ResolvedComparisonVariant* variant;
    const ExpandedMatrixExperiment* matrix;
    const MatrixWorkerRow* row;
    const ResolvedMatrixCell* cell;
    const MatrixComparisonEdge* edge;
    const MatrixCellId* cell_id;
    const ExactRatio* ratio;
    const kernel::SequentialReadBandwidthConfig* bandwidth;
    const kernel::PointerCycleSeedIntent* seed;
    std::string_view string, key, scalar_text, origin_text, ordering_text;
    Decimal decimal, index_decimal;
    TextStock text, returned_text;
    std::size_t prior_path, width, index, size, level, amount, reserve, units;
    std::size_t Q::*quantity;
    std::optional<std::size_t> checked_product, checked_reservation;
    std::uint64_t integer, decimal_value;
    std::int64_t signed_value;
    bool value, tagged, matrix_default;
    Json::const_iterator current, begin, end, found, resources;
    std::vector<ExperimentWorkloadBinding>::const_iterator binding_found;
    std::vector<ResolvedComparisonExperiment>::const_iterator comparison_found;
    std::vector<ExpandedMatrixExperiment>::const_iterator matrix_found;
    std::map<std::string, DispatchedWorkload>::const_iterator workload_begin, workload_end;
    std::map<std::string, ResolvedServiceCpuPool>::const_iterator pool_begin, pool_end;
    core::CpuSet::const_iterator cpu_begin;
    Json::object_t::Container ordered_domain;
    Json::array_t array_domain;
    nlohmann::json::object_t unordered_domain;
    nlohmann::json::array_t unordered_array_domain;
    const Json* discarded_argument;
    const ValueOrigin* origin_argument;
    Json::const_iterator existing_origin;
    std::size_t discarded_reservation;
};
struct ResourceOperationCells {
    const RetainedPlanProjectionView* view;
    ProjectionResourceLimits scenario_limits, resolved_limits;
    ProjectionResources result;
    InspectorMirror inspector;
    std::expected<ProjectionResources, Failure> returned;
    std::unexpected<Failure> rejected;
    Failure caught;
    Walker scenario_walk, resolved_walk, discarded_walk;
    Decimal decimal;
    TextStock text;
    std::size_t arithmetic_left, arithmetic_right, depth, semantic_depth;
    std::optional<std::size_t> arithmetic_result;
};

auto auxiliary_stock(Walker& walk, const RetainedPlanProjectionView& view) -> std::size_t {
    std::size_t result{};
    const auto add = [&](std::size_t count, std::size_t size) {
        walk.work(count); result = plus(result, times(count, size));
    };
    walk.work(plus(view.service_cpu_pools.size(), plus(view.comparison_experiments.size(), view.matrix_experiments.size())));
    add(view.experiment_workloads.size(), 2 * sizeof(std::size_t));
    for (const auto& [name, pool] : view.service_cpu_pools) {
        (void)name;
        add(pool.processing_units.size(), 2 * sizeof(core::LinuxCpuId) + sizeof(std::string));
        for (const auto& worker : pool.processing_units) add(worker.core_id.value().size(), 1);
    }
    for (const auto& core : view.reserved_service_cores) { add(1, sizeof(std::string)); add(core.value().size(), 1); }
    for (const auto& comparison : view.comparison_experiments) {
        add(comparison.workers.size(), sizeof(core::LinuxCpuId) + sizeof(std::string_view));
        for (const auto& candidate : comparison.candidate_variant_ids) { add(1, sizeof(std::string)); add(candidate.size(), 1); }
        for (const auto& variant : comparison.variants) std::visit([&](const auto& placement) {
            if constexpr (requires { placement.nodes; }) add(placement.nodes.size(), sizeof(core::LinuxNumaNodeId) + sizeof(std::string_view));
        }, variant.memory_placement);
    }
    for (const auto& matrix : view.matrix_experiments) {
        for (const auto& row : matrix.worker_rows) add(row.workers.size(), sizeof(core::LinuxCpuId) + sizeof(std::string_view));
        for (const auto& cell : matrix.cells) {
            add(1, sizeof(MatrixCellId));
            add(plus(cell.id.experiment_id.size(), plus(cell.id.worker_node.value().size(), cell.id.memory_node.value().size())), 1);
            add(cell.memory_placement.nodes.size(), sizeof(core::LinuxNumaNodeId) + sizeof(std::string_view));
            add(cell.memory_node_stable_id.value().size(), 1);
        }
    }
    return result;
}
void finish(Walker& walk, const RetainedPlanProjectionView& view) {
    auto& q = walk.q;
    const auto depth = plus(q.depth, 1);
    const auto semantic_depth = plus(std::max(q.depth, walk.discarded_depth), 1);
    // All named nonrecursive return/staging slots and depth-active recursive
    // slots retain separate logical copies. Explicit doubled arrays are extra
    // member slots, not invented key/value payload for unused reservations.
    const auto tree_slots = plus(sizeof(OrderedPayloadSlots) / sizeof(Json),
        times(semantic_depth, sizeof(RecursivePayloadSlots) / sizeof(Json)));
    const auto extra_members = walk.member_reservations - q.object_members;
    const auto ordered_tree = plus(walk.ordered_payload,
        times(extra_members, sizeof(Json::object_t::value_type)));
    // The overwritten input may be larger/deeper than the final result. Add its
    // separately measured payload to each named ordered staging slot's bound.
    q.ordered_staging_bytes = times(tree_slots, plus(ordered_tree, walk.discarded_payload));
    q.unordered_clone_bytes = times(plus(1, times(depth, 4)), walk.unordered_payload);
    // One decimal helper result per conceptual node conservatively includes every
    // decimal-producing helper occurrence; non-decimal nodes contribute spare
    // named decimal slots. Three digest renderings each hold hex and prefix forms.
    constexpr auto decimal_payload = std::numeric_limits<std::uint64_t>::digits10 + 1U;
    q.projector_auxiliary_bytes = plus(auxiliary_stock(walk, view),
        plus(times(plus(q.nodes, walk.discarded_nodes), decimal_payload), 3 * (71 + 64)));
    q.fixed_bytes = plus(projection_resource_detail::walk_fixed_stock() + sizeof(OrderedPayloadSlots) + sizeof(ProjectorCallCells) + sizeof(ProjectorCallbackCells) +
        sizeof(UnicodeCells) + sizeof(ShaCells) + sizeof(ErrorCells) + sizeof(ResourceOperationCells),
        plus(times(semantic_depth, sizeof(RecursivePayloadSlots) + sizeof(JsonHelperFrameCells)),
             times(depth, sizeof(CanonicalFrameCells) + sizeof(InspectionFrameCells))));
    // Error strings/context plus every full node path, conservatively covering
    // all current/returned/concatenation diagnostic path copies.
    // Conservative union of the three context shapes (Scenario 3, Kernel 1,
    // Core 2 members), all possible fixed strings at the selected helpers, and
    // their argument/returned-string payload roles. Strict rejection has null
    // context. Unsupported canonical types are rejected earlier, but retaining
    // their fixed error family makes the legacy helper inventory explicit.
    constexpr auto scenario_literals = std::string_view{
        "scenario.digest.internal_failureScenario digest construction failedcompute_scenario_plan_digests"
        "classificationphaseunderlying_codescenario_projection_failedresolved_plan_projection_failed"
        "contained_unknown_failureprior_stage_profile_mismatchinvariant_failedworkloadservicecomparisonmatrix"
        "scenario_projectionresolved_plan_projectionexperiment_bindingskernel.contract.invalid"}.size();
    constexpr auto kernel_literals = std::string_view{
        "kernel.contract.invalidKernel contract invariant failedkernel_contractclassification"
        "configuration_identity_mismatchunknown_kernel_identity"}.size();
    constexpr auto strict_literals = std::string_view{
        "scenario.projection.invalid_limitsscenario.projection.input_capacityscenario.projection.malformed_input"
        "Retained projection input rejectedinspect_retained_plan_projection"}.size();
    constexpr auto core_literals = std::string_view{
        "core.canonical_json.invalid_unicode_scalarcore.canonical_json.invalid_utf8"
        "JSON text contains an encoded Unicode surrogateJSON text is not well-formed UTF-8canonicalize_json"
        "pathclassificationstring_valueobject_property_namecore.canonical_json.floating_point_unsupported"
        "Floating-point JSON values are unsupported by the canonical profilefloating_point_unsupported"
        "core.canonical_json.unsupported_typeThe JSON value type is unsupported by the canonical profileunsupported_type"}.size();
    q.error_bytes = plus(times(q.path_bytes, 6), 3 * sizeof(nlohmann::json::object_t) +
        6 * sizeof(nlohmann::json::object_t::value_type) + 6 * sizeof(std::string) +
        2 * (scenario_literals + kernel_literals + strict_literals + core_literals));
    auto canonical = plus(q.canonical_bytes, times(q.canonical_member_elements, sizeof(MemberMirror)));
    canonical = plus(canonical, times(q.utf32_reserved_elements, 2 * sizeof(std::uint32_t)));
    canonical = plus(canonical, times(q.utf16_elements, 2 * sizeof(std::uint16_t)));
    canonical = plus(canonical, plus(times(q.path_bytes, 6), plus(q.quoted_key_bytes, q.index_decimal_bytes)));
    q.simultaneous_bytes = plus(q.ordered_staging_bytes, plus(q.unordered_clone_bytes,
        plus(q.projector_auxiliary_bytes, plus(q.fixed_bytes, plus(q.error_bytes, canonical)))));
    for (const auto field : quantities) need(q.*field <= walk.limits.*field);
}
} // namespace
namespace projection_resource_detail {
auto valid_limits(const ProjectionResourceLimits& limits) noexcept -> bool {
    for (const auto field : quantities) if (limits.*field > projection_input_max) return false;
    return limits.depth <= 64;
}
auto walk_fixed_stock() noexcept -> std::size_t {
    return sizeof(ResourceOperationCells) + 65 * (sizeof(WalkFrameCells) + sizeof(InspectionFrameCells));
}
}
auto measure_retained_projection_resources(const RetainedPlanProjectionView& view,
    ProjectionResourceLimits scenario_limits, ProjectionResourceLimits resolved_limits)
    -> std::expected<ProjectionResources, ProjectionResourceFailure> {
    try {
        need(projection_resource_detail::valid_limits(scenario_limits) &&
             projection_resource_detail::valid_limits(resolved_limits), Failure::invalid_limits);
        need(projection_resource_detail::walk_fixed_stock() <= scenario_limits.simultaneous_bytes &&
             projection_resource_detail::walk_fixed_stock() <= resolved_limits.simultaneous_bytes &&
             projection_resource_detail::walk_fixed_stock() <= scenario_limits.fixed_bytes &&
             projection_resource_detail::walk_fixed_stock() <= resolved_limits.fixed_bytes);
        Walker scenario_walk{scenario_limits}; scenario_walk.scenario(view); finish(scenario_walk, view);
        Walker resolved_walk{resolved_limits}; resolved_walk.resolved(view); finish(resolved_walk, view);
        return ProjectionResources{scenario_walk.q, resolved_walk.q};
    } catch (Failure failure) { return std::unexpected{failure}; }
}
} // namespace epycbench::scenario::detail
