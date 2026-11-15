#pragma once

#include "epycbench/runtime/runner_context.hpp"
#include "epycbench/topology/invariants.hpp"

#include <optional>
#include "platform_linux/topology_source_io.hpp"

namespace epycbench::runtime::detail {

struct RunnerContextOperations {
    void* context;
    topology::ValidationReport (*validate)(void*, const topology::TopologySnapshot&);
    std::expected<topology::StableTopologyIds, core::Error> (*derive_ids)(
        void*, const topology::TopologySnapshot&);
    std::expected<topology::TopologyFingerprints, core::Error> (*derive_fingerprints)(
        void*, const topology::TopologySnapshot&, const topology::StableTopologyIds&);
};

struct RunnerContextFactory {
    [[nodiscard]] static auto create(topology::TopologySnapshot topology,
                                     RunnerContextOperations operations) noexcept
        -> std::expected<RunnerContext, core::Error>;
    [[nodiscard]] static auto create_fallible(topology::TopologySnapshot,
        RunnerContextOperations, std::optional<core::Error>* raw_failure = nullptr)
        -> std::expected<RunnerContext, core::Error>;
};

struct RunnerContextAcquireOperations {
    void* context;
    std::expected<topology::TopologySnapshot, core::Error> (*acquire_topology)(void*);
    RunnerContextOperations context_operations;
};

[[nodiscard]] auto acquire_runner_context_with(RunnerContextAcquireOperations operations) noexcept
    -> std::expected<RunnerContext, core::Error>;
[[nodiscard]] auto acquire_runner_context_with_fallible(RunnerContextAcquireOperations,
    std::optional<core::Error>* raw_failure = nullptr)
    -> std::expected<RunnerContext, core::Error>;
[[nodiscard]] auto acquire_runner_context_fallible(std::optional<core::Error>* raw_failure,
    platform_linux::detail::PreparedTopologySourceIO* prepared = nullptr,
    std::uint64_t original_absolute_end = 0)
    -> std::expected<RunnerContext, core::Error>;

} // namespace epycbench::runtime::detail
