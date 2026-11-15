#pragma once

#include "epycbench/platform_linux/perf_context_switch.hpp"

#include <linux/perf_event.h>
#include <type_traits>

namespace epycbench::platform_linux::detail {
struct Q2PerfContextSwitchState;
class Q2PerfRetiredState final {
  public:
    Q2PerfRetiredState() noexcept;
    Q2PerfRetiredState(Q2PerfRetiredState&&) noexcept;
    Q2PerfRetiredState& operator=(Q2PerfRetiredState&&) noexcept;
    Q2PerfRetiredState(const Q2PerfRetiredState&) = delete;
    ~Q2PerfRetiredState() noexcept;

  private:
    explicit Q2PerfRetiredState(std::unique_ptr<Q2PerfContextSwitchState>) noexcept;
    std::unique_ptr<Q2PerfContextSwitchState> state_;
    friend class Q2PerfContextSwitchSource;
};
struct Q2PerfBoundedDrain final {
    std::size_t record_count{};
    PerfCaptureCompleteness completeness{PerfCaptureCompleteness::complete};
};

enum class BoundedPerfFailureKind : unsigned char {
    metadata_capacity,
    cursor,
    truncated_header,
    record_size,
    switch_size,
    record,
    capacity,
    record_type,
    lost,
};

struct BoundedPerfMetadata final {
    std::size_t ring_offset{};
    std::size_t ring_size{};
};

struct BoundedPerfDrain final {
    std::size_t record_count{};
    bool lost{};
};

// Owned scalar diagnostics only; no record identity, address or borrowed ring.
struct BoundedPerfFirstLoss final {
    bool available{};
    std::uint32_t record_type{};
    std::uint16_t header_size{};
    std::optional<std::uint64_t> lost_count{};
    std::uint64_t captured_head{};
    std::uint64_t captured_tail{};
    std::uint64_t ring_bytes{};
};
static_assert(std::is_trivially_copyable_v<BoundedPerfFirstLoss>);

enum class BoundedPerfMapFailure : unsigned char { mmap, map };

struct PerfContextLossReadResult final {
    std::uint64_t value{};
    std::uint64_t lost{};
    std::int64_t bytes{};
    int error_number{};
};

struct BoundedPerfContextSwitchOperations final {
    struct DescriptorResult final {
        int value{-1};
        int error_number{};
    };
    struct MappingResult final {
        void* value{};
        int error_number{};
        BoundedPerfMapFailure failure{BoundedPerfMapFailure::mmap};
    };
    struct StatusResult final {
        int value{};
        int error_number{};
    };
    void* context{};
    DescriptorResult (*open)(void*, LinuxThreadId) noexcept{};
    MappingResult (*map)(void*, int, std::size_t) noexcept{};
    StatusResult (*unmap)(void*, void*, std::size_t) noexcept{};
    StatusResult (*close)(void*, int) noexcept{};
    void (*before_metadata)(void*) noexcept{};
    void (*before_second_pass)(void*) noexcept{};
    StatusResult (*enable)(void*, int) noexcept{};
    StatusResult (*disable)(void*, int) noexcept{};
    PerfContextLossReadResult (*read_loss)(void*, int) noexcept{};
    DescriptorResult (*open_capture)(void*, LinuxThreadId) noexcept{};
};

[[nodiscard]] auto production_bounded_perf_context_switch_operations() noexcept
    -> BoundedPerfContextSwitchOperations;

[[nodiscard]] auto validate_bounded_perf_metadata(
    void* mapping, std::size_t mapping_size, std::size_t ring_limit,
    std::size_t record_capacity) noexcept
    -> std::expected<BoundedPerfMetadata, BoundedPerfFailureKind>;
[[nodiscard]] auto drain_bounded_perf_records(
    void* mapping, BoundedPerfMetadata metadata,
    std::span<PerfContextSwitchRecord> output, void* hook_context,
    void (*before_second_pass)(void*) noexcept,
    bool discard_lost = false, bool commit_tail = true,
    BoundedPerfFirstLoss* first_loss = nullptr) noexcept
    -> std::expected<BoundedPerfDrain, BoundedPerfFailureKind>;

struct PerfContextSwitchOperations {
    void* context;
    auto (*open)(void*, const perf_event_attr&, LinuxThreadId) noexcept
        -> std::expected<int, core::Error>;
    auto (*map)(void*, int, std::size_t) noexcept -> std::expected<void*, core::Error>;
    void (*unmap)(void*, void*, std::size_t) noexcept;
    void (*close)(void*, int) noexcept;
};
struct Q2PerfContextSwitchOperations {
    struct OpenOutcome {
        int descriptor{-1};
        int error{};
    };
    struct MapOutcome {
        void* mapping{};
        int error{};
    };
    void* context{};
    OpenOutcome (*open)(void*, const perf_event_attr&, LinuxThreadId) noexcept {};
    MapOutcome (*map)(void*, int, std::size_t) noexcept {};
    int (*unmap)(void*, void*, std::size_t) noexcept {};
    int (*close)(void*, int) noexcept {};
    void (*after_map_test)(void*){};
    int (*enable)(void*, int) noexcept{};
    int (*disable)(void*, int) noexcept{};
    PerfContextLossReadResult (*read_loss)(void*, int) noexcept{};
};
struct Q2PerfProductionSyscalls {
    void* context{};
    long (*perf_open)(void*, const perf_event_attr*, int, int, int, unsigned long) noexcept {};
    void* (*map)(void*, std::size_t, int) noexcept {};
    int (*unmap)(void*, void*, std::size_t) noexcept {};
    int (*close)(void*, int) noexcept {};
    int (*control)(void*, int, bool) noexcept {};
    std::int64_t (*read)(void*, int, void*, std::size_t) noexcept {};
};
class Q2PerfProductionProvider final {
  public:
    explicit Q2PerfProductionProvider(Q2PerfProductionSyscalls operations) noexcept
        : operations_{operations} {}
    auto operations() noexcept -> Q2PerfContextSwitchOperations;

