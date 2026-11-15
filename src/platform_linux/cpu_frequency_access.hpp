#pragma once

#include "epycbench/platform_linux/cpu_frequency.hpp"
#include "platform_linux/bounded_sysfs_operations.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::platform_linux::detail {

[[nodiscard]] inline auto cpu_frequency_unreadable_hardware_identity(
    const struct stat& before, const struct stat& after, uid_t effective_uid) noexcept -> bool {
    return effective_uid != 0 && before.st_uid == 0 &&
           before.st_mode == static_cast<mode_t>(S_IFREG | S_IRUSR) && before.st_nlink == 1 &&
           bounded_sysfs_same_identity(before, after);
}

class PreparedBoundedCpuFrequencyDiscoveryStorage;
class FailedPreparedBoundedCpuFrequencyDiscovery;
class PreparedBoundedCpuFrequencyDiscoveryAccess;
class PreparedBoundedCpuFrequencyDiscoveryTestAccess;

struct CpuFrequencyFile {
    std::string path;
    std::string contents;
};
enum class CpuFrequencyUnsupportedReason : unsigned char {
    no_resolvable_policy,
    no_current_attribute
};
struct CpuFrequencyPolicyView {
    std::uint32_t locator{};
    std::string_view related_cpus;
    std::string_view scaling_driver;
    std::string_view scaling_governor;
    std::string_view scaling_minimum;
    std::string_view scaling_maximum;
    std::optional<std::string_view> hardware_current;
    std::optional<std::string_view> scaling_current;
};
struct CpuFrequencySemanticProjection {
    std::vector<core::LinuxCpuId> related_cpus;
    CpuFrequencyCurrentKind kind{};
    std::string driver;
    std::string governor;
    std::uint64_t minimum_hz{};
    std::uint64_t maximum_hz{};
    std::uint32_t locator{};
};
struct CpuFrequencyDetailedOutcome {
    std::optional<CpuFrequencySemanticProjection> value;
    std::optional<CpuFrequencyUnsupportedReason> unsupported;
};
enum class CpuFrequencyBoundedFailure : unsigned char {
    membership,
    capacity,
    ambiguous,
    metadata,
    value
};
struct CpuFrequencyBoundedScratch final {
    std::vector<core::LinuxCpuId> parsed;
    std::vector<core::LinuxCpuId> selected;
    std::vector<core::LinuxCpuId> seen;
    std::vector<std::uint32_t> selected_policies;
};
struct CpuFrequencyBoundedProjection final {
    std::span<const core::LinuxCpuId> related_cpus;
    CpuFrequencyCurrentKind kind{};
    std::string_view driver;
    std::string_view governor;
    std::uint64_t minimum_hz{};
    std::uint64_t maximum_hz{};
    std::uint32_t locator{};
};
struct CpuFrequencyBoundedOutcome final {
    std::optional<CpuFrequencyBoundedProjection> value;
    std::optional<CpuFrequencyUnsupportedReason> unsupported;
};
enum class CpuFrequencyBoundedDiscoveryFailureKind : unsigned char {
    enumeration_capacity,
    token_capacity,
    semantic,
};
struct CpuFrequencyBoundedDiscoveryFailure final {
    CpuFrequencyBoundedDiscoveryFailureKind kind{};
    CpuFrequencyBoundedFailure semantic{CpuFrequencyBoundedFailure::membership};
};
struct CpuFrequencyBoundedGraph final {
    std::uint64_t domain_count{};
    std::uint64_t membership_count{};
    std::uint64_t covered_worker_count{};
    std::span<const std::uint32_t> selected_policies;
};
enum class BoundedCpuFrequencySourceFailureKind : unsigned char {
    enumerate,
    enumeration_capacity,
    token_capacity,
    membership,
    metadata,
    value,
    ambiguous,
    read,
    contained,
    preparation_rejected,
};
struct BoundedCpuFrequencySourceFailure final {
    BoundedCpuFrequencySourceFailureKind kind{};
    int native{};
    BoundedSysfsDispositionKind disposition{BoundedSysfsDispositionKind::success};
};
struct BoundedCpuFrequencySourceFailureSequence final {
    BoundedCpuFrequencySourceFailure primary;
    std::size_t cleanup_count{};
    BoundedCpuFrequencySourceFailureSequence(
        BoundedCpuFrequencySourceFailure value) noexcept : primary{value} {}
    BoundedCpuFrequencySourceFailureSequence(
        BoundedCpuFrequencySourceFailure value, std::size_t count) noexcept
        : primary{value}, cleanup_count{count} {}
};
struct BoundedCpuFrequencyCapacity final {
    std::uint64_t maximum_cpu{};
    std::uint64_t root_entries{};
    std::uint64_t maximum_policies{};
    std::uint64_t maximum_memberships{};
    std::uint64_t token_bytes{};
};

enum class BoundedCpuFrequencyValidatedDisposition : unsigned char {
    unsupported,
    supported,
};

