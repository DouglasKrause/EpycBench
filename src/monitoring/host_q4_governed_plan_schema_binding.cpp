#include "monitoring/host_q4_governed_plan_schema_binding.hpp"

#include <array>
#include <new>
#include <string_view>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr std::array filenames{
    std::string_view{"monitoring-q4-frequency-collection-plan-v1.schema.json"},
    std::string_view{"monitoring-host-q4-frequency-evidence-v1.schema.json"},
    std::string_view{"monitoring-host-q4-backend-result-v1.schema.json"},
    std::string_view{"monitoring-host-q4-backend-error-v1.schema.json"},
    std::string_view{"monitoring-host-q4-backend-authored-v1.schema.json"}};
thread_local unsigned char fault{};
} // namespace

thread_local MonitoringQ4GovernedPlanSchemaBinding::Trace
    MonitoringQ4GovernedPlanSchemaBinding::trace_{};

auto MonitoringQ4GovernedPlanSchemaBinding::bind(
    const MonitoringQ4GovernedInputCandidate& candidate,
    const std::shared_ptr<const void>& expected_binding)
    -> std::expected<MonitoringQ4GovernedPlanSchemaBinding,
                     MonitoringQ4GovernedPlanSchemaBindingFailure> {
    if (!candidate.valid_ || !candidate.binding_ || !expected_binding ||
        candidate.binding_ != expected_binding) {
        return std::unexpected{MonitoringQ4GovernedPlanSchemaBindingFixedFailure::invalid_binding};
    }
    try {
        const auto record = [](const unsigned char event) noexcept {
            if (trace_.input_mask != nullptr && event < 5U)
                *trace_.input_mask |= 1U << event;
            if (trace_.events != nullptr && trace_.count != nullptr &&
                *trace_.count < trace_.capacity)
                trace_.events[(*trace_.count)++] = event;
        };
        const std::array artifacts{&candidate.q4_plan_schema(), &candidate.q4_evidence_schema(),
                                   &candidate.q4_result_schema(), &candidate.q4_error_schema(),
                                   &candidate.q4_authored_schema()};
        std::array<MonitoringQ4SchemaInput, 5> inputs;
        for (std::size_t index{}; index < inputs.size(); ++index) {
            inputs[index] = MonitoringQ4SchemaInput{filenames[index], artifacts[index]->bytes};
            record(static_cast<unsigned char>(index));
        }
        const std::array digests{artifacts[0]->sha256, artifacts[1]->sha256, artifacts[2]->sha256,
                                 artifacts[3]->sha256, artifacts[4]->sha256};
        record(5U);
        auto schemas = resolve_monitoring_q4_schema_bundle(inputs);
        if (!schemas)
            return std::unexpected{MonitoringQ4GovernedPlanSchemaBindingFailure{schemas.error()}};
        record(6U);
        auto plan = decode_monitoring_q4_plan_authored(candidate.q4_plan().bytes);
        if (!plan) {
            if (auto* authored =
                    std::get_if<MonitoringGovernedPlanAuthoredDiagnostic>(&plan.error()))
                return std::unexpected{
                    MonitoringQ4GovernedPlanSchemaBindingFailure{std::move(*authored)}};
            return std::unexpected{MonitoringQ4GovernedPlanSchemaBindingFailure{
                std::move(std::get<core::Error>(plan.error()))}};
        }
        if (fault == 1U)
            throw std::bad_alloc{};
        if (fault == 2U)
            throw 1;
        return MonitoringQ4GovernedPlanSchemaBinding{std::move(*plan), std::move(*schemas),
                                                     candidate.head_,  candidate.q4_plan().sha256,
                                                     digests,          candidate.binding_};
    } catch (const std::bad_alloc&) {
        return std::unexpected{MonitoringQ4GovernedPlanSchemaBindingFixedFailure::allocation};
    } catch (...) {
        return std::unexpected{MonitoringQ4GovernedPlanSchemaBindingFixedFailure::contained};
    }
}

void MonitoringQ4GovernedPlanSchemaBinding::set_fault(const unsigned char value) noexcept {
    fault = value;
}

void MonitoringQ4GovernedPlanSchemaBinding::set_trace(const Trace value) noexcept {
    trace_ = value;
}

auto MonitoringQ4GovernedPlanSchemaBinding::schema_sha256(
    const MonitoringQ4SchemaKind kind) const noexcept -> const core::Sha256Digest* {
    switch (kind) {
    case MonitoringQ4SchemaKind::plan:
        return &schema_sha256_[0];
    case MonitoringQ4SchemaKind::evidence:
        return &schema_sha256_[1];
    case MonitoringQ4SchemaKind::backend_result:
        return &schema_sha256_[2];
    case MonitoringQ4SchemaKind::backend_error:
        return &schema_sha256_[3];
    case MonitoringQ4SchemaKind::backend_authored:
        return &schema_sha256_[4];
    }
    return nullptr;
}

} // namespace epycbench::monitoring::qualification::detail
