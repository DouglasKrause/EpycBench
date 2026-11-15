#pragma once

#include "monitoring/host_q4_authority_session.hpp"
#include "monitoring/host_frozen_source_launch.hpp"

#include <memory>

namespace epycbench::monitoring::qualification::detail {
struct HostFrozenSourceAdmissionState;
// Closed diagnostic checkpoints in primary.detail; they confer no authority.
enum class HostFrozenRetainedRecheck : unsigned int {
    repository = 1, governed, build, source_guard, retained_root,
    completed_state, retired_guard, retired_root
};
// Borrowed only from the live authenticated source input owner. The producer
// has accepted and published Q5 before minting this receipt; bytes alone cannot
// construct it or recreate a previous collector's Session.
class HostFrozenSourceRetainedInput final {
  public:
    HostFrozenSourceRetainedInput(const HostFrozenSourceRetainedInput&) = delete;
    HostFrozenSourceRetainedInput(HostFrozenSourceRetainedInput&&) = delete;
    ~HostFrozenSourceRetainedInput() noexcept = default;
  private:
    using Recheck = std::expected<void, HostFrozenSourceLaunchReason> (*)(
        void*, const std::filesystem::path&, std::uint64_t, std::uint64_t) noexcept;
    HostFrozenSourceRetainedInput(const HostFrozenSourceLaunchBinding&, std::string_view,
        std::string_view, std::string_view, std::string_view, const std::filesystem::path&,
        std::uint64_t, std::uint64_t, const std::filesystem::path&, std::uint64_t,
        std::uint64_t, const core::Sha256Digest&, std::string_view, void*, Recheck) noexcept;
    const HostFrozenSourceLaunchBinding* binding_;
    std::string_view packet_, q5_, manifest_, checksums_;
    const std::filesystem::path *repository_, *qualification_root_;
    std::uint64_t repository_device_, repository_inode_, qualification_device_, qualification_inode_;
    const core::Sha256Digest* build_;
    std::string_view build_bytes_;
    void* owner_;
    Recheck recheck_;
    friend struct HostFrozenSourceLaunchState;
    friend class HostFrozenSourceAdmission;
};
enum class HostFrozenSourceAccessFailure : unsigned char { inactive };
enum class HostFrozenPredecessorArtifact : unsigned char {
    environment, q0b, q1, q2, q3, manifest, checksums
};

class HostFrozenSourceAdmissionFailure final {
  public:
    HostFrozenSourceAdmissionFailure(HostFrozenSourceAdmissionFailure&&) noexcept;
    HostFrozenSourceAdmissionFailure(const HostFrozenSourceAdmissionFailure&) = delete;
    ~HostFrozenSourceAdmissionFailure() noexcept;
    [[nodiscard]] auto authority_failure() const noexcept -> const MonitoringQ4AuthorityFailure& {
        return failure_;
    }
  private:
    HostFrozenSourceAdmissionFailure(MonitoringQ4AuthorityFailure,
                                    std::unique_ptr<HostFrozenSourceAdmissionState>) noexcept;
    MonitoringQ4AuthorityFailure failure_;
    // Hidden facts and the consumed Session remain owned on refusal; neither
    // cleanup uncertainty nor a failed recheck can publish the genuine values.
    std::unique_ptr<HostFrozenSourceAdmissionState> retained_;
    friend class HostFrozenSourceAdmission;
    friend class HostFrozenSourceAdmissionTestAccess;
    friend struct HostFrozenSourceLaunchState;
};

class HostFrozenSourceAdmission final {
  public:
    HostFrozenSourceAdmission(HostFrozenSourceAdmission&&) noexcept;
    HostFrozenSourceAdmission(const HostFrozenSourceAdmission&) = delete;
    auto operator=(const HostFrozenSourceAdmission&) -> HostFrozenSourceAdmission& = delete;
    auto operator=(HostFrozenSourceAdmission&&) -> HostFrozenSourceAdmission& = delete;
    ~HostFrozenSourceAdmission() noexcept;
    [[nodiscard]] static auto authenticate(const std::filesystem::path&, std::string_view) noexcept
        -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure>;
    [[nodiscard]] auto values() const noexcept
        -> std::expected<const MonitoringQ1ValidatedPublicValues*, HostFrozenSourceAccessFailure>;
    [[nodiscard]] auto completed_q4_bytes() const noexcept
        -> std::expected<std::string_view, HostFrozenSourceAccessFailure>;
    [[nodiscard]] auto completed_predecessor_bytes(HostFrozenPredecessorArtifact) const noexcept
        -> std::expected<std::string_view, HostFrozenSourceAccessFailure>;
    [[nodiscard]] auto governed_inputs() const noexcept
        -> std::expected<const MonitoringQ4GovernedInputValue*, HostFrozenSourceAccessFailure>;
    [[nodiscard]] auto retained_source_provenance() const noexcept
        -> std::expected<const MonitoringQ4CompletedSourceProvenance*, HostFrozenSourceAccessFailure>;
    // A ready-only borrow of the accepted completed-Q4 history. The v0.1
    // publication owner also binds exact consumed inputs on the controlled,
    // unchanged checkout and refuses detected relevant drift.
    [[nodiscard]] auto retained_history() const noexcept
        -> std::expected<const MonitoringQ4HistoricalGitValue*, HostFrozenSourceAccessFailure>;
  private:
    [[nodiscard]] static auto authenticate_retained_source(const HostFrozenSourceRetainedInput&) noexcept
        -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure>;
    [[nodiscard]] static auto authenticate_retained_impl(const HostFrozenSourceRetainedInput&,
        const MonitoringRunningBuildAuthority::Operations*, bool synthetic) noexcept
        -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure>;
    struct RetainedPacket;
    [[nodiscard]] static auto decode_retained_packet(std::string_view) -> std::optional<RetainedPacket>;
    [[nodiscard]] static auto verify_retained_q4(const std::array<std::string, 7>&,
        std::string_view, const MonitoringQ4GovernedInputCandidate&,
        const MonitoringQ4HistoricalGitValue&, const MonitoringQ1ValidatedPublicValues&,
        const MonitoringQ4GovernedPlanSchemaBinding&)
        -> std::expected<MonitoringQ4RetainedEvidence, core::Error>;
    [[nodiscard]] static auto verify_published_q5(std::string_view, std::string_view,
        std::string_view, const std::array<std::string, 7>&, std::string_view,
        std::string_view, std::string_view, std::string_view, const core::Sha256Digest&,
        bool synthetic = false) -> bool;
    [[nodiscard]] static auto authenticate_source(const std::filesystem::path&, std::string_view,
        const HostFrozenSourceLaunchBinding&) noexcept
        -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure>;
    [[nodiscard]] static auto authenticate_impl(const std::filesystem::path&, std::string_view,
        const HostFrozenSourceLaunchBinding*,
        const MonitoringRunningBuildAuthority::Operations* = nullptr) noexcept
        -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure>;
    explicit HostFrozenSourceAdmission(std::unique_ptr<HostFrozenSourceAdmissionState>) noexcept;
    static auto finish(std::unique_ptr<HostFrozenSourceAdmissionState>) noexcept
        -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure>;
    [[nodiscard]] auto ready() const noexcept -> bool;
    std::unique_ptr<HostFrozenSourceAdmissionState> state_;
    friend class HostFrozenSourceAdmissionTestAccess;
    friend struct HostFrozenSourceLaunchState;
};
} // namespace epycbench::monitoring::qualification::detail
