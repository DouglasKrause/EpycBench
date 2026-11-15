#pragma once
#include "monitoring/host_frozen_source_admission.hpp"
#include "monitoring/host_q4_cpufreq_evidence_json.hpp"

namespace epycbench::monitoring::qualification::detail {
// Synthetic boundary access does not create a ready Admission or successful
// production Session; every returned lower result retains its actual domain.
class HostFrozenSourceAdmissionTestAccess final {
  public:
    // Only the executable-local fixture selects controlled Build leaves and a
    // synthetic published phase. The normal source entries never select these.
    static auto authenticate_source_synthetic(const std::filesystem::path& root,
        std::string_view qid, const HostFrozenSourceLaunchBinding& binding,
        const MonitoringRunningBuildAuthority::Operations& build) noexcept {
        return HostFrozenSourceAdmission::authenticate_impl(root, qid, &binding, &build);
    }
    static auto authenticate_retained_synthetic(const HostFrozenSourceRetainedInput& input,
        const MonitoringRunningBuildAuthority::Operations& build) noexcept {
        return HostFrozenSourceAdmission::authenticate_retained_impl(input, &build, true);
    }
    static auto retained_packet_valid(std::string_view) noexcept -> bool;
    static auto retained_q4(const std::array<std::string, 7>& bytes, std::string_view q4,
        const MonitoringQ4GovernedInputCandidate& candidate,
        const MonitoringQ4HistoricalGitValue& history, const MonitoringQ1ValidatedPublicValues& values,
        const MonitoringQ4GovernedPlanSchemaBinding& governed) {
        return HostFrozenSourceAdmission::verify_retained_q4(bytes, q4, candidate, history, values, governed);
    }
    static auto published_q5(std::string_view q5, std::string_view manifest, std::string_view checksums,
        const std::array<std::string, 7>& predecessor, std::string_view q4,
        std::string_view qid, std::string_view profile, std::string_view head,
        const core::Sha256Digest& build) {
        return HostFrozenSourceAdmission::verify_published_q5(q5, manifest, checksums,
            predecessor, q4, qid, profile, head, build);
    }
    static auto published_synthetic_q5(std::string_view q5, std::string_view manifest,
        std::string_view checksums, const std::array<std::string, 7>& predecessor,
        std::string_view q4, std::string_view qid, std::string_view profile,
        std::string_view head, const core::Sha256Digest& build) {
        return HostFrozenSourceAdmission::verify_published_q5(q5, manifest, checksums,
            predecessor, q4, qid, profile, head, build, true);
    }
    static auto retained_depth_valid(std::string_view bytes, std::size_t maximum) noexcept {
        return MonitoringQ4CompleteQ3PredecessorAuthenticator::retained_depth_valid(bytes, maximum);
    }
    static void prefix_hook(void (*hook)(unsigned char) noexcept) noexcept {
        MonitoringQ4CompleteQ3PredecessorAuthenticator::set_prefix_test_hook(hook);
    }
    static void parsed_hook(void (*hook)() noexcept) noexcept {
        MonitoringQ4RetainedEvidence::set_parsed_test_hook(hook);
    }
    static auto authenticate_controlled_build(const std::filesystem::path& root,
        const MonitoringRunningBuildAuthority::Operations& operations) {
        return MonitoringQ4AuthoritySession::authenticate_impl(root,
            "MON-007.host-v2.campaign-20261005-01", true, &operations);
    }
    static auto transfer(MonitoringQ4AuthoritySession)
        -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure>;
    static auto terminal_failure(MonitoringQ4AuthorityFailure)
        -> HostFrozenSourceAdmissionFailure;
    using SnapshotOperations = MonitoringQ4CompleteQ3PredecessorAuthority::Operations;
    static auto completed_snapshot(MonitoringQ4AuthoritySession& session) noexcept {
        return session.completed_predecessor_for_test();
    }
    static auto snapshot_operations(const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot) noexcept {
        return snapshot.operations_;
    }
    static auto snapshot_descriptor(const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot,
                                    std::size_t role) noexcept -> int {
        return role < snapshot.descriptors_.size() ? snapshot.descriptors_[role] : -1;
    }
    static void snapshot_close_leaf(MonitoringQ4CompleteQ3PredecessorAuthority& snapshot,
                                   int (*close)(void*, int) noexcept) noexcept {
        snapshot.operations_.close_fd = close;
    }
    static void inventory_test_leaves(void (*fault)(), int (*close)(void*) noexcept) noexcept {
        MonitoringQ4CompleteQ3PredecessorAuthority::set_inventory_test_leaves(fault, close);
    }
    static auto capture(int root, const MonitoringQ4GovernedInputCandidate& candidate,
        const MonitoringQ1ValidatedPublicValues& values, SnapshotOperations operations) {
        return MonitoringQ4CompleteQ3PredecessorAuthority::retain_impl(
            root, candidate, nullptr, operations, nullptr, &values, true);
    }
    static auto prefix(const std::array<std::string, 7>& bytes, std::string_view q4,
        const MonitoringQ4GovernedInputCandidate& candidate,
        const MonitoringQ1ValidatedPublicValues& values) {
        return MonitoringQ4CompleteQ3PredecessorAuthenticator::completed_prefix(bytes, q4, candidate, values);
    }
    static auto raw(const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot) noexcept {
        return std::string_view{snapshot.q4_bytes_};
    }
    static auto prefix_bytes(const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot) noexcept
        -> const std::array<std::string, 2>& { return snapshot.q3_prefix_; }
    static auto history(MonitoringQ4HistoricalGitAuthority::Operations operations,
        const MonitoringQ4GovernedInputCandidate& candidate,
        const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot) {
        return MonitoringQ4HistoricalGitAuthority::authenticate_completed_with(
            operations, candidate, snapshot, MonitoringQ4HistoricalGitAuthority::bootstrap_failure());
    }
    static auto bind(MonitoringQ4CompleteQ3PredecessorAuthority& snapshot,
        const MonitoringQ4HistoricalGitValue& history) { return snapshot.bind_campaign(history); }
    static auto verify(const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot,
        const MonitoringQ4HistoricalGitValue& history, const MonitoringQ4GovernedPlanSchemaBinding& governed,
        const MonitoringQ4GovernedInputCandidate& candidate, std::string_view bytes) {
        return MonitoringQ4RetainedEvidence::verify(*snapshot.value_, history, governed, candidate, bytes);
    }
    static auto finalize_inactive(MonitoringQ4AuthoritySession& session,
        std::optional<MonitoringQ1ValidatedPublicValues>& values,
        std::optional<MonitoringQ4RetainedEvidence>& evidence) {
        std::optional<std::array<std::string, 7>> predecessor;
        std::optional<MonitoringQ4GovernedInputValue> governed;
        return session.finalize_completed(values, evidence, predecessor, governed);
    }
    static auto hidden_inputs(const HostFrozenSourceAdmissionFailure&) noexcept -> bool;
    static void source_provenance_checkpoint(void (*checkpoint)(unsigned char) noexcept) noexcept {
        MonitoringQ4AuthoritySession::set_source_provenance_checkpoint(checkpoint);
    }
    static auto finalize_inputs(MonitoringQ4AuthoritySession& session,
        std::optional<MonitoringQ1ValidatedPublicValues>& values,
        std::optional<MonitoringQ4RetainedEvidence>& evidence,
        std::optional<std::array<std::string, 7>>& predecessor,
        std::optional<MonitoringQ4GovernedInputValue>& governed,
        std::optional<MonitoringQ4CompletedSourceProvenance>* provenance = nullptr,
        std::optional<MonitoringQ4HistoricalGitValue>* history = nullptr) {
        return session.finalize_completed(values, evidence, predecessor, governed, provenance, history);
    }
};
} // namespace epycbench::monitoring::qualification::detail
