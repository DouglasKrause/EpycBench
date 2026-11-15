#pragma once

#include "epycbench/kernel/region_initialization.hpp"
#include "epycbench/memory/placement_diagnostic.hpp"
#include "epycbench/memory/region_finalization.hpp"
#include "epycbench/memory/region_finalization_diagnostic.hpp"
#include "epycbench/memory/retained_region_cleanup.hpp"
#include "epycbench/runtime/pairwise_evidence_failure.hpp"
#include "runtime/pairwise_kernel_leaf_facade.hpp"
#include "runtime/pairwise_platform_leaf_facade.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace epycbench::runtime::detail {

enum class PairwiseMemoryOwningOperation : std::uint8_t {
    exact_ratio,
    request,
    mapping,
    binding,
    initialization,
    placement_query,
    placement_evaluation,
    protection,
    finalization,
    retained_cleanup,
};

struct PairwiseMemoryOwningHooks final {
    void* context{};
    void (*before_operation)(void*, PairwiseMemoryOwningOperation){};
    void (*before_node_count_insert)(void*, bool post_run){};
    void (*adjust_observation)(void*, bool post_run, std::uint64_t& total_pages,
                               std::uint64_t& resident_pages,
                               std::uint64_t& unresolved_pages) noexcept{};
    void (*after_mapping)(void*, platform_linux::AnonymousMapping&) noexcept{};
    void (*after_binding)(void*, platform_linux::AnonymousMapping&) noexcept{};
    void (*after_protection)(void*, platform_linux::AnonymousMapping&) noexcept{};
    bool (*write_prefault_byte)(void*, std::span<std::byte>, std::size_t,
                                std::byte){};
};

class PairwisePreparedMemoryMask final {
  public:
    PairwisePreparedMemoryMask(const PairwisePreparedMemoryMask&) = delete;
    auto operator=(const PairwisePreparedMemoryMask&) -> PairwisePreparedMemoryMask& = delete;
    PairwisePreparedMemoryMask(PairwisePreparedMemoryMask&&) noexcept = default;
    auto operator=(PairwisePreparedMemoryMask&&)
        -> PairwisePreparedMemoryMask& = delete;
    ~PairwisePreparedMemoryMask() noexcept = default;

  private:
    PairwisePreparedMemoryMask(
        platform_linux::NativeNumaMask mask,
        platform_linux::detail::NumaDomainMaxNode domain,
        platform_linux::detail::NumaBindInvocationFacts facts) noexcept
        : mask_{std::move(mask)}, domain_{domain}, facts_{facts} {}
    platform_linux::NativeNumaMask mask_;
    platform_linux::detail::NumaDomainMaxNode domain_;
    platform_linux::detail::NumaBindInvocationFacts facts_;
    friend class PairwiseMemoryOwningFacade;
    friend class PairwiseMemoryWorkspace;
};

class PairwiseMemoryWorkspace final {
  public:
    PairwiseMemoryWorkspace(const PairwiseMemoryWorkspace&) = delete;
    auto operator=(const PairwiseMemoryWorkspace&) -> PairwiseMemoryWorkspace& = delete;
    PairwiseMemoryWorkspace(PairwiseMemoryWorkspace&&) noexcept = default;
    auto operator=(PairwiseMemoryWorkspace&&)
        -> PairwiseMemoryWorkspace& = delete;
    ~PairwiseMemoryWorkspace() noexcept = default;

    [[nodiscard]] auto total_pages() const noexcept -> std::uint64_t { return total_pages_; }
    [[nodiscard]] auto batch_capacity() const noexcept -> std::size_t {
        return query_buffers_.capacity();
    }
    [[nodiscard]] auto node_capacity() const noexcept -> std::size_t {
        return node_capacity_;
    }

  private:
    static constexpr std::size_t query_batch_pages = 4096U;
    PairwiseMemoryWorkspace(PairwisePreparedMemoryMask prepared,
                            PairwiseNumaBindWorkspace bind_workspace,
                            PairwisePreparedNumaQueryBuffers query_buffers,
                            std::vector<memory::LinuxNodePageCount> pre_counts,
                            std::vector<memory::LinuxNodePageCount> post_counts,
                            std::uint64_t total_pages,
                            std::size_t node_capacity) noexcept
        : prepared_{std::move(prepared)}, bind_workspace_{std::move(bind_workspace)},
          query_buffers_{std::move(query_buffers)}, pre_counts_{std::move(pre_counts)},
          post_counts_{std::move(post_counts)}, total_pages_{total_pages},
          node_capacity_{node_capacity} {}

