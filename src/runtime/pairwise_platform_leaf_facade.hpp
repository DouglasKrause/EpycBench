#pragma once

#include "epycbench/runtime/pairwise_evidence_failure.hpp"
#include "epycbench/platform_linux/task_stat.hpp"
#include "epycbench/platform_linux/perf_context_switch.hpp"
#include "platform_linux/clock_operations.hpp"
#include "platform_linux/cpu_mask_allocator.hpp"
#include "platform_linux/cpu_frequency_access.hpp"
#include "platform_linux/hwmon_temperature_access.hpp"
#include "platform_linux/mapping_operations.hpp"
#include "platform_linux/numa_operations.hpp"
#include "platform_linux/perf_context_switch_access.hpp"
#include "platform_linux/thread_operations.hpp"
#include "platform_linux/task_stat_operations.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <memory>

namespace epycbench::runtime::detail {

class PairwisePlatformPreparedDiscoveryTestAccess;

enum class PairwisePlatformLeafOperation : std::uint8_t {
    raw_clock,
    thread_identity,
    current_cpu,
    set_affinity,
    get_affinity,
    map,
    advise,
    protect,
    unmap,
    numa_mask,
    numa_bind,
    numa_query,
    task_stat,
    perf_prepare,
    perf_drain,
    perf_retire,
};

using PairwiseTaskStatOperations = platform_linux::detail::BoundedTaskStatOperations;

struct PairwiseTaskStatReadFailure final {
    PairwisePlatformFailure primary;
    std::optional<PairwisePlatformFailure> cleanup;
    [[nodiscard]] auto code() const noexcept { return primary.code(); }
    [[nodiscard]] auto operation() const noexcept { return primary.operation(); }
    [[nodiscard]] auto reason() const noexcept { return primary.reason(); }
    [[nodiscard]] auto context() const noexcept -> const PairwiseFailureContext& {
        return primary.context();
    }
    [[nodiscard]] auto posix_errno() const noexcept { return primary.posix_errno(); }
};

struct PairwiseSysfsPreparationFailure final {
    PairwisePlatformFailure primary;
    std::vector<PairwisePlatformFailure> cleanup;
    [[nodiscard]] auto code() const noexcept { return primary.code(); }
    [[nodiscard]] auto operation() const noexcept { return primary.operation(); }
    [[nodiscard]] auto reason() const noexcept { return primary.reason(); }
    [[nodiscard]] auto context() const noexcept -> const PairwiseFailureContext& {
        return primary.context();
    }
    [[nodiscard]] auto posix_errno() const noexcept { return primary.posix_errno(); }
};

class PairwisePreparedHwmonDiscoveryStorage final {
  public:
    PairwisePreparedHwmonDiscoveryStorage(
        const PairwisePreparedHwmonDiscoveryStorage&) = delete;
    PairwisePreparedHwmonDiscoveryStorage(
        PairwisePreparedHwmonDiscoveryStorage&& other) noexcept
        : lower_{std::move(other.lower_)},
          mapped_cleanup_{std::move(other.mapped_cleanup_)},
          consumed_{std::exchange(other.consumed_, true)} {}
    auto operator=(const PairwisePreparedHwmonDiscoveryStorage&)
        -> PairwisePreparedHwmonDiscoveryStorage& = delete;
    auto operator=(PairwisePreparedHwmonDiscoveryStorage&&)
        -> PairwisePreparedHwmonDiscoveryStorage& = delete;
    ~PairwisePreparedHwmonDiscoveryStorage() noexcept = default;