class BoundedCpuFrequencyValidatedGraphOutcome final {
  public:
    BoundedCpuFrequencyValidatedGraphOutcome(
        const BoundedCpuFrequencyValidatedGraphOutcome&) = delete;
    BoundedCpuFrequencyValidatedGraphOutcome(
        BoundedCpuFrequencyValidatedGraphOutcome&&) noexcept = default;
    [[nodiscard]] auto disposition() const noexcept {
        return disposition_;
    }
    [[nodiscard]] auto domain_count() const noexcept -> std::size_t {
        return memberships_.size();
    }
    [[nodiscard]] auto domain_memberships() const noexcept
        -> std::span<const std::size_t> { return memberships_; }
  private:
    BoundedCpuFrequencyValidatedGraphOutcome(
        BoundedCpuFrequencyValidatedDisposition disposition,
        std::vector<std::size_t> memberships) noexcept
        : disposition_{disposition}, memberships_{std::move(memberships)} {}
    BoundedCpuFrequencyValidatedDisposition disposition_{};
    std::vector<std::size_t> memberships_;
    friend class BoundedCpuFrequencySources;
    friend class BoundedCpuFrequencyValidatedGraphOutcomeTestAccess;
};

struct BoundedCpuFrequencyValidationGate final {
    void* context{};
    bool (*accept)(void*, BoundedCpuFrequencyValidatedGraphOutcome&&) noexcept{};
};