  private:
    Q2PerfProductionSyscalls operations_;
};
auto q2_perf_production_provider() noexcept -> Q2PerfProductionProvider&;
[[nodiscard]] auto bounded_perf_context_switch_operations(Q2PerfProductionProvider&) noexcept
    -> BoundedPerfContextSwitchOperations;
struct Q2PerfRetirement final {
    std::optional<core::Error> primary;
    std::optional<core::Error> cleanup;
    Q2PerfRetiredState deferred_state;
};
struct Q2PerfPrepareFailure final {
    enum class Stage : std::uint8_t { validation, open, map, construction } stage{};
    core::Error primary;
    std::optional<core::Error> cleanup;
    std::optional<core::Error> cleanup2;
};
class Q2PerfContextSwitchSource final {
  public:
    static auto prepare(LinuxThreadId, std::size_t, std::size_t,
                        Q2PerfContextSwitchOperations) noexcept
        -> std::expected<Q2PerfContextSwitchSource, Q2PerfPrepareFailure>;
    Q2PerfContextSwitchSource(Q2PerfContextSwitchSource&&) noexcept;
    Q2PerfContextSwitchSource(const Q2PerfContextSwitchSource&) = delete;
    ~Q2PerfContextSwitchSource() noexcept;
    auto drain_bounded(std::span<PerfContextSwitchRecord>) noexcept
        -> std::expected<Q2PerfBoundedDrain, core::Error>;
    // Validate/decode into caller-owned scratch without advancing the perf tail.
    auto preview_bounded(std::span<PerfContextSwitchRecord>) noexcept
        -> std::expected<Q2PerfBoundedDrain, core::Error>;
    auto terminal_drain_after_failure(std::span<PerfContextSwitchRecord>) noexcept
        -> std::expected<Q2PerfBoundedDrain, core::Error>;
    auto retire() noexcept -> Q2PerfRetirement;

  private:
    auto decode_bounded(std::span<PerfContextSwitchRecord>, bool commit_tail) noexcept
        -> std::expected<Q2PerfBoundedDrain, core::Error>;
    explicit Q2PerfContextSwitchSource(std::unique_ptr<Q2PerfContextSwitchState>) noexcept;
    std::unique_ptr<Q2PerfContextSwitchState> state_;
};

class PerfContextSwitchAccess {
  public:
    struct BoundedDrain final {
        std::size_t record_count{};
        PerfCaptureCompleteness completeness{PerfCaptureCompleteness::complete};
    };
    [[nodiscard]] static auto prepare(LinuxThreadId thread_id, std::size_t data_pages,
                                      std::size_t page_size,
                                      const PerfContextSwitchOperations& operations)
        -> std::expected<PerfContextSwitchSource, core::Error>;
    [[nodiscard]] static auto decode(std::span<const std::byte> ring, std::uint64_t tail,
                                     std::uint64_t head)
        -> std::expected<PerfContextSwitchDrain, core::Error>;
    [[nodiscard]] static auto drain_bounded(PerfContextSwitchSource&,
                                            std::span<PerfContextSwitchRecord>) noexcept
        -> std::expected<BoundedDrain, core::Error>;
};

} // namespace epycbench::platform_linux::detail