  private:
    PairwisePreparedHwmonDiscoveryStorage(
        platform_linux::detail::PreparedBoundedHwmonDiscoveryStorage lower,
        std::vector<PairwisePlatformFailure> mapped_cleanup) noexcept
        : lower_{std::move(lower)},
          mapped_cleanup_{std::move(mapped_cleanup)} {}
    platform_linux::detail::PreparedBoundedHwmonDiscoveryStorage lower_;
    std::vector<PairwisePlatformFailure> mapped_cleanup_;
    bool consumed_{};
    friend class PairwisePlatformLeafFacade;
    friend class PairwisePlatformPreparedDiscoveryTestAccess;
};

class PairwisePreparedCpuFrequencyDiscoveryStorage final {
  public:
    PairwisePreparedCpuFrequencyDiscoveryStorage(
        const PairwisePreparedCpuFrequencyDiscoveryStorage&) = delete;
    PairwisePreparedCpuFrequencyDiscoveryStorage(
        PairwisePreparedCpuFrequencyDiscoveryStorage&& other) noexcept
        : lower_{std::move(other.lower_)},
          mapped_cleanup_{std::move(other.mapped_cleanup_)},
          consumed_{std::exchange(other.consumed_, true)} {}
    auto operator=(const PairwisePreparedCpuFrequencyDiscoveryStorage&)
        -> PairwisePreparedCpuFrequencyDiscoveryStorage& = delete;
    auto operator=(PairwisePreparedCpuFrequencyDiscoveryStorage&&)
        -> PairwisePreparedCpuFrequencyDiscoveryStorage& = delete;
    ~PairwisePreparedCpuFrequencyDiscoveryStorage() noexcept = default;

