#pragma once
#include "monitoring/host_q1_git_repository_metadata.hpp"
#include "monitoring/host_q1_git_spawn.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1GitFixedRecord final {
  public:
    MonitoringQ1GitFixedRecord(const MonitoringQ1GitFixedRecord&) = delete;
    MonitoringQ1GitFixedRecord(MonitoringQ1GitFixedRecord&&) noexcept = default;

  private:
    MonitoringQ1GitFixedRecord(std::string path, MonitoringQ1GitObjectId object, std::string bytes)
        : path_{std::move(path)}, object_{std::move(object)}, bytes_{std::move(bytes)} {}
    std::string path_;
    MonitoringQ1GitObjectId object_;
    std::string bytes_;
    friend class MonitoringQ1GitFixedResultAuthority;
    friend class MonitoringQ1GitFixedResultAuthorityTestAccess;
    friend class MonitoringQ1PublicInputAuthority;
    friend class MonitoringGovernedPlanInputCore;
    friend class MonitoringQ2HistoricalQ1GitAuthority;
    friend class MonitoringQ3HistoricalGitAuthority;
    friend class MonitoringQ3AuthoritySession;
};
struct MonitoringQ1GitFixedResults final {
    MonitoringQ1GitFixedResults(const MonitoringQ1GitFixedResults&) = delete;
    MonitoringQ1GitFixedResults(MonitoringQ1GitFixedResults&&) noexcept = default;
    MonitoringQ1GitFixedResults& operator=(const MonitoringQ1GitFixedResults&) = delete;
    MonitoringQ1GitFixedResults& operator=(MonitoringQ1GitFixedResults&&) noexcept = default;
    std::vector<MonitoringQ1GitFixedRecord> records;

  private:
    struct CaptureFrame final {};
    explicit MonitoringQ1GitFixedResults(std::string captured_head)
        : head_{std::move(captured_head)}, frame_{std::make_shared<CaptureFrame>()} {}
    [[nodiscard]] auto head() const noexcept -> std::string_view { return head_; }
    [[nodiscard]] auto binding() const noexcept -> const void* { return frame_.get(); }
    std::string head_;
    std::shared_ptr<CaptureFrame> frame_;
    friend class MonitoringQ1GitFixedResultAuthority;
    friend class MonitoringQ1GitFixedResultAuthorityTestAccess;
    friend class MonitoringQ1PublicInputAuthority;
    friend class MonitoringGovernedPlanInputCore;
    friend class MonitoringQ2HistoricalQ1GitAuthority;
    friend class MonitoringQ3HistoricalGitAuthority;
    friend class MonitoringQ3AuthoritySession;
};
class MonitoringQ1GitFixedResultAuthority final {
  public:
    [[nodiscard]] static auto run(const MonitoringQ1GitExecutable&,
                                  const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                                  const MonitoringQ1GitRepositoryMetadata&)
        -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto
    run_with_q2_plan(const MonitoringQ1GitExecutable&,
                     const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                     const MonitoringQ1GitRepositoryMetadata&)
        -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto
    run_with_q3_plan(const MonitoringQ1GitExecutable&,
                     const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                     const MonitoringQ1GitRepositoryMetadata&)
        -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto parse_head(std::string_view) -> std::optional<std::string>;
    [[nodiscard]] static auto parse_tree(std::string_view, std::string_view, std::string_view)
        -> std::optional<std::string>;

  private:
    struct Operations final {
        void* context{};
        std::expected<bool, MonitoringQ1GitCommandFailure> (*recheck)(void*){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*run)(
            void*, MonitoringQ1GitInvocation){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*run_blob)(
            void*, const MonitoringQ1GitObjectId&, const void*, std::size_t, std::size_t){};
    };
    [[nodiscard]] static auto run_with(Operations)
        -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto run_with(Operations, std::size_t)
        -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure>;
    friend class MonitoringQ1GitFixedResultAuthorityTestAccess;
};
} // namespace epycbench::monitoring::qualification::detail
