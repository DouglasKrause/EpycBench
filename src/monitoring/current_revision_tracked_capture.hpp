#pragma once

#include "monitoring/host_q1_git_fixed_results.hpp"

#include <array>

namespace epycbench::monitoring::qualification::detail {

class MonitoringCurrentRevisionTrackedRecord final {
  public:
    MonitoringCurrentRevisionTrackedRecord(const MonitoringCurrentRevisionTrackedRecord&) = delete;
    MonitoringCurrentRevisionTrackedRecord(MonitoringCurrentRevisionTrackedRecord&&) noexcept =
        default;

  private:
    MonitoringCurrentRevisionTrackedRecord(std::string path, MonitoringQ1GitObjectId object,
                                           std::string bytes)
        : path_{std::move(path)}, object_{std::move(object)}, bytes_{std::move(bytes)} {}
    std::string path_;
    MonitoringQ1GitObjectId object_;
    std::string bytes_;
    friend class MonitoringCurrentRevisionTrackedCaptureCore;
    friend class MonitoringCurrentRevisionTrackedCaptureTestAccess;
    friend class MonitoringGovernedTrackedBundleCore;
};

class MonitoringCurrentRevisionTrackedCapture final {
  public:
    static constexpr std::size_t q4_path_count = 17;
    MonitoringCurrentRevisionTrackedCapture(const MonitoringCurrentRevisionTrackedCapture&) =
        delete;
    MonitoringCurrentRevisionTrackedCapture(MonitoringCurrentRevisionTrackedCapture&&) noexcept =
        default;

  private:
    struct Frame final {};
    explicit MonitoringCurrentRevisionTrackedCapture(std::string head)
        : head_{std::move(head)}, frame_{std::make_shared<Frame>()} {}
    std::string head_;
    std::shared_ptr<Frame> frame_;
    std::vector<MonitoringCurrentRevisionTrackedRecord> records_;
    friend class MonitoringCurrentRevisionTrackedCaptureCore;
    friend class MonitoringCurrentRevisionTrackedCaptureTestAccess;
    friend class MonitoringGovernedTrackedBundleCore;
};

class MonitoringCurrentRevisionTrackedCaptureCore final {
  private:
    struct Operations final {
        void* context{};
        std::expected<bool, MonitoringQ1GitCommandFailure> (*recheck)(void*){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*head)(void*){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*status)(
            void*){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*tree)(
            void*, std::string_view){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*blob)(
            void*, const MonitoringQ1GitObjectId&, const void*, std::size_t, std::size_t){};
    };
    [[nodiscard]] static auto run_with(Operations)
        -> std::expected<MonitoringCurrentRevisionTrackedCapture, MonitoringQ1GitCommandFailure>;
    friend class MonitoringCurrentRevisionTrackedCaptureTestAccess;
    friend class MonitoringQ4CurrentRevisionTrackedCapture;
};

class MonitoringQ4CurrentRevisionTrackedCapture final {
  public:
    [[nodiscard]] static auto run(const MonitoringQ1GitExecutable&,
                                  const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                                  const MonitoringQ1GitRepositoryMetadata&)
        -> std::expected<MonitoringCurrentRevisionTrackedCapture, MonitoringQ1GitCommandFailure>;
};

} // namespace epycbench::monitoring::qualification::detail