  private:
    PairwisePreparedCpuFrequencyDiscoveryStorage(
        platform_linux::detail::PreparedBoundedCpuFrequencyDiscoveryStorage lower,
        std::vector<PairwisePlatformFailure> mapped_cleanup) noexcept
        : lower_{std::move(lower)},
          mapped_cleanup_{std::move(mapped_cleanup)} {}
    platform_linux::detail::PreparedBoundedCpuFrequencyDiscoveryStorage lower_;
    std::vector<PairwisePlatformFailure> mapped_cleanup_;
    bool consumed_{};
    friend class PairwisePlatformLeafFacade;
    friend class PairwisePlatformPreparedDiscoveryTestAccess;
};

using PairwisePerfContextSwitchOperations =
    platform_linux::detail::BoundedPerfContextSwitchOperations;

struct PairwiseBoundedTextReadOperations final {
    struct Result final {
        std::size_t count{};
        int error_number{};
        bool overflow{};
    };
    void* context{};
    Result (*read)(void*, std::uint32_t, std::span<char>) noexcept{};
};

using PairwiseHwmonDiscoveryEntry = platform_linux::detail::BoundedHwmonEntry;

class PairwisePreparedTemperatureSource final {
  public:
    PairwisePreparedTemperatureSource(const PairwisePreparedTemperatureSource&) = delete;
    PairwisePreparedTemperatureSource(PairwisePreparedTemperatureSource&&) noexcept = default;
    ~PairwisePreparedTemperatureSource() noexcept;
    [[nodiscard]] auto locator() const noexcept -> std::uint32_t { return locator_; }
  private:
    PairwisePreparedTemperatureSource(std::uint32_t locator,
                                      PairwiseBoundedTextReadOperations operations) noexcept
        : locator_{locator}, operations_{operations} {}
    explicit PairwisePreparedTemperatureSource(
        platform_linux::detail::BoundedHwmonTemperatureSource source) noexcept
        : owned_{std::move(source)} {}
    std::uint32_t locator_{};
    PairwiseBoundedTextReadOperations operations_{};
    std::optional<platform_linux::detail::BoundedHwmonTemperatureSource> owned_;
    friend class PairwisePlatformLeafFacade;
};

class PairwisePreparedFrequencySources final {
  public:
    PairwisePreparedFrequencySources(const PairwisePreparedFrequencySources&) = delete;
    PairwisePreparedFrequencySources(PairwisePreparedFrequencySources&&) noexcept = default;
    [[nodiscard]] auto domain_count() const noexcept -> std::size_t {
        return sources_.domain_count();
    }
    [[nodiscard]] auto related_cpus(std::size_t index) const noexcept {
        return sources_.related_cpus(index);
    }
    [[nodiscard]] auto kind(std::size_t index) const noexcept {
        return sources_.kind(index);
    }
    [[nodiscard]] auto driver(std::size_t index) const noexcept {
        return sources_.driver(index);
    }
    [[nodiscard]] auto governor(std::size_t index) const noexcept {
        return sources_.governor(index);
    }
    [[nodiscard]] auto minimum_hz(std::size_t index) const noexcept {
        return sources_.minimum_hz(index);
    }
    [[nodiscard]] auto maximum_hz(std::size_t index) const noexcept {
        return sources_.maximum_hz(index);
    }
  private:
    explicit PairwisePreparedFrequencySources(
        platform_linux::detail::BoundedCpuFrequencySources sources) noexcept
        : sources_{std::move(sources)} {}
    platform_linux::detail::BoundedCpuFrequencySources sources_;
    friend class PairwisePlatformLeafFacade;
};

class PairwisePreparedFrequencySource final {
  public:
    PairwisePreparedFrequencySource(const PairwisePreparedFrequencySource&) = delete;
    PairwisePreparedFrequencySource(PairwisePreparedFrequencySource&&) noexcept = default;
    [[nodiscard]] auto related_cpus() const noexcept
        -> std::span<const core::LinuxCpuId> { return related_cpus_; }
    [[nodiscard]] auto kind() const noexcept -> platform_linux::CpuFrequencyCurrentKind {
        return kind_;
    }
    [[nodiscard]] auto driver() const noexcept -> std::string_view { return driver_; }
    [[nodiscard]] auto governor() const noexcept -> std::string_view { return governor_; }
    [[nodiscard]] auto minimum_hz() const noexcept -> std::uint64_t { return minimum_hz_; }
    [[nodiscard]] auto maximum_hz() const noexcept -> std::uint64_t { return maximum_hz_; }
  private:
    PairwisePreparedFrequencySource(
        std::vector<core::LinuxCpuId> cpus,
        platform_linux::CpuFrequencyCurrentKind kind, std::string driver,
        std::string governor, std::uint64_t minimum, std::uint64_t maximum,
        std::uint32_t locator, PairwiseBoundedTextReadOperations operations) noexcept
        : related_cpus_{std::move(cpus)}, kind_{kind}, driver_{std::move(driver)},
          governor_{std::move(governor)}, minimum_hz_{minimum}, maximum_hz_{maximum},
          locator_{locator}, operations_{operations} {}
    std::vector<core::LinuxCpuId> related_cpus_;
    platform_linux::CpuFrequencyCurrentKind kind_{};
    std::string driver_;
    std::string governor_;
    std::uint64_t minimum_hz_{};
    std::uint64_t maximum_hz_{};
    std::uint32_t locator_{};
    PairwiseBoundedTextReadOperations operations_{};
    friend class PairwisePlatformLeafFacade;
};

struct PairwiseNumaBindInvocation final {
    void* address{};
    std::size_t length{};
    std::uint64_t base_page_bytes{};
    int mode{};
    unsigned flags{};
    bool selected_mask_nonempty{};
    bool selected_maxnode_within_domain{};
    bool selected_nodes_effective{};
    bool selected_nodes_online{};
    bool selected_nodes_memory_backed{};
};

struct PairwisePlatformLeafDependencies final {
    platform_linux::detail::ClockOperations clock;
    platform_linux::detail::ThreadOperations threads;
    platform_linux::detail::CpuMaskAllocator cpu_masks;
    platform_linux::detail::MappingOperations mapping;
    platform_linux::detail::NumaOperations numa;
    platform_linux::detail::WordAllocator numa_words;
    platform_linux::detail::StatusAllocator numa_statuses;
    platform_linux::detail::ResultAllocator numa_results;
    PairwiseTaskStatOperations task_stat{};
    PairwisePerfContextSwitchOperations perf_context{};
    void* hook_context{};
    void (*before_operation)(void*, PairwisePlatformLeafOperation){};
    void (*adjust_numa_bind_invocation)(void*, PairwiseNumaBindInvocation&) noexcept{};
    bool (*retain_mapping_after_successful_unmap)(void*) noexcept{};
    std::uint64_t affinity_capacity_limit{
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U};
    std::uint64_t numa_mask_native_limit{std::numeric_limits<unsigned long>::max()};
    std::size_t numa_mask_size_limit{std::numeric_limits<std::size_t>::max()};
    std::uint64_t numa_length_limit{std::numeric_limits<unsigned long>::max()};
    std::uint64_t numa_count_limit{std::numeric_limits<unsigned long>::max()};
};

class PairwiseTaskStatWorkspace final {
  public:
    static constexpr std::size_t capacity = 16U * 1024U;
  private:
    std::array<char, capacity> bytes_{};
    friend class PairwisePlatformLeafFacade;
};

class PairwiseSysfsReadWorkspace final {
  public:
    [[nodiscard]] static auto prepare(std::size_t content_capacity)
        -> PairwiseSysfsReadWorkspace {
        return PairwiseSysfsReadWorkspace{std::vector<char>(content_capacity + 1U),
                                           content_capacity};
    }
  private:
    explicit PairwiseSysfsReadWorkspace(std::vector<char> bytes,
                                        std::size_t content_capacity) noexcept
        : bytes_{std::move(bytes)}, content_capacity_{content_capacity} {}
    std::vector<char> bytes_;
    std::size_t content_capacity_{};
    friend class PairwisePlatformLeafFacade;
};

class PairwisePerfDrainStorage final {
  public:
    PairwisePerfDrainStorage(const PairwisePerfDrainStorage&) = delete;
    PairwisePerfDrainStorage(PairwisePerfDrainStorage&&) noexcept = default;
    auto operator=(const PairwisePerfDrainStorage&) -> PairwisePerfDrainStorage& = delete;
    auto operator=(PairwisePerfDrainStorage&&) -> PairwisePerfDrainStorage& = delete;
    [[nodiscard]] static auto prepare(std::size_t capacity) -> PairwisePerfDrainStorage;
    [[nodiscard]] auto capacity() const noexcept -> std::size_t { return records_.capacity(); }
    void reset_drain_boundary() noexcept { last_successful_drain_completed_.reset(); }
    void note_drain_completed(platform_linux::RawMonotonicTimestamp value) noexcept {
        last_successful_drain_completed_ = value;
    }
    [[nodiscard]] auto last_successful_drain_completed() const noexcept
        -> std::optional<platform_linux::RawMonotonicTimestamp> {
        return last_successful_drain_completed_;
    }
  private:
    explicit PairwisePerfDrainStorage(
        std::vector<platform_linux::PerfContextSwitchRecord> records) noexcept
        : records_{std::move(records)} {}
    std::vector<platform_linux::PerfContextSwitchRecord> records_;
    std::optional<platform_linux::RawMonotonicTimestamp> last_successful_drain_completed_{};
    friend class PairwisePlatformLeafFacade;
};

struct PairwisePerfDrainView final {
    std::span<const platform_linux::PerfContextSwitchRecord> records;
    platform_linux::PerfCaptureCompleteness completeness;
};

struct PairwisePerfRetirement final {
    std::optional<PairwisePlatformFailure> primary;
    std::optional<PairwisePlatformFailure> cleanup;
};

class PairwisePreparedPerfContextSwitch final {
  public:
    struct State;
    PairwisePreparedPerfContextSwitch(const PairwisePreparedPerfContextSwitch&) = delete;
    PairwisePreparedPerfContextSwitch(PairwisePreparedPerfContextSwitch&&) noexcept;
    auto operator=(const PairwisePreparedPerfContextSwitch&)
        -> PairwisePreparedPerfContextSwitch& = delete;
    auto operator=(PairwisePreparedPerfContextSwitch&&)
        -> PairwisePreparedPerfContextSwitch& = delete;
    ~PairwisePreparedPerfContextSwitch() noexcept;
    [[nodiscard]] auto active() const noexcept -> bool;
  private:
    explicit PairwisePreparedPerfContextSwitch(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
    friend class PairwisePlatformLeafFacade;
};

class PairwisePerfPreparationWorkspace final {
  public:
    PairwisePerfPreparationWorkspace(const PairwisePerfPreparationWorkspace&) = delete;
    PairwisePerfPreparationWorkspace(PairwisePerfPreparationWorkspace&&) noexcept;
    auto operator=(const PairwisePerfPreparationWorkspace&)
        -> PairwisePerfPreparationWorkspace& = delete;
    auto operator=(PairwisePerfPreparationWorkspace&&)
        -> PairwisePerfPreparationWorkspace& = delete;
    ~PairwisePerfPreparationWorkspace() noexcept;
  private:
    explicit PairwisePerfPreparationWorkspace(
        std::unique_ptr<PairwisePreparedPerfContextSwitch::State> state) noexcept;
    std::unique_ptr<PairwisePreparedPerfContextSwitch::State> state_;
    friend class PairwisePlatformLeafFacade;
};

struct PairwisePerfPreparationFailure final {
    PairwisePlatformFailure primary;
    std::array<std::optional<PairwisePlatformFailure>, 2> cleanup;
};

struct PairwiseNumaBindWorkspace final {
    std::vector<unsigned long> words;
    bool einval_available{true};
};

struct PairwiseDiagnosedNumaBindFailure final {
    PairwisePlatformFailure primary;
    std::optional<platform_linux::detail::NumaBindEinvalDiagnosis> diagnosis;
};

struct PairwiseLinuxNumaNodeSelection final {
    const void* context{};
    std::size_t count{};
    core::LinuxNumaNodeId (*at)(const void*, std::size_t) noexcept{};
};

class PairwisePreparedNumaQueryBuffers final {
  public:
    PairwisePreparedNumaQueryBuffers(const PairwisePreparedNumaQueryBuffers&) = delete;
    auto operator=(const PairwisePreparedNumaQueryBuffers&)
        -> PairwisePreparedNumaQueryBuffers& = delete;
    PairwisePreparedNumaQueryBuffers(PairwisePreparedNumaQueryBuffers&&) noexcept = default;
    auto operator=(PairwisePreparedNumaQueryBuffers&&) noexcept
        -> PairwisePreparedNumaQueryBuffers& = default;
    ~PairwisePreparedNumaQueryBuffers() noexcept = default;

