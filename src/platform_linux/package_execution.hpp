#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/kernel/package.h"
#include "platform_linux/package_worker_counters.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::kernel::packages {
struct ImplementationManifest;
}

namespace epycbench::platform_linux::package_execution {
inline constexpr std::uint64_t default_region_budget = 256U * 1024U * 1024U;

struct InspectionContext {
    std::string workload, worker, l1_cache;
    std::uint64_t base_page_bytes{}, l1_capacity_bytes{}, l1_line_bytes{}, l1_ways{};
};

struct Request {
    nlohmann::json parameters = nlohmann::json::object();
    nlohmann::json tuning = nlohmann::json::object();
    std::uint64_t seed{};
    std::uint64_t repetitions{1};
    std::uint32_t rank{};
    std::uint32_t rank_count{1};
    std::uint64_t partition_begin{};
    std::uint64_t partition_count{};
    std::uint64_t block{};
    bool hardware_counters{};
    bool supplied_workers{};
    std::vector<EpycBenchPackageLocalityV1> locality{};
    std::vector<InspectionContext> inspection{};
};

struct AllocationRequest {
    std::string_view name;
    std::uint32_t kind{};
    std::size_t bytes{};
    std::size_t alignment{};
    std::uint32_t rank{};
    bool reference_output{};
};

struct Allocation {
    std::shared_ptr<void> owner;
    std::span<std::byte> bytes;
};

// A custom provider can bind the allocation to the runtime's placement plan.
// It must provide disjoint, aligned regions and an owner valid for every view.
struct ResourceProvider {
    void* context{};
    std::expected<Allocation, core::Error> (*allocate)(void*, const AllocationRequest&){};
};

struct Clock {
    void* context{};
    std::expected<std::uint64_t, core::Error> (*now)(void*){};
};

struct RankWindow {
    std::optional<std::uint64_t> begin_ns, end_ns;
    EpycBenchPackageRankReceiptV1 receipt{};
};

struct FinishGate {
    void* context{};
    void (*wait)(void*, const RankWindow&) noexcept{};
};

struct CollectiveCall {
    std::span<const EpycBenchPackageWriteRegionV1> shared;
    std::span<const EpycBenchPackageReadRegionV1> shared_inputs;
    EpycBenchPackageCancellationV1 cancellation{};
    void (*cancel)(void*) noexcept{};
};

struct InputBinding {
    std::string_view name;
    std::span<const std::byte> bytes;
};

struct BorrowedInput {
    std::string_view name;
    std::shared_ptr<void> owner;
    std::span<const std::byte> bytes;
};

struct RegionBindings {
    std::span<const InputBinding> inputs;
    std::span<const std::string_view> export_outputs;
    const EpycBenchPackageLayoutV1* expected_layout{};
    std::span<const BorrowedInput> borrowed_inputs{};
};

struct ExportedOutput {
    std::string name;
    std::shared_ptr<void> owner;
    std::span<const std::byte> bytes;
};

struct RegionEvidence {
    std::string name;
    std::uint32_t kind{};
    std::uint64_t bytes{};
    std::uint64_t alignment{};
    std::optional<core::Sha256Digest> before;
    std::optional<core::Sha256Digest> after;
    std::optional<core::Sha256Digest> reference;
};

struct Trial {
    std::optional<std::uint64_t> begin_ns;
    std::optional<std::uint64_t> end_ns;
    EpycBenchPackageWorkV1 expected{};
    EpycBenchPackageWorkV1 actual{};
    int execute_status{};
    int verify_status{};
    bool input_unchanged{};
    bool output_matches{};
    bool work_matches{};
    std::optional<core::Error> timing_error;
    std::vector<RegionEvidence> regions;
    std::vector<ExportedOutput> outputs;
    std::optional<CounterEvidence> hardware_counters;
    std::uint64_t partition_begin{};
    std::uint64_t partition_count{};

    [[nodiscard]] auto valid() const noexcept -> bool;
};

class PreparedTrial;
class PreparedCollective;

struct FinalizationEvidence {
    std::optional<std::uint64_t> release_ns, scan_end_ns, begin_ns, end_ns;
    int execute_status{};
    EpycBenchPackageWorkV1 actual{};
    std::optional<core::Error> error;
    nlohmann::json monitoring = nullptr;
};

struct CollectiveEvidence {
    std::string role;
    std::uint32_t owner_rank{};
    std::optional<std::uint64_t> begin_ns, end_ns;
    std::optional<std::uint64_t> overlap_begin_ns, overlap_end_ns;
    bool cancelled{};
    int verify_status{};
    EpycBenchPackageWorkV1 expected{};
    EpycBenchPackageWorkV1 actual{};
    std::vector<RegionEvidence> regions;
    std::optional<core::Error> error;
    std::optional<FinalizationEvidence> finalization;
    std::optional<nlohmann::json> task_observation;

    [[nodiscard]] auto valid() const noexcept -> bool;
};

struct MappingCallbacks {
    const EpycBenchPackageMappingTaskV1* task{};
    const EpycBenchPackageMappingImplementationV1* implementation{};
};

class Module {
  public:
    [[nodiscard]] static auto load(const kernel::packages::ImplementationManifest&)
        -> std::expected<Module, core::Error>;
    [[nodiscard]] auto collective() const noexcept -> bool;
    [[nodiscard]] auto collective_finalizer() const noexcept -> bool;
    [[nodiscard]] auto shared_cpu_local() const noexcept -> bool;
    [[nodiscard]] auto per_worker_input() const noexcept -> bool;
    [[nodiscard]] auto mapping() const noexcept -> MappingCallbacks;
    [[nodiscard]] auto control_inputs() const noexcept -> std::span<const std::string>;
    [[nodiscard]] auto partition_extent_parameters() const noexcept -> std::span<const std::string>;

