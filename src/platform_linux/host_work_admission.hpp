#pragma once

#include "epycbench/core/id.hpp"
#include "epycbench/core/quantity.hpp"
#include "platform_linux/process_ipc_operations.hpp"

#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace epycbench::platform_linux::host_work {

enum class Reason : unsigned char {
    invalid_input, capacity, arithmetic, clock, deadline, cancelled, open,
    metadata, authentication, read, progress, parse, missing, duplicate,
    wrong_node, headroom, close, abandoned, invalid_state, limit_get, limit_set,
    limit_verify, allocation
};
struct Failure final {
    Reason primary{};
    std::array<Reason, 4> cleanup{};
    std::size_t cleanup_count{};
};
enum class Source : unsigned char { proc, sysfs };
struct Metadata final {
    std::uint64_t device{}, inode{}, uid{}, mode{}, links{};
    Source filesystem{};
    bool known_filesystem{};
    auto operator==(const Metadata&) const -> bool = default;
};
struct GuardOperations final {
    void* context{};
    detail::RawStatusResult (*now)(void*, std::uint64_t*) noexcept{};
    bool (*cancelled)(void*) noexcept{};
    // Source owners may separate checked timespec conversion from the actual
    // syscall pair. The actor-local cell is written before conversion and
    // copied immediately by a failing guard, before any cleanup clock sample.
    // This is diagnostic provenance, not authority to bypass the checked result.
    const detail::RawStatusResult* actual_raw_now{};
};
struct ReadOperations final {
    void* context{};
    detail::RawStatusResult (*open_root)(void*, Source) noexcept{};
    detail::RawStatusResult (*open_at)(void*, int, const char*, bool directory) noexcept{};
    detail::RawStatusResult (*metadata)(void*, int, Metadata*) noexcept{};
    detail::RawStatusResult (*read)(void*, int, std::span<char>) noexcept{};
    detail::RawStatusResult (*close)(void*, int) noexcept{};
    GuardOperations guard;
};
struct ReadLimits final {
    std::size_t bytes{65536}, calls{1024};
};
struct HeadroomReceipt final {
    std::size_t selected_nodes{};
    bool global_sufficient{};
    bool all_nodes_sufficient{};
    auto operator==(const HeadroomReceipt&) const -> bool = default;
};
// Fixed, non-retaining parser. The caller authenticates the source and node.
[[nodiscard]] auto parse_memory_bytes(std::string_view, std::optional<core::LinuxNumaNodeId>) noexcept
    -> std::expected<core::ByteCount, Reason>;
[[nodiscard]] auto check_memory_headroom(core::ByteCount, bool selected_node) noexcept
    -> std::expected<void, Reason>;
[[nodiscard]] auto read_headroom(std::span<const core::LinuxNumaNodeId>,
    const ReadOperations&, ReadLimits = {}) noexcept -> std::expected<HeadroomReceipt, Failure>;
[[nodiscard]] auto production_read_operations() noexcept -> ReadOperations;