    [[nodiscard]] static auto prepare(std::size_t capacity)
        -> PairwisePreparedNumaQueryBuffers;
    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
        return raw_statuses_.size();
    }
    [[nodiscard]] auto results() const noexcept
        -> std::span<const platform_linux::PageQueryStatus> {
        return results_;
    }

  private:
    PairwisePreparedNumaQueryBuffers(std::vector<int> raw_statuses,
                                    std::vector<platform_linux::PageQueryStatus> results) noexcept
        : raw_statuses_{std::move(raw_statuses)}, results_{std::move(results)} {}

    std::vector<int> raw_statuses_;
    std::vector<platform_linux::PageQueryStatus> results_;
    friend class PairwisePlatformLeafFacade;
};

class PairwisePreparedAffinityWorkspace final {
  public:
    PairwisePreparedAffinityWorkspace(const PairwisePreparedAffinityWorkspace&) = delete;
    auto operator=(const PairwisePreparedAffinityWorkspace&)
        -> PairwisePreparedAffinityWorkspace& = delete;
    PairwisePreparedAffinityWorkspace(PairwisePreparedAffinityWorkspace&&) noexcept = default;
    auto operator=(PairwisePreparedAffinityWorkspace&&) noexcept
        -> PairwisePreparedAffinityWorkspace& = default;
    ~PairwisePreparedAffinityWorkspace() noexcept = default;