    PairwisePreparedMemoryMask prepared_;
    PairwiseNumaBindWorkspace bind_workspace_;
    PairwisePreparedNumaQueryBuffers query_buffers_;
    std::array<void*, query_batch_pages> page_addresses_{};
    std::vector<memory::LinuxNodePageCount> pre_counts_;
    std::vector<memory::LinuxNodePageCount> post_counts_;
    std::uint64_t total_pages_{};
    std::size_t node_capacity_{};
    bool pre_available_{true};
    bool post_available_{true};
    friend class PairwiseMemoryOwningFacade;
};

class PairwiseMemoryFailureSequence final {
  public:
    [[nodiscard]] auto append(PairwiseEvidenceFailure failure) noexcept -> bool;
    [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
    [[nodiscard]] auto operator[](std::size_t index) const noexcept
        -> const PairwiseEvidenceFailure&;
  private:
    std::array<std::optional<PairwiseEvidenceFailure>, 3> values_{};
    std::size_t size_{};
};

struct PairwiseMemoryMappingFailure final {
    PairwiseMemoryFailure primary;
    std::optional<PairwiseMemoryFailure> cleanup;
    std::optional<platform_linux::AnonymousMapping> retained_owner;
};

template <class Owner, class Failure = PairwiseMemoryFailure>
struct PairwiseOwnedMemoryFailure final {
    Failure primary;
    Owner owner;
};

using PairwiseMemoryInitializationPrimary =
    std::variant<PairwiseMemoryFailure, PairwiseKernelFailure>;
using PairwiseMemoryBindingFailure = PairwiseOwnedMemoryFailure<memory::RegionMapping>;
using PairwiseMemoryInitializationFailure =
    PairwiseOwnedMemoryFailure<memory::BoundRegion, PairwiseMemoryInitializationPrimary>;
using PairwiseMemoryQueryFailure = PairwiseOwnedMemoryFailure<memory::InitializedRegion>;
using PairwiseMemoryEvaluationFailure =
    PairwiseOwnedMemoryFailure<memory::PlacementObservedRegion>;
using PairwiseMemoryProtectionFailure =
    PairwiseOwnedMemoryFailure<memory::PlacementEvaluatedRegion>;

using PairwiseMemoryFinalizationOwner =
    std::variant<memory::ReadOnlyRegion, memory::PlacementEvaluatedRegion>;
struct PairwiseMemoryFinalizationFailure final {
    PairwiseMemoryFailure primary;
    std::optional<PairwiseMemoryFailure> cleanup;
    PairwiseMemoryFinalizationOwner owner;
    std::optional<memory::PostRunPlacementEvidence> post_run;
    memory::RegionCleanupEvidence cleanup_evidence;
    memory::RegionFinalizationPath path;
};

struct PairwiseProjectedMemoryFinalizationFailure final {
    memory::RegionFinalizationDiagnostic diagnostic;
    PairwiseMemoryFailure primary;
    std::optional<PairwiseMemoryFailure> cleanup;
};

[[nodiscard]] auto project_pairwise_memory_finalization_failure(
    PairwiseMemoryFinalizationFailure) noexcept
    -> PairwiseProjectedMemoryFinalizationFailure;

template <class Owner>
struct PairwiseMemoryCleanupFailure final {
    PairwiseMemoryFailure primary;
    memory::RegionCleanupEvidence evidence;
    memory::RetainedRegionSourceState source;
    Owner owner;
};

class PairwiseMemoryOwningFacade final {
  public:
    PairwiseMemoryOwningFacade(PairwisePlatformLeafFacade platform,
                               PairwiseKernelLeafFacade kernel,
                               PairwiseMemoryOwningHooks hooks = {}) noexcept
        : platform_{std::move(platform)}, kernel_{std::move(kernel)}, hooks_{hooks} {}

