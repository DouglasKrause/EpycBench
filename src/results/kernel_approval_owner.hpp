#pragma once
#include "results/kernel_report_owner.hpp"
#include <array>

namespace epycbench::results::kernel_approval {
struct Limits {
    std::size_t input_bytes, string_bytes, integer_digits, nodes, entries, depth, retained_bytes,
        scanner_bytes;
};
struct Usage {
    std::size_t input_bytes{}, string_bytes{}, integer_digits{}, nodes{}, entries{}, depth{},
        retained_bytes{}, scanner_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct SemanticItem {
    std::string id, disposition;
};
struct ReportReference {
    std::string path, sha256;
};
struct Artifact {
    std::string automated_disposition, compiler_family, function_sha256,
        generated_human_review_status;
    ReportReference human_report, machine_report;
    std::string object_sha256, qualification_preset;
};
struct Approval {
    std::string actor, recorded_at_utc, disposition, statement;
    std::vector<SemanticItem> semantic_items;
};
enum class ReviewRoute { human_approval, independent_agents };
struct AgentSemanticItem {
    std::string id, conclusion, instruction_locations, explanation, uncertainty;
};
struct AgentAssessment {
    std::string compiler_family, qualification_preset, compiler_version, compiler_target,
        object_sha256, function_sha256, machine_report_sha256, human_report_sha256;
    std::vector<AgentSemanticItem> semantic_items;
};
struct AgentReviewer {
    std::string agent_id, run_id, model, started_at_utc, completed_at_utc, input_sha256;
    std::array<AgentAssessment, 2> assessments;
};
struct AgentQualification {
    std::string route, disposition, implementer_agent_id, input_sha256;
    std::vector<std::string> author_agent_ids;
    std::array<AgentReviewer, 2> reviewers;
};
struct Manifest {
    std::string schema, framework_version, policy_version, report_schema, implementation_id, issue,
        source_commit, title;
    Approval approval;
    std::array<Artifact, 2> artifacts;
    ReviewRoute route{ReviewRoute::human_approval};
    std::optional<AgentQualification> agent_qualification;
};
class Owner {
  public:
    Owner(const Owner&);
    auto operator=(const Owner&) -> Owner&;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    [[nodiscard]] auto manifest() const noexcept -> const Manifest*;
    [[nodiscard]] auto bytes() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto report(std::size_t slot) const noexcept -> const kernel_report::Owner*;
    [[nodiscard]] auto human_bytes(std::size_t slot) const noexcept -> std::string_view;
    [[nodiscard]] auto usage() const noexcept -> Usage;

  private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit Owner(std::unique_ptr<Storage>) noexcept;
    static auto retain_impl(std::string_view, kernel_report::Owner&&, kernel_report::Owner&&,
                            std::array<std::string_view, 2>, Limits, bool)
        -> std::expected<Owner, core::Error>;
    friend auto retain(std::string_view, kernel_report::Owner&&, kernel_report::Owner&&,
                       std::array<std::string_view, 2>, Limits)
        -> std::expected<Owner, core::Error>;
    friend auto retain_agent(std::string_view, kernel_report::Owner&&, kernel_report::Owner&&,
                             std::array<std::string_view, 2>, Limits)
        -> std::expected<Owner, core::Error>;
};
// Inputs are consumed only on success. Paths are labels; no acquisition occurs.
[[nodiscard]] auto retain(std::string_view, kernel_report::Owner&& gcc,
                          kernel_report::Owner&& clang, std::array<std::string_view, 2> human,
                          Limits) -> std::expected<Owner, core::Error>;
// Separate versioned agent route. Verified frozen-inventory execution is an external
// prerequisite; this owner checks supplied identities and retains exact evidence.
[[nodiscard]] auto retain_agent(std::string_view, kernel_report::Owner&& gcc,
                                kernel_report::Owner&& clang,
                                std::array<std::string_view, 2> readable, Limits)
    -> std::expected<Owner, core::Error>;
} // namespace epycbench::results::kernel_approval
