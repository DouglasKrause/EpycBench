#pragma once

#include "monitoring/host_q4_governed_input_authority.hpp"
#include "monitoring/host_q4_plan_value.hpp"
#include "monitoring/host_q4_schema_bundle.hpp"

#include <array>
#include <expected>
#include <memory>
#include <string>
#include <variant>

namespace epycbench::monitoring::qualification::detail {

class MonitoringQ4AuthoritySession;
class MonitoringQ4GovernedPlanSchemaBindingTestAccess;

enum class MonitoringQ4GovernedPlanSchemaBindingFixedFailure : unsigned char {
    invalid_binding,
    allocation,
    contained
};

using MonitoringQ4GovernedPlanSchemaBindingFailure =
    std::variant<MonitoringQ4SchemaBundleFailure, MonitoringGovernedPlanAuthoredDiagnostic,
                 core::Error, MonitoringQ4GovernedPlanSchemaBindingFixedFailure>;

class MonitoringQ4GovernedPlanSchemaBinding final {
  public:
    MonitoringQ4GovernedPlanSchemaBinding(const MonitoringQ4GovernedPlanSchemaBinding&) = delete;
    MonitoringQ4GovernedPlanSchemaBinding(MonitoringQ4GovernedPlanSchemaBinding&&) noexcept =
        default;
    auto operator=(const MonitoringQ4GovernedPlanSchemaBinding&)
        -> MonitoringQ4GovernedPlanSchemaBinding& = delete;
    auto operator=(MonitoringQ4GovernedPlanSchemaBinding&&)
        -> MonitoringQ4GovernedPlanSchemaBinding& = delete;

    [[nodiscard]] auto plan() const noexcept -> const MonitoringQ4PlanValue& { return plan_; }
    [[nodiscard]] auto schemas() const noexcept -> const MonitoringQ4ResolvedSchemaBundle& {
        return schemas_;
    }
    [[nodiscard]] auto captured_head() const noexcept -> std::string_view { return head_; }
    [[nodiscard]] auto plan_sha256() const noexcept -> const core::Sha256Digest& {
        return plan_sha256_;
    }
    [[nodiscard]] auto schema_sha256(MonitoringQ4SchemaKind kind) const noexcept
        -> const core::Sha256Digest*;

  private:
    struct Trace final {
        unsigned char* events{};
        std::size_t capacity{};
        std::size_t* count{};
        unsigned int* input_mask{};
    };
    static auto bind(const MonitoringQ4GovernedInputCandidate&, const std::shared_ptr<const void>&)
        -> std::expected<MonitoringQ4GovernedPlanSchemaBinding,
                         MonitoringQ4GovernedPlanSchemaBindingFailure>;
    static auto candidate_binding(const MonitoringQ4GovernedInputCandidate& candidate)
        -> std::shared_ptr<const void> {
        return candidate.binding_;
    }
    static void set_fault(unsigned char) noexcept;
    static void set_trace(Trace) noexcept;
    static thread_local Trace trace_;
    MonitoringQ4GovernedPlanSchemaBinding(MonitoringQ4PlanValue plan,
                                          MonitoringQ4ResolvedSchemaBundle schemas,
                                          std::string head, core::Sha256Digest plan_sha256,
                                          std::array<core::Sha256Digest, 5> schema_sha256,
                                          std::shared_ptr<const void> binding) noexcept
        : plan_{std::move(plan)}, schemas_{std::move(schemas)}, head_{std::move(head)},
          plan_sha256_{plan_sha256}, schema_sha256_{schema_sha256}, binding_{std::move(binding)} {}
    [[nodiscard]] auto bound_to(const std::shared_ptr<const void>& value) const noexcept -> bool {
        return binding_ && binding_ == value;
    }

    MonitoringQ4PlanValue plan_;
    MonitoringQ4ResolvedSchemaBundle schemas_;
    std::string head_;
    core::Sha256Digest plan_sha256_;
    std::array<core::Sha256Digest, 5> schema_sha256_;
    std::shared_ptr<const void> binding_;
    friend class HostFrozenSourceAdmission;
    friend class MonitoringQ4AuthoritySession;
    friend class MonitoringQ4GovernedPlanSchemaBindingTestAccess;
    friend class MonitoringQ4SourceReadyTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
