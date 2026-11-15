#pragma once
#include "results/private_execution_plan.hpp"
#include "results/preflight_admission.hpp"
#include "results/private_host_serialization.hpp"

namespace epycbench::results::event_profile {
namespace detail { struct Work; }
struct PlanLimits {
    std::size_t bytes{},nodes{},depth{},text{},workloads{},bindings{},pairs{},
        retained{},simultaneous{};
    base_build_serialization::Limits base;
    kernel_report::Limits report;
    kernel_approval::Limits approval;
};
struct PlanFacts {
    std::string canonical,semantic;
    core::Sha256Digest digest,execution_digest;
    std::optional<base_build_serialization::Owner> base;
    std::array<std::optional<kernel_approval::Owner>,2> approvals;
    std::vector<implementation_selection::Workload> workloads;
    std::vector<implementation_selection::Binding> bindings;
    std::vector<execution_plan::Pair> pairs;
    execution_plan::Counts counts;
    std::size_t retained{},simultaneous{};
};
struct PreflightLimits {
    std::size_t bytes{},nodes{},depth{},text{},retained{},simultaneous{};
    preflight::Limits evaluator;
    validation_event::Limits monitoring;
};
namespace detail {
// Evidence-only reload against the exact retained normalization. No successful
// selection, Build association or ExecutionPlan producer owner is reconstructed.
[[nodiscard]] auto decode_plan(std::string_view,const normalization_serialization::Owner&,
    const PlanLimits&,Work&,std::size_t canonical_overlap = 0) -> PlanFacts;
[[nodiscard]] auto preflight_context(const normalization_serialization::Owner&,const host_serialization::Owner&,
    const base_build_serialization::Owner&) -> preflight::detail::FactualContext;
// Inline preflight field projection; the event envelope separately binds source
// addresses to earlier journal members and retains explicit synthetic origin.
[[nodiscard]] auto encode_preflight(const preflight::detail::State&,const PreflightLimits&,Work&) -> std::string;
[[nodiscard]] auto decode_preflight(std::string_view,const preflight::detail::FactualContext&,
    const PreflightLimits&,Work&) -> std::unique_ptr<preflight::detail::State>;
}
} // namespace epycbench::results::event_profile