  private:
    struct State;
    explicit Module(std::shared_ptr<const State> state) noexcept : state_{std::move(state)} {}
    std::shared_ptr<const State> state_;

    friend class PreparedTrial;
    friend class PreparedCollective;
    friend auto layout(const Module&, const Request&, std::uint64_t)
        -> std::expected<EpycBenchPackageLayoutV1, core::Error>;
    friend auto prepare(const Module&, const Request&, ResourceProvider, std::uint64_t, RegionBindings)
        -> std::expected<PreparedTrial, core::Error>;
    friend auto collective_layout(const Module&, const Request&, std::uint64_t)
        -> std::expected<EpycBenchPackageLayoutV1, core::Error>;
    friend auto inspect_task(const Module&, const Request&, const EpycBenchPackageLayoutV1&)
        -> std::expected<std::optional<nlohmann::json>, core::Error>;
    friend auto prepare_collective(const Module&, const Request&, const EpycBenchPackageLayoutV1&,
                                   ResourceProvider, std::uint64_t) -> std::expected<PreparedCollective, core::Error>;
};

class PreparedTrial {
  public:
    PreparedTrial(PreparedTrial&&) noexcept;
    auto operator=(PreparedTrial&&) noexcept -> PreparedTrial&;
    ~PreparedTrial();
    [[nodiscard]] auto collective_input() const noexcept -> EpycBenchPackageCollectiveInputV1;
    [[nodiscard]] auto immutable_inputs() const noexcept -> std::span<const BorrowedInput>;
    void reset_warmup() noexcept;
    [[nodiscard]] auto measure_warmup(Clock, FinishGate) -> std::expected<Trial, core::Error>;
    [[nodiscard]] auto stream_reference(std::string_view output) const noexcept -> std::span<const std::byte>;
    [[nodiscard]] auto stream_input(std::string_view input) noexcept -> std::span<std::byte>;
    struct StreamCallback {
        std::optional<std::uint64_t> begin_ns, end_ns;
        EpycBenchPackageWorkV1 actual{};
        int status{};
        std::optional<core::Error> error;
        [[nodiscard]] auto completed() const noexcept -> bool;
    };
    [[nodiscard]] auto invoke_stream(Clock) -> StreamCallback;
    [[nodiscard]] auto retire_stream(StreamCallback, std::string_view reused_output = {},
                                     std::span<const std::byte> handed_off = {}) && -> Trial;

  private:
    struct State;
    [[nodiscard]] static auto measure_state(State*, Clock, FinishGate, CollectiveCall, bool warmup)
        -> std::expected<Trial, core::Error>;
    explicit PreparedTrial(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;

    friend auto prepare(const Module&, const Request&, ResourceProvider, std::uint64_t, RegionBindings)
        -> std::expected<PreparedTrial, core::Error>;
    friend auto measure(PreparedTrial&&, Clock, FinishGate, CollectiveCall) -> std::expected<Trial, core::Error>;
};

[[nodiscard]] auto layout(const Module&, const Request&,
                          std::uint64_t maximum_region_bytes = default_region_budget)
    -> std::expected<EpycBenchPackageLayoutV1, core::Error>;

[[nodiscard]] auto inspect_task(const Module&, const Request&, const EpycBenchPackageLayoutV1&)
    -> std::expected<std::optional<nlohmann::json>, core::Error>;

[[nodiscard]] auto prepare(const Module&, const Request&, ResourceProvider = {},
                           std::uint64_t maximum_region_bytes = default_region_budget,
                           RegionBindings = {})
    -> std::expected<PreparedTrial, core::Error>;

// Resources and expected results are prepared before this one-shot operation.
// A run with a timed callback always returns its verification record, including
// a failed end clock, callback, work report, or output verification.
[[nodiscard]] auto measure(PreparedTrial&&, Clock = {}, FinishGate = {}, CollectiveCall = {})
    -> std::expected<Trial, core::Error>;

class PreparedCollective {
  public:
    PreparedCollective(PreparedCollective&&) noexcept;
    auto operator=(PreparedCollective&&) noexcept -> PreparedCollective&;
    ~PreparedCollective();

    [[nodiscard]] auto reference(std::span<const EpycBenchPackageCollectiveInputV1>)
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto call() const noexcept -> CollectiveCall;
    void finalize(std::span<const EpycBenchPackageRankReceiptV1>, Clock, FinalizationEvidence&) const;
    void verify(std::span<const EpycBenchPackageRankReceiptV1>, CollectiveEvidence&) const;

  private:
    struct State;
    explicit PreparedCollective(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;

    friend auto prepare_collective(const Module&, const Request&, const EpycBenchPackageLayoutV1&,
                                   ResourceProvider, std::uint64_t) -> std::expected<PreparedCollective, core::Error>;
};

[[nodiscard]] auto collective_layout(const Module&, const Request&,
                                     std::uint64_t maximum_region_bytes = default_region_budget)
    -> std::expected<EpycBenchPackageLayoutV1, core::Error>;
[[nodiscard]] auto prepare_collective(const Module&, const Request&, const EpycBenchPackageLayoutV1&,
                                      ResourceProvider = {}, std::uint64_t maximum_region_bytes = default_region_budget)
    -> std::expected<PreparedCollective, core::Error>;
} // namespace epycbench::platform_linux::package_execution
