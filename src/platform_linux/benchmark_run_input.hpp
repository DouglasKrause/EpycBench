#pragma once

#include "platform_linux/bounded_sysfs_operations.hpp"
#include "platform_linux/perf_context_switch_access.hpp"
#include "platform_linux/task_stat_operations.hpp"
#include "platform_linux/cpu_frequency_access.hpp"
#include "platform_linux/hwmon_temperature_access.hpp"
#include "epycbench/platform_linux/thread_affinity.hpp"

namespace epycbench::platform_linux::benchmark_run_input {
enum class Stage { admission, open, identity, read, close, affinity_save, affinity_set,
    cpuid, affinity_restore, kernel_identity, process_thp, perf_map, perf_metadata, perf_unmap, clock };
struct Failure {
    Stage stage{};
    int error_number{};
    std::optional<core::Error> detail;
};
struct Outcome {
    std::optional<Failure> primary;
    std::array<Failure,4> cleanup{};
    std::size_t cleanup_count{};
    [[nodiscard]] auto succeeded() const noexcept -> bool { return !primary && cleanup_count == 0; }
};
struct ObservationTime {
    void* context{};
    std::expected<void,Failure> (*record)(void*){};
};
enum class EvidenceFile { approval, gcc_machine, gcc_human, clang_machine, clang_human };
enum class SourceFile { smt_control, smt_active, numa_balancing, thp_enabled, thp_defrag, microcode };
struct TextRead { std::string bytes; Outcome outcome; };
struct FileOperations {
    detail::BoundedSysfsOperations files;
    std::uint64_t (*effective_uid)(void*) noexcept{};
};
// The caller selects and authenticates the historical identity. This layer owns
// bounded private IO only; directory is exactly a lowercase SHA-256 path token.
[[nodiscard]] auto read_evidence(std::string_view root, std::string_view directory,
    EvidenceFile, std::size_t byte_limit, FileOperations) -> TextRead;
[[nodiscard]] auto read_source(SourceFile, std::uint32_t cpu, FileOperations,ObservationTime) -> TextRead;
// Authored input alone permits a caller-selected relative/absolute path. It
// shares the bounded reader and never adopts the historical private-file policy.
[[nodiscard]] auto read_authored(std::string_view path,FileOperations) -> TextRead;
// Demo-only output setup: private cwd/epycbench-demo-results and its journal
// child. Existing directories must already be private and owned by this user.
[[nodiscard]] auto prepare_demo_directory() -> TextRead;
struct ThpSizeRow { std::uint64_t bytes{};bool attempted{};TextRead enabled; };
struct ThpInventory {
    std::vector<ThpSizeRow> rows;
    Outcome outcome;
    bool enumeration_complete{};
};

struct HostOperations {
    void* context{};
    std::expected<core::CpuSet,core::Error> (*affinity)(void*){};
    std::expected<void,core::Error> (*set_affinity)(void*,const core::CpuSet&){};
    std::expected<std::uint32_t,Failure> (*cpuid_leaf1_eax)(void*){};
    // Only these three uname fields cross the boundary; no nodename/domainname.
    std::expected<std::array<std::string,3>,Failure> (*kernel_identity)(void*){};
    std::expected<std::uint64_t,Failure> (*process_thp)(void*){};
};
struct CpuIdentity { std::optional<std::uint32_t> eax; Outcome outcome; };
[[nodiscard]] auto read_thp_inventory(FileOperations,ObservationTime) -> ThpInventory;
struct KernelIdentity {
    std::optional<std::array<std::string,3>> fields;
    Outcome outcome;
};
struct ProcessThp {
    std::optional<std::uint64_t> value;
    Outcome outcome;
};
[[nodiscard]] auto read_kernel_identity(HostOperations,ObservationTime) -> KernelIdentity;
[[nodiscard]] auto read_process_thp(HostOperations,ObservationTime) -> ProcessThp;
struct PageFaultRead {
    std::optional<LinuxTaskPageFaultCounters> counters;
    std::optional<detail::BoundedTaskStatReadFailure> source_failure;
    Outcome outcome;
};
[[nodiscard]] auto read_page_faults(LinuxThreadId,detail::BoundedTaskStatOperations,
    ObservationTime) noexcept -> PageFaultRead;
struct FrequencyDomain {
    std::size_t membership_offset{},membership_count{};
    CpuFrequencyCurrentKind kind{};
    std::string driver,governor;
    std::uint64_t minimum_hz{},maximum_hz{};
    std::optional<std::uint64_t> current_hz;
};
struct FrequencyRead {
    std::vector<FrequencyDomain> domains;
    std::vector<core::LinuxCpuId> memberships;
    std::optional<detail::BoundedCpuFrequencySourceFailureSequence> source_failure;
    std::vector<detail::BoundedCpuFrequencySourceFailure> source_cleanup;
    std::vector<detail::BoundedSysfsDisposition> retirement;
    Outcome outcome;
    std::size_t discovered_policies{},source_bytes{};
    bool evaluated{},supported{};
};
// Private fault seam can only tighten the closed production budgets.
struct SourceReadLimits {
    std::size_t bytes{16U*1024U*1024U};
    std::optional<std::size_t> calls;
};
[[nodiscard]] auto read_frequency(std::span<const core::LinuxCpuId> present,
    std::span<const core::LinuxCpuId> selected,detail::BoundedSysfsOperations,
    ObservationTime,SourceReadLimits={}) -> FrequencyRead;
struct TemperatureRead {
    std::optional<std::int64_t> millicelsius;
    std::optional<detail::BoundedHwmonSourceFailureSequence> source_failure;
    std::array<detail::BoundedHwmonSourceFailure,3> source_cleanup{};
    std::array<detail::BoundedSysfsDisposition,3> retirement{};
    std::size_t retirement_count{},source_bytes{};
    Outcome outcome;
    bool evaluated{},supported{};
};
[[nodiscard]] auto read_temperature(detail::BoundedSysfsOperations,ObservationTime,SourceReadLimits={})
    noexcept -> TemperatureRead;
[[nodiscard]] auto read_cpu_identity(core::LinuxCpuId,HostOperations,ObservationTime) -> CpuIdentity;
struct PerfCapability { Outcome outcome; bool observed{}; };
[[nodiscard]] auto check_perf(LinuxThreadId,std::size_t page_bytes,
    detail::BoundedPerfContextSwitchOperations,ObservationTime) noexcept -> PerfCapability;
[[nodiscard]] auto native_file_operations() noexcept -> FileOperations;
[[nodiscard]] auto native_host_operations() noexcept -> HostOperations;
}
