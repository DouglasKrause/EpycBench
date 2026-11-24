#pragma once

#include "epycbench/core/id.hpp"
#include "kernel/package_input_policy.hpp"
#include "measurement/package_control.hpp"
#include "platform_linux/package_execution.hpp"

#include <array>

namespace epycbench::platform_linux::package_execution {
class PackageMonitor;
enum class Composition { sequential, mixed, pipeline };
using kernel::packages::InputPolicy;

struct InputPlacement {
    InputPolicy policy{InputPolicy::bind};
    std::vector<core::LinuxNumaNodeId> targets;
    std::string replica_group;
};

struct StageLink {
    std::string output;
    std::string input;
};

struct Role {
    std::string id;
    Module module;
    Request request;
    std::vector<core::LinuxCpuId> cpus;
    core::LinuxNumaNodeId node;
    std::vector<StageLink> links;
    InputPlacement input_placement{};
};

struct NodeResidency {
    core::LinuxNumaNodeId node{0};
    std::uint64_t before{}, after{};
    auto operator==(const NodeResidency&) const noexcept -> bool = default;
};

struct RegionPlacement {
    std::string name;
    std::uint64_t pages{};
    std::uint64_t resident_before{};
    std::uint64_t resident_after{};
    core::LinuxNumaNodeId node{0};
    InputPolicy policy{InputPolicy::bind};
    std::vector<core::LinuxNumaNodeId> targets{};
    std::vector<NodeResidency> distribution{};
    std::uint64_t unresolved_before{}, unresolved_after{};
    std::optional<std::uint32_t> owner_rank{};
    std::optional<bool> released{};
};

struct Placement {
    core::LinuxCpuId requested_cpu{0};
    core::LinuxNumaNodeId requested_node{0};
    std::optional<core::LinuxCpuId> cpu_before;
    std::optional<core::LinuxCpuId> cpu_after;
    std::uint64_t page_bytes{};
    std::vector<RegionPlacement> regions;
    bool verified{};
};

struct StageTransfer {
    std::string source_role;
    std::string output;
    std::string input;
    std::uint64_t bytes;
    core::Sha256Digest sha256;
};

struct RankTrial {
    std::string role;
    std::uint32_t rank{};
    std::uint32_t rank_count{};
    std::optional<Trial> trial;
    Placement placement;
    std::vector<StageTransfer> transfers;
    std::optional<core::Error> error;
    std::optional<core::Error> cleanup_error{};
    nlohmann::json monitoring = nullptr;

    [[nodiscard]] auto valid() const noexcept -> bool;
};

struct WarmupRank {
    std::uint32_t rank{};
    std::optional<Trial> trial;
    std::optional<core::Error> error;
};

struct WarmupRole {
    std::string role;
    measurement::package_control::WarmupStatus status{measurement::package_control::WarmupStatus::continue_trials};
    std::vector<std::vector<WarmupRank>> attempts{};
};

struct WarmupInterval {
    std::uint64_t begin_ns{}, end_ns{};
};

struct InputRegionProof {
    std::string name;
    std::uint64_t bytes;
    core::Sha256Digest sha256;
};

struct RoleInputProof {
    std::string role;
    std::vector<InputRegionProof> regions{};
    std::optional<core::Error> error{};
};

struct Group {
    bool supplied{};
    Composition composition{Composition::sequential};
    std::optional<std::uint64_t> overlap_begin_ns;
    std::optional<std::uint64_t> overlap_end_ns;
    std::vector<RankTrial> ranks;
    std::vector<CollectiveEvidence> collectives;
    struct ReplicaRegion {
        std::string name;
        std::uint64_t bytes;
        core::Sha256Digest content_sha256;
    };
    struct ReplicaOwner {
        std::string role;
        std::uint32_t rank;
        core::LinuxNumaNodeId node;
    };
    struct ReplicaEvidence {
        std::string group;
        InputPolicy policy;
        std::vector<ReplicaOwner> owners;
        std::vector<ReplicaRegion> regions;
        std::optional<core::Error> error;
    };
    std::vector<ReplicaEvidence> replicas{};
    std::vector<WarmupRole> warmup{};
    std::vector<RoleInputProof> input_proofs{};