    [[nodiscard]] auto capacity() const noexcept -> std::size_t { return mask_.capacity(); }

  private:
    explicit PairwisePreparedAffinityWorkspace(
        platform_linux::DynamicCpuMask mask) noexcept
        : mask_{std::move(mask)} {}

    platform_linux::DynamicCpuMask mask_;
    friend class PairwisePlatformLeafFacade;
};

class PairwisePlatformLeafFacade final {
  public:
    [[nodiscard]] static auto production() noexcept -> PairwisePlatformLeafFacade;
    [[nodiscard]] static auto create(PairwisePlatformLeafDependencies dependencies) noexcept
        -> PairwisePlatformLeafFacade;

    [[nodiscard]] auto raw_monotonic_now() const noexcept
        -> std::expected<platform_linux::RawMonotonicTimestamp, PairwisePlatformFailure>;
    [[nodiscard]] auto current_thread_id() const noexcept
        -> std::expected<platform_linux::LinuxThreadId, PairwisePlatformFailure>;
    [[nodiscard]] auto current_cpu() const noexcept
        -> std::expected<core::LinuxCpuId, PairwisePlatformFailure>;
    [[nodiscard]] auto set_current_thread_affinity(const core::CpuSet& cpus) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto get_current_thread_affinity() const noexcept
        -> std::expected<core::CpuSet, PairwisePlatformFailure>;
    [[nodiscard]] auto prepare_affinity_workspace(
        std::size_t minimum_capacity) const noexcept
        -> std::expected<PairwisePreparedAffinityWorkspace, PairwisePlatformFailure>;
    [[nodiscard]] auto prepare_affinity_workspace_exact(
        std::size_t capacity) const noexcept
        -> std::expected<PairwisePreparedAffinityWorkspace, PairwisePlatformFailure>;
    [[nodiscard]] auto set_current_thread_affinity_prepared(
        core::LinuxCpuId cpu, PairwisePreparedAffinityWorkspace& workspace) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto current_thread_affinity_is_singleton_prepared(
        core::LinuxCpuId cpu, PairwisePreparedAffinityWorkspace& workspace) const noexcept
        -> std::expected<bool, PairwisePlatformFailure>;
    [[nodiscard]] auto map(std::size_t length) const noexcept
        -> std::expected<platform_linux::AnonymousMapping, PairwisePlatformFailure>;
    [[nodiscard]] auto advise_no_huge_pages(platform_linux::AnonymousMapping& mapping) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto protect_read_only(platform_linux::AnonymousMapping& mapping) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto unmap(platform_linux::AnonymousMapping& mapping) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto numa_mask(const core::LinuxNumaNodeSet& nodes) const noexcept
        -> std::expected<platform_linux::NativeNumaMask, PairwisePlatformFailure>;
    [[nodiscard]] auto prepare_numa_mask(
        PairwiseLinuxNumaNodeSelection nodes) const noexcept
        -> std::expected<platform_linux::NativeNumaMask, PairwisePlatformFailure>;
    [[nodiscard]] auto bind(const platform_linux::AnonymousMapping& mapping,
                            const platform_linux::NativeNumaMask& nodes) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto bind_diagnosed(
        const platform_linux::AnonymousMapping& mapping,
        const platform_linux::NativeNumaMask& nodes,
        platform_linux::detail::NumaDomainMaxNode domain,
        const platform_linux::detail::NumaBindInvocationFacts& facts,
        PairwiseNumaBindWorkspace& workspace) const noexcept
        -> std::expected<void, PairwiseDiagnosedNumaBindFailure>;
    [[nodiscard]] auto query_pages(std::span<void* const> pages) const noexcept
        -> std::expected<std::vector<platform_linux::PageQueryStatus>, PairwisePlatformFailure>;
    [[nodiscard]] auto query_pages_prepared(
        std::span<void* const> pages,
        PairwisePreparedNumaQueryBuffers& buffers) const noexcept
        -> std::expected<std::span<const platform_linux::PageQueryStatus>,
                         PairwisePlatformFailure>;
    [[nodiscard]] auto read_task_page_fault_counters(
        platform_linux::LinuxThreadId thread,
        PairwiseTaskStatWorkspace& workspace) const noexcept
        -> std::expected<platform_linux::LinuxTaskPageFaultCounters,
                         PairwiseTaskStatReadFailure>;
    [[nodiscard]] auto discover_hwmon_temperature(
        std::uint64_t root_entries, std::uint64_t directory_count,
        std::span<const PairwiseHwmonDiscoveryEntry> entries,
        std::uint64_t maximum_directories, std::uint64_t entries_per_directory,
        std::uint64_t token_bytes, PairwiseBoundedTextReadOperations operations,
        std::optional<int> enumeration_error = std::nullopt) const noexcept
        -> std::expected<std::optional<PairwisePreparedTemperatureSource>,
                         PairwisePlatformFailure>;
    [[nodiscard]] auto discover_hwmon_temperature(
        platform_linux::detail::BoundedHwmonCapacity,
        platform_linux::detail::BoundedSysfsOperations,
        platform_linux::detail::BoundedHwmonValidationGate gate = {}) const noexcept
        -> std::expected<std::optional<PairwisePreparedTemperatureSource>,
                         PairwiseSysfsPreparationFailure>;
    [[nodiscard]] auto prepare_hwmon_discovery_storage(
        platform_linux::detail::BoundedHwmonCapacity,
        platform_linux::detail::BoundedSysfsOperations) const noexcept
        -> std::expected<PairwisePreparedHwmonDiscoveryStorage,
                         PairwiseSysfsPreparationFailure>;
    [[nodiscard]] auto discover_hwmon_temperature_prepared(
        PairwisePreparedHwmonDiscoveryStorage&&,
        platform_linux::detail::BoundedHwmonValidationGate gate = {}) const noexcept
        -> std::expected<std::optional<PairwisePreparedTemperatureSource>,
                         PairwiseSysfsPreparationFailure>;
    [[nodiscard]] auto read_hwmon_temperature(
        const PairwisePreparedTemperatureSource& source,
        PairwiseSysfsReadWorkspace& workspace) const noexcept
        -> std::expected<std::int64_t, PairwisePlatformFailure>;
    [[nodiscard]] auto retire_hwmon_temperature(
        PairwisePreparedTemperatureSource source,
        std::span<platform_linux::detail::BoundedSysfsDisposition>
            dispositions) const noexcept -> std::size_t;
    [[nodiscard]] auto discover_cpu_frequency(
        core::LinuxCpuId cpu,
        std::span<const platform_linux::detail::CpuFrequencyPolicyView> policies,
        std::uint64_t maximum_policies, std::uint64_t maximum_memberships,
        std::uint64_t token_bytes, PairwiseBoundedTextReadOperations operations,
        platform_linux::detail::CpuFrequencyBoundedScratch& scratch,
        std::optional<int> enumeration_error = std::nullopt) const noexcept
        -> std::expected<std::optional<PairwisePreparedFrequencySource>,
                         PairwisePlatformFailure>;
    [[nodiscard]] auto discover_cpu_frequencies(
        platform_linux::detail::BoundedCpuFrequencyCapacity,
        std::span<const core::LinuxCpuId> present_cpus,
        std::span<const core::LinuxCpuId> worker_cpus,
        platform_linux::detail::BoundedSysfsOperations,
        platform_linux::detail::BoundedCpuFrequencyValidationGate gate = {}) const noexcept
        -> std::expected<std::optional<PairwisePreparedFrequencySources>,
                         PairwiseSysfsPreparationFailure>;
    [[nodiscard]] auto prepare_cpu_frequency_discovery_storage(
        platform_linux::detail::BoundedCpuFrequencyCapacity,
        std::span<const core::LinuxCpuId> present_cpus,
        std::span<const core::LinuxCpuId> worker_cpus,
        platform_linux::detail::BoundedSysfsOperations) const noexcept
        -> std::expected<PairwisePreparedCpuFrequencyDiscoveryStorage,
                         PairwiseSysfsPreparationFailure>;
    [[nodiscard]] auto discover_cpu_frequencies_prepared(
        PairwisePreparedCpuFrequencyDiscoveryStorage&&,
        platform_linux::detail::BoundedCpuFrequencyValidationGate gate = {}) const noexcept
        -> std::expected<std::optional<PairwisePreparedFrequencySources>,
                         PairwiseSysfsPreparationFailure>;
    [[nodiscard]] auto read_cpu_frequency(
        const PairwisePreparedFrequencySource& source,
        PairwiseSysfsReadWorkspace& workspace) const noexcept
        -> std::expected<std::uint64_t, PairwisePlatformFailure>;
    [[nodiscard]] auto read_cpu_frequency(
        const PairwisePreparedFrequencySources& sources, std::size_t domain,
        PairwiseSysfsReadWorkspace& workspace) const noexcept
        -> std::expected<std::uint64_t, PairwisePlatformFailure>;
    [[nodiscard]] auto retire_cpu_frequencies(
        PairwisePreparedFrequencySources sources,
        std::span<platform_linux::detail::BoundedSysfsDisposition>
            dispositions) const noexcept -> std::size_t;
    [[nodiscard]] auto prepare_perf_context_switch(
        platform_linux::LinuxThreadId thread, std::uint64_t base_page_bytes,
        std::uint64_t record_capacity) const noexcept
        -> std::expected<PairwisePreparedPerfContextSwitch,
                         PairwisePerfPreparationFailure>;
    [[nodiscard]] auto prepare_perf_context_switch_workspace() const noexcept
        -> std::expected<PairwisePerfPreparationWorkspace,
                         PairwisePlatformFailure>;
    [[nodiscard]] auto prepare_perf_context_switch_prepared(
        PairwisePerfPreparationWorkspace workspace,
        platform_linux::LinuxThreadId thread, std::uint64_t base_page_bytes,
        std::uint64_t record_capacity) const noexcept
        -> std::expected<PairwisePreparedPerfContextSwitch,
                         PairwisePerfPreparationFailure>;
    [[nodiscard]] auto begin_perf_context_switch_capture(
        PairwisePreparedPerfContextSwitch& source) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto disable_perf_context_switch_capture(
        PairwisePreparedPerfContextSwitch& source) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto verify_perf_context_switch_capture_loss(
        PairwisePreparedPerfContextSwitch& source) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto drain_perf_context_switch(
        PairwisePreparedPerfContextSwitch& source,
        PairwisePerfDrainStorage storage) const noexcept
        -> std::expected<platform_linux::PerfContextSwitchDrain,
                         PairwisePlatformFailure>;
    [[nodiscard]] auto drain_perf_context_switch_prepared(
        PairwisePreparedPerfContextSwitch& source,
        PairwisePerfDrainStorage& storage) const noexcept
        -> std::expected<PairwisePerfDrainView, PairwisePlatformFailure>;
    [[nodiscard]] auto reset_perf_context_switch_prepared(
        PairwisePreparedPerfContextSwitch& source,
        PairwisePerfDrainStorage& storage) const noexcept
        -> std::expected<void, PairwisePlatformFailure>;
    [[nodiscard]] auto retire_perf_context_switch(
        PairwisePreparedPerfContextSwitch source) const noexcept
        -> PairwisePerfRetirement;

  private:
    explicit PairwisePlatformLeafFacade(PairwisePlatformLeafDependencies dependencies) noexcept
        : dependencies_{dependencies} {}

    PairwisePlatformLeafDependencies dependencies_;
};

} // namespace epycbench::runtime::detail