    [[nodiscard]] auto exact_ratio(std::uint64_t numerator,
                                   std::uint64_t denominator) const noexcept
        -> std::expected<memory::ExactRatio, PairwiseMemoryFailure>;
    [[nodiscard]] auto request(memory::RegionRequestInput input) const noexcept
        -> std::expected<memory::RegionRequest, PairwiseMemoryFailure>;
    [[nodiscard]] auto prepare_mask(
        const memory::RegionRequest& request,
        platform_linux::detail::NumaDomainMaxNode domain,
        platform_linux::detail::NumaBindInvocationFacts facts) const noexcept
        -> std::expected<PairwisePreparedMemoryMask, PairwiseMemoryFailure>;
    [[nodiscard]] auto prepare_workspace(PairwisePreparedMemoryMask prepared,
                                         std::uint64_t total_pages) const noexcept
        -> std::expected<PairwiseMemoryWorkspace, PairwiseRuntimeFailure>;
    [[nodiscard]] auto map(memory::RegionRequest request) const noexcept
        -> std::expected<memory::RegionMapping, PairwiseMemoryMappingFailure>;
    [[nodiscard]] auto bind(memory::RegionMapping mapping,
                            PairwiseMemoryWorkspace& workspace) const noexcept
        -> std::expected<memory::BoundRegion, PairwiseMemoryBindingFailure>;
    [[nodiscard]] auto initialize(
        memory::BoundRegion region,
        const PairwisePreparedRegionInitializer* initializer) const noexcept
        -> std::expected<memory::InitializedRegion,
                         PairwiseMemoryInitializationFailure>;
    [[nodiscard]] auto observe(memory::InitializedRegion region,
                               PairwiseMemoryWorkspace& workspace) const noexcept
        -> std::expected<memory::PlacementObservedRegion, PairwiseMemoryQueryFailure>;
    [[nodiscard]] auto evaluate(memory::PlacementObservedRegion region) const noexcept
        -> std::expected<memory::PlacementEvaluatedRegion,
                         PairwiseMemoryEvaluationFailure>;
    [[nodiscard]] auto protect(memory::PlacementEvaluatedRegion region) const noexcept
        -> std::expected<memory::ReadOnlyRegion, PairwiseMemoryProtectionFailure>;
    [[nodiscard]] auto finalize(
        memory::ReadOnlyRegion region,
        memory::PostRunVerificationApplicability applicability,
        PairwiseMemoryWorkspace& workspace) const noexcept
        -> std::expected<memory::FinalizedRegion,
                         PairwiseMemoryFinalizationFailure>;
    [[nodiscard]] auto finalize_unprotected(
        memory::PlacementEvaluatedRegion region,
        memory::UnprotectedFinalizationReason reason) const noexcept
        -> std::expected<memory::FinalizedRegion,
                         PairwiseMemoryFinalizationFailure>;
    [[nodiscard]] auto cleanup(memory::RegionMapping region) const noexcept
        -> std::expected<memory::CleanedRetainedRegion,
                         PairwiseMemoryCleanupFailure<memory::RegionMapping>>;
    [[nodiscard]] auto cleanup(memory::BoundRegion region) const noexcept
        -> std::expected<memory::CleanedRetainedRegion,
                         PairwiseMemoryCleanupFailure<memory::BoundRegion>>;
    [[nodiscard]] auto cleanup(memory::InitializedRegion region) const noexcept
        -> std::expected<memory::CleanedRetainedRegion,
                         PairwiseMemoryCleanupFailure<memory::InitializedRegion>>;
    [[nodiscard]] auto cleanup(memory::PlacementObservedRegion region) const noexcept
        -> std::expected<
            memory::CleanedRetainedRegion,
            PairwiseMemoryCleanupFailure<memory::PlacementObservedRegion>>;

  private:
    [[nodiscard]] auto query_observation(
        const memory::InitializedRegion& initialized,
        PairwiseMemoryWorkspace& workspace, bool post_run) const noexcept
        -> std::expected<memory::RegionPlacementObservation, PairwiseMemoryFailure>;
    [[nodiscard]] auto finish_finalization(
        PairwiseMemoryFinalizationOwner owner,
        std::optional<memory::PostRunPlacementEvidence> evidence,
        memory::RegionFinalizationPath path,
        std::optional<PairwiseMemoryFailure> primary) const noexcept
        -> std::expected<memory::FinalizedRegion,
                         PairwiseMemoryFinalizationFailure>;

    PairwisePlatformLeafFacade platform_;
    PairwiseKernelLeafFacade kernel_;
    PairwiseMemoryOwningHooks hooks_;
};

} // namespace epycbench::runtime::detail
