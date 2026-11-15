#pragma once

#include "epycbench/core/error.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace epycbench::platform_linux::detail {
struct NumaOperations;
struct AllowedMemsWorkspace;
class NumaBindInvocationFacts;
using WordAllocator = std::vector<unsigned long> (*)(std::size_t);
}
namespace epycbench::runtime { struct PairwiseProductionHostOperations; class PairwiseProductionOperationsAdapter; }

namespace epycbench::runtime::detail {

enum class ProductionAdapterPreparationStage : std::uint8_t {
    first_memory,
    second_memory,
    family,
    worker_scopes,
    monitoring,
    destination_allocation,
    ownership_transfer
};

enum class ProductionAdapterFaultMode : std::uint8_t {
    none,
    returned_error,
    standard_exception,
    unknown_exception,
    allocation_exception
};

struct ProductionAdapterFaultScript {
    const void* context{};
    std::uint64_t invocation{};
    ProductionAdapterPreparationStage stage{};
    ProductionAdapterFaultMode mode{};
};

struct LinuxDiagnosticPreparationOverrides {
    std::uint64_t native_max{std::numeric_limits<unsigned long>::max()};
    std::size_t size_max{std::numeric_limits<std::size_t>::max()};
    platform_linux::detail::WordAllocator workspace_allocator{};
    void* observer_context{};
    void (*workspace_observer)(void*, std::size_t, bool,
                               const platform_linux::detail::AllowedMemsWorkspace*) noexcept{};
    void (*facts_observer)(void*, std::size_t, bool,
                           const platform_linux::detail::NumaBindInvocationFacts*) noexcept{};
    bool force_context_mismatch{};
    bool force_domain_too_small{};
    bool force_invocation_authority_invalid{};
    int projection_fault{};
};

struct PairwiseProductionOperationsAdapterAccess {
    static void install_fault_script(ProductionAdapterFaultScript) noexcept;
    static void clear_fault_script() noexcept;
    static void set_emergency_cleanup_complete_hook(void (*hook)() noexcept) noexcept;
    static void set_reportable_cleanup_hooks(void (*begin)() noexcept,
                                             void (*complete)() noexcept) noexcept;
    static void inject_calibration_stop_failure_for_test(std::uint32_t worker_index) noexcept;
    [[nodiscard]] static auto lower_memory_allocation_attribution_active() noexcept -> bool;
    [[nodiscard]] static auto probe_cleanup_ledger(std::size_t worker_count,
                                                   std::size_t regular_records,
                                                   bool boundary_record,
                                                   std::size_t excess_records)
        -> std::vector<core::Error>;
    [[nodiscard]] static auto probe_cleanup_force_stock() -> std::vector<core::Error>;
    [[nodiscard]] static auto probe_cleanup_order(bool monitoring_first)
        -> std::vector<core::Error>;
    [[nodiscard]] static auto create_linux(PairwiseProductionHostOperations,
                                           platform_linux::detail::NumaOperations,
                                           std::shared_ptr<void>,
                                           LinuxDiagnosticPreparationOverrides = {})
        -> PairwiseProductionOperationsAdapter;
};

} // namespace epycbench::runtime::detail