    [[nodiscard]] auto valid() const noexcept -> bool;
};

struct WorkerServices {
    ResourceProvider resources;
    Clock clock;
    Placement placement;
    // Optional lifetime for callback contexts kept until this rank retires.
    std::shared_ptr<void> owner;
    Clock collective_clock{};
    std::span<const WarmupInterval> warmup_intervals{};
    const nlohmann::json* monitoring{};
    const nlohmann::json* finalization_monitoring{};
};

struct WorkerProviders {
    void* context{};
    std::expected<WorkerServices, core::Error> (*supplied)(void*, const Role&, std::uint32_t){};
    // Zero selects the supplied-memory cap or the native address-space limit
    // combined with the plan-specific physical/NUMA headroom check.
    std::uint64_t maximum_region_bytes{};
    std::optional<measurement::package_control::WarmupSettings> warmup{};
    // Immutable rank geometry from the admitted plan, in authored role order.
    const nlohmann::json* admitted_geometry{};
    PackageMonitor* monitor{};
};

struct SuppliedRank {
    std::string role;
    std::uint32_t rank{};
    std::uint64_t begin_ns{}, end_ns{};
    Placement placement;
    struct FinalizationTimes {
        std::uint64_t release_ns{}, scan_end_ns{}, begin_ns{}, end_ns{};
        nlohmann::json monitoring = nullptr;
    };
    std::optional<FinalizationTimes> finalization{};
    std::vector<WarmupInterval> warmup{};
    nlohmann::json monitoring = nullptr;
};

struct StreamChunk {
    std::array<RankTrial,2> ranks;
    std::optional<std::uint64_t> handoff_ns;
    bool producer_waited{};
    bool consumer_waited{};
};

struct StreamEvidence {
    struct Handoff {
        std::uint64_t bytes;
        core::Sha256Digest reference, observed;
    };
    std::optional<std::uint64_t> release_ns, completion_ns;
    std::size_t chunks_per_pass{};
    std::vector<StreamChunk> chunks;
    std::array<nlohmann::json, 2> external_inputs{};
    std::optional<Handoff> logical_handoff;
    std::optional<core::Error> error;
    [[nodiscard]] auto valid() const noexcept -> bool;
};

struct SuppliedStream {
    std::uint64_t release_ns{}, completion_ns{};
    std::vector<std::vector<SuppliedRank>> chunks;
    std::vector<std::uint64_t> handoff_ns;
};

[[nodiscard]] auto inspect_stream_layouts(std::span<const Role>, std::size_t chunks, std::uint64_t page_bytes,
                                          std::optional<std::size_t>* short_partition_stage = nullptr)
    -> std::expected<nlohmann::json,core::Error>;
[[nodiscard]] auto run_stream(std::span<const Role>,std::size_t chunks,const SuppliedStream* = nullptr,
                              const nlohmann::json* admitted_geometry = nullptr,
                              PackageMonitor* monitor = nullptr)
    -> std::expected<StreamEvidence,core::Error>;

// Each fixture states an interval and observed placement for one role and rank.
// The caller keeps this vector alive until run_workers returns.
[[nodiscard]] auto supplied_workers(std::vector<SuppliedRank>&) noexcept -> WorkerProviders;

// Resolve task-declared geometry and pipeline bindings without reserving buffers
// or entering measurement. The optional page size applies native mapping charges.
[[nodiscard]] auto inspect_layouts(std::span<const Role>, Composition, std::uint64_t page_bytes = 0)
    -> std::expected<nlohmann::json, core::Error>;

// The caller admits the roles against its selected topology. Workers pin and
// verify their native logical CPUs, bind and observe their timed memory regions,
// run simultaneous callbacks in mixed mode, and complete each pipeline stage
// before its successor observes the verified predecessor output at that rank.
[[nodiscard]] auto run_workers(std::span<const Role>, Composition, const WorkerProviders& = {})
    -> std::expected<Group, core::Error>;
} // namespace epycbench::platform_linux::package_execution