class BoundedCpuFrequencySources final {
  public:
    BoundedCpuFrequencySources(const BoundedCpuFrequencySources&) = delete;
    BoundedCpuFrequencySources(BoundedCpuFrequencySources&&) noexcept;
    auto operator=(const BoundedCpuFrequencySources&)
        -> BoundedCpuFrequencySources& = delete;
    auto operator=(BoundedCpuFrequencySources&&)
        -> BoundedCpuFrequencySources& = delete;
    ~BoundedCpuFrequencySources() noexcept;
    [[nodiscard]] static auto discover(
        BoundedCpuFrequencyCapacity,
        std::span<const core::LinuxCpuId> present_cpus,
        std::span<const core::LinuxCpuId> worker_cpus,
        BoundedSysfsOperations,
        std::span<BoundedCpuFrequencySourceFailure> cleanup_stock,
        BoundedCpuFrequencyValidationGate gate = {}) noexcept
        -> std::expected<std::optional<BoundedCpuFrequencySources>,
                         BoundedCpuFrequencySourceFailureSequence>;
    [[nodiscard]] static auto discover_prepared(
        PreparedBoundedCpuFrequencyDiscoveryStorage&&,
        BoundedCpuFrequencyValidationGate gate = {}) noexcept
        -> std::expected<std::optional<BoundedCpuFrequencySources>,
                         FailedPreparedBoundedCpuFrequencyDiscovery>;
    [[nodiscard]] auto domain_count() const noexcept -> std::size_t;
    [[nodiscard]] auto related_cpus(std::size_t) const noexcept
        -> std::span<const core::LinuxCpuId>;
    [[nodiscard]] auto kind(std::size_t) const noexcept
        -> CpuFrequencyCurrentKind;
    [[nodiscard]] auto driver(std::size_t) const noexcept -> std::string_view;
    [[nodiscard]] auto governor(std::size_t) const noexcept -> std::string_view;
    [[nodiscard]] auto minimum_hz(std::size_t) const noexcept -> std::uint64_t;
    [[nodiscard]] auto maximum_hz(std::size_t) const noexcept -> std::uint64_t;
    [[nodiscard]] auto read(
        std::size_t, std::span<char>,
        std::span<BoundedCpuFrequencySourceFailure> cleanup_stock) const noexcept
        -> std::expected<std::uint64_t, BoundedCpuFrequencySourceFailureSequence>;
    [[nodiscard]] auto retire(std::span<BoundedSysfsDisposition> close_dispositions) noexcept
        -> std::size_t;
  private:
    struct State;
    [[nodiscard]] static auto discover_prepared_engine(
        PreparedBoundedCpuFrequencyDiscoveryStorage&,
        BoundedCpuFrequencyValidationGate) noexcept
        -> std::expected<std::optional<BoundedCpuFrequencySources>,
                         BoundedCpuFrequencySourceFailureSequence>;
    explicit BoundedCpuFrequencySources(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend class PreparedBoundedCpuFrequencyDiscoveryStorage;
    friend class PreparedBoundedCpuFrequencyDiscoveryTestAccess;
};

class PreparedBoundedCpuFrequencyDiscoveryStorage final {
  public:
    PreparedBoundedCpuFrequencyDiscoveryStorage(
        const PreparedBoundedCpuFrequencyDiscoveryStorage&) = delete;
    PreparedBoundedCpuFrequencyDiscoveryStorage(
        PreparedBoundedCpuFrequencyDiscoveryStorage&&) noexcept;
    auto operator=(const PreparedBoundedCpuFrequencyDiscoveryStorage&)
        -> PreparedBoundedCpuFrequencyDiscoveryStorage& = delete;
    auto operator=(PreparedBoundedCpuFrequencyDiscoveryStorage&&) noexcept
        -> PreparedBoundedCpuFrequencyDiscoveryStorage& = delete;
    ~PreparedBoundedCpuFrequencyDiscoveryStorage() noexcept;
    [[nodiscard]] static auto prepare(
        BoundedCpuFrequencyCapacity,
        std::span<const core::LinuxCpuId> present_cpus,
        std::span<const core::LinuxCpuId> worker_cpus,
        BoundedSysfsOperations) noexcept
        -> std::expected<PreparedBoundedCpuFrequencyDiscoveryStorage,
                         BoundedCpuFrequencySourceFailureSequence>;
  private:
    struct State;
    explicit PreparedBoundedCpuFrequencyDiscoveryStorage(
        std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend class BoundedCpuFrequencySources;
    friend class FailedPreparedBoundedCpuFrequencyDiscovery;
    friend class PreparedBoundedCpuFrequencyDiscoveryAccess;
    friend class PreparedBoundedCpuFrequencyDiscoveryTestAccess;
};

class FailedPreparedBoundedCpuFrequencyDiscovery final {
  public:
    FailedPreparedBoundedCpuFrequencyDiscovery(
        const FailedPreparedBoundedCpuFrequencyDiscovery&) = delete;
    FailedPreparedBoundedCpuFrequencyDiscovery(
        FailedPreparedBoundedCpuFrequencyDiscovery&&) noexcept;
    auto operator=(const FailedPreparedBoundedCpuFrequencyDiscovery&)
        -> FailedPreparedBoundedCpuFrequencyDiscovery& = delete;
    auto operator=(FailedPreparedBoundedCpuFrequencyDiscovery&&) noexcept
        -> FailedPreparedBoundedCpuFrequencyDiscovery& = delete;
    ~FailedPreparedBoundedCpuFrequencyDiscovery() noexcept;
  private:
    FailedPreparedBoundedCpuFrequencyDiscovery(
        PreparedBoundedCpuFrequencyDiscoveryStorage&&,
        BoundedCpuFrequencySourceFailureSequence) noexcept;
    std::optional<PreparedBoundedCpuFrequencyDiscoveryStorage> storage_;
    BoundedCpuFrequencySourceFailureSequence sequence_{{}};
    friend class BoundedCpuFrequencySources;
    friend class PreparedBoundedCpuFrequencyDiscoveryAccess;
};

class PreparedBoundedCpuFrequencyDiscoveryAccess final {
  public:
    [[nodiscard]] static auto cleanup(
        const FailedPreparedBoundedCpuFrequencyDiscovery&) noexcept
        -> std::optional<std::span<const BoundedCpuFrequencySourceFailure>>;
    [[nodiscard]] static auto sequence(
        const FailedPreparedBoundedCpuFrequencyDiscovery&) noexcept
        -> std::optional<BoundedCpuFrequencySourceFailureSequence>;
};

class CpuFrequencyAccess {
  public:
    using ReadOperation = CpuFrequencyPolicySource::ReadOperation;
    [[nodiscard]] static auto discover(core::LinuxCpuId cpu,
                                       std::span<const CpuFrequencyFile> files, void* context,
                                       ReadOperation read)
        -> std::expected<std::optional<CpuFrequencyPolicySource>, core::Error>;
    [[nodiscard]] static auto parse_khz(std::string_view text)
        -> std::expected<std::uint64_t, core::Error>;
    [[nodiscard]] static auto parse_khz_bounded(std::string_view text) noexcept
        -> std::optional<std::uint64_t>;
    [[nodiscard]] static auto project(core::LinuxCpuId, std::span<const CpuFrequencyPolicyView>)
        -> std::expected<CpuFrequencyDetailedOutcome, core::Error>;
    [[nodiscard]] static auto project_bounded(core::LinuxCpuId,
                                              std::span<const CpuFrequencyPolicyView>,
                                              CpuFrequencyBoundedScratch&) noexcept
        -> std::expected<CpuFrequencyBoundedOutcome, CpuFrequencyBoundedFailure>;
    [[nodiscard]] static auto discover_bounded(
        core::LinuxCpuId, std::span<const CpuFrequencyPolicyView>,
        std::uint64_t maximum_policies, std::uint64_t maximum_memberships,
        std::uint64_t token_bytes, CpuFrequencyBoundedScratch&) noexcept
        -> std::expected<CpuFrequencyBoundedOutcome,
                         CpuFrequencyBoundedDiscoveryFailure>;
    [[nodiscard]] static auto validate_graph_bounded(
        std::span<const core::LinuxCpuId> present_cpus,
        std::span<const core::LinuxCpuId> worker_cpus,
        std::span<const CpuFrequencyPolicyView>, std::uint64_t maximum_policies,
        std::uint64_t maximum_memberships, std::uint64_t token_bytes,
        CpuFrequencyBoundedScratch&) noexcept
        -> std::expected<CpuFrequencyBoundedGraph,
                         CpuFrequencyBoundedDiscoveryFailure>;
};

} // namespace epycbench::platform_linux::detail
