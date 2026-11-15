#pragma once

#include "platform_linux/host_work_admission.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace epycbench::platform_linux::detail {

enum class TopologySourceRoot : unsigned char { sys, proc, cgroup };
enum class TopologySourceFilesystem : unsigned char { other, sysfs, proc, cgroup, cgroup2, tmpfs };
enum class TopologySourceNativePath : unsigned char { sys, proc, cgroup_mount, runtime_hwdata };
enum class TopologySourceOperation : unsigned char {
    state, reserve, allocation, identity, guard, open, stat, filesystem, readlink,
    enumerate, read, close, path, conversion
};
enum class TopologySourceReason : unsigned char {
    invalid_input, capacity, arithmetic, allocation, native, identity, path,
    missing, malformed, clock, deadline, cancelled, uncertain, invalid_state
};
struct TopologySourceFailure final {
    TopologySourceReason reason{};
    TopologySourceOperation operation{};
    long native_value{};
    int native_error{};
    TopologySourceRoot root{};
    std::array<char, 4096> relative{};
};
struct TopologySourceCloseFact final {
    unsigned char slot{};
    int descriptor{-1};
    long native_value{};
    int native_error{};
};
// Work allowances, not descriptor/storage authority. Zero/missing preparation
// refuses; callers bind these independently of post-RunnerContext capacity.
struct TopologySourceLimits final {
    std::uint64_t acquisitions{}, io_calls{}, guard_checks{}, conversion_work{};
    std::uint64_t path_bytes{}, component_bytes{}, path_depth{}, link_expansions{};
    std::uint64_t file_bytes{}, total_text_bytes{}, entries_per_directory{}, total_entries{}, refills{};
    std::uint64_t online_cpus{}, enumerated_cpus{}, cache_entries_per_cpu{}, cache_entries{};
    std::uint64_t online_nodes{}, pci_entries{}, memberships{}, bitmap_groups{};
    std::uint32_t maximum_cpu_index{}, maximum_node_index{}, maximum_cache_index{};
    std::uint64_t cluster_work{}, distance_values{};
};
[[nodiscard]] auto topology_source_limits_valid(const TopologySourceLimits&) noexcept -> bool;
enum class TopologySourceCount : unsigned char {
    online_cpus, enumerated_cpus, cache_entries_per_cpu, cache_entries, online_nodes,
    pci_entries, memberships, bitmap_groups, cpu_index, node_index, cache_index,
    cluster_work, distance_values
};
struct TopologySourceOperations final {
    void* context{};
    RawStatusResult (*open_root)(void*, const char*, int) noexcept{};
    RawStatusResult (*open_at)(void*, int, const char*, int) noexcept{};
    RawStatusResult (*stat_fd)(void*, int, struct stat*) noexcept{};
    RawStatusResult (*stat_at)(void*, int, const char*, struct stat*) noexcept{};
    RawStatusResult (*filesystem)(void*, int, TopologySourceFilesystem*) noexcept{};
    RawStatusResult (*readlink)(void*, int, char*, std::size_t) noexcept{};
    RawStatusResult (*entries)(void*, int, std::span<char>) noexcept{};
    RawStatusResult (*read)(void*, int, std::span<char>) noexcept{};
    RawStatusResult (*close)(void*, int) noexcept{};
    RawStatusResult (*pid)(void*) noexcept{};
    RawStatusResult (*tid)(void*) noexcept{};
};
// Every callback is one attempt. EINTR, dots, ignored entries, refill and EOF
// are returned/charged by this purpose-specific owner, not hidden by a leaf.
[[nodiscard]] auto production_topology_source_operations() noexcept -> TopologySourceOperations;

class PreparedTopologySourceIO final {
  public:
    PreparedTopologySourceIO(const PreparedTopologySourceIO&) = delete;
    PreparedTopologySourceIO(PreparedTopologySourceIO&&) noexcept;
    auto operator=(const PreparedTopologySourceIO&) -> PreparedTopologySourceIO& = delete;
    auto operator=(PreparedTopologySourceIO&&) -> PreparedTopologySourceIO& = delete;
    ~PreparedTopologySourceIO() noexcept;
    [[nodiscard]] static auto prepare(host_work::ResourceLedger&, TopologySourceLimits,
        TopologySourceOperations, host_work::GuardOperations,
        std::array<std::string_view, 3> roots, std::uint64_t original_main) noexcept
        -> std::expected<PreparedTopologySourceIO, TopologySourceFailure>;
    [[nodiscard]] auto begin(std::uint64_t original_absolute_end) noexcept
        -> std::expected<void, TopologySourceFailure>;
    // Nonallocating pre-hwloc admission of the actual prepared owner. It does
    // not start an invocation, renew counters, or acquire a native resource.
    [[nodiscard]] auto invocation_ready() const noexcept
        -> std::expected<void, TopologySourceFailure>;
    // All six consuming close attempts occur even after guard/capacity failure.
    [[nodiscard]] auto finish() noexcept -> bool;
    [[nodiscard]] auto read(TopologySourceRoot, std::string_view relative, bool required)
        -> std::expected<std::optional<std::string>, TopologySourceFailure>;
    [[nodiscard]] auto entries(TopologySourceRoot, std::string_view relative)
        -> std::expected<std::vector<std::string>, TopologySourceFailure>;
    [[nodiscard]] auto directory(TopologySourceRoot, std::string_view relative,
        bool forbid_final_link) noexcept -> std::expected<void, TopologySourceFailure>;
    // The destination is the already reserved persistent native invocation cell.
    // A successful FD is published there before lower ownership is cleared and
    // before any later close/check. Failure may therefore still own destination.
    // These primitives do not grant source/artifact/runtime authority.
    [[nodiscard]] auto native_open(TopologySourceNativePath, std::string_view absolute,
        bool directory, bool required, int& destination,
        std::span<const int> native_owned = {}) noexcept
        -> std::expected<bool, TopologySourceFailure>;
    [[nodiscard]] auto count(TopologySourceCount, std::uint64_t) noexcept
        -> std::expected<void, TopologySourceFailure>;
    [[nodiscard]] auto cluster_work_for(std::uint64_t cpus, std::uint64_t maximum_membership) noexcept
        -> std::expected<void, TopologySourceFailure>;
    [[nodiscard]] auto distance_values_for(std::uint64_t nodes) noexcept
        -> std::expected<void, TopologySourceFailure>;
    [[nodiscard]] auto reject(TopologySourceReason) noexcept
        -> std::expected<void, TopologySourceFailure>;
    [[nodiscard]] auto work(std::uint64_t = 1) noexcept -> std::expected<void, TopologySourceFailure>;
    [[nodiscard]] auto limits() const noexcept -> const TopologySourceLimits&;
    [[nodiscard]] auto failure() const noexcept -> const TopologySourceFailure*;
    [[nodiscard]] auto cleanup() const noexcept -> std::span<const TopologySourceCloseFact>;
    [[nodiscard]] auto uncertain() const noexcept -> bool;
    [[nodiscard]] auto active() const noexcept -> bool;
  private:
    struct State;
    PreparedTopologySourceIO(std::unique_ptr<State>, host_work::ResourceLease) noexcept;
    std::unique_ptr<State> state_;
    host_work::ResourceLease lease_;
};

} // namespace epycbench::platform_linux::detail