struct Resources final {
    std::size_t processes{}, threads{}, measurement_workers{}, service_roles{}, q2_helpers{};
    std::size_t descriptors{};
    core::ByteCount stack_bytes{core::ByteCount::from_base_units(0)};
    core::ByteCount mapped_bytes{core::ByteCount::from_base_units(0)};
    core::ByteCount ordinary_bytes{core::ByteCount::from_base_units(0)};
    core::ByteCount address_space_bytes{core::ByteCount::from_base_units(0)};
    auto operator==(const Resources&) const -> bool = default;
};
class ResourceLedger;
class ResourceLease final {
  public:
    ResourceLease(const ResourceLease&) = delete;
    auto operator=(const ResourceLease&) -> ResourceLease& = delete;
    ResourceLease(ResourceLease&&) noexcept;
    auto operator=(ResourceLease&&) noexcept -> ResourceLease&;
    ~ResourceLease() noexcept;
    // The actual resource owner calls this only after confirmed retirement.
    [[nodiscard]] auto retire() noexcept -> std::expected<void, Reason>;
    [[nodiscard]] auto active() const noexcept -> bool;
  private:
    struct State;
    ResourceLease(std::shared_ptr<State>, Resources) noexcept;
    void abandon() noexcept;
    std::shared_ptr<State> state_;
    Resources charged_;
    friend class ResourceLedger;
};
class ResourceLedger final {
  public:
    ResourceLedger(const ResourceLedger&) = delete;
    auto operator=(const ResourceLedger&) -> ResourceLedger& = delete;
    ResourceLedger(ResourceLedger&&) noexcept = default;
    auto operator=(ResourceLedger&&) noexcept -> ResourceLedger& = default;
    [[nodiscard]] static auto prepare() noexcept -> std::expected<ResourceLedger, Reason>;
    // A quota inside an already-reserved enclosing grant, never another full budget.
    // Byte quotas are independent caps, each within the address-space cap. Omitted
    // descriptor/byte caps grant no storage authority, including count-only grants.
    [[nodiscard]] static auto prepare(Resources quota) noexcept -> std::expected<ResourceLedger, Reason>;
    // Each charge has address_space_bytes == stack + mapped + ordinary. Charge
    // each address range once; the actual owner still proves each stack <=8MiB
    // and confirms retirement before releasing the corresponding lease.
    [[nodiscard]] auto reserve(Resources) noexcept -> std::expected<ResourceLease, Reason>;
    [[nodiscard]] auto outstanding() const noexcept -> Resources;
    [[nodiscard]] auto poisoned() const noexcept -> bool;
  private:
    explicit ResourceLedger(std::shared_ptr<ResourceLease::State>) noexcept;
    std::shared_ptr<ResourceLease::State> state_;
};

enum class NativeLimit : unsigned char { address_space, stack, descriptors };
struct LimitPair final {
    std::uint64_t soft{}, hard{}; // UINT64_MAX represents RLIM_INFINITY.
    auto operator==(const LimitPair&) const -> bool = default;
};
struct LimitDemand final {
    core::ByteCount address_space{core::ByteCount::from_base_units(0)};
    core::ByteCount stack{core::ByteCount::from_base_units(0)};
    std::size_t descriptors{};
};
[[nodiscard]] auto check_limit_demand(LimitDemand) noexcept -> std::expected<void, Reason>;
struct LimitOperations final {
    void* context{};
    detail::RawStatusResult (*identity)(void*, int* pid, int* parent, std::uint64_t* uid) noexcept{};
    detail::RawStatusResult (*get)(void*, NativeLimit, LimitPair*) noexcept{};
    detail::RawStatusResult (*set)(void*, NativeLimit, LimitPair) noexcept{};
    std::uint64_t maximum_finite{};
    GuardOperations guard;
};
struct LimitReceipt final {
    std::array<LimitPair, 3> limits{};
};
class ChildLimitAdmission final {
  public:
    ChildLimitAdmission(const ChildLimitAdmission&) = delete;
    auto operator=(const ChildLimitAdmission&) -> ChildLimitAdmission& = delete;
    ChildLimitAdmission(ChildLimitAdmission&&) noexcept;
    auto operator=(ChildLimitAdmission&&) noexcept -> ChildLimitAdmission&;
    [[nodiscard]] static auto prepare(int expected_parent, LimitDemand) noexcept
        -> std::expected<ChildLimitAdmission, Reason>;
    // One terminal consumption, including failed or partially applied limits.
    [[nodiscard]] auto apply(const LimitOperations&) noexcept -> std::expected<LimitReceipt, Reason>;
  private:
    ChildLimitAdmission(int, LimitDemand) noexcept;
    int parent_{};
    LimitDemand demand_;
};
[[nodiscard]] auto production_limit_operations() noexcept -> LimitOperations;

} // namespace epycbench::platform_linux::host_work
