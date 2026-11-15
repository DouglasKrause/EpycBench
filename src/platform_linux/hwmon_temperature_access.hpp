#pragma once

#include "epycbench/platform_linux/hwmon_temperature.hpp"
#include "platform_linux/bounded_sysfs_operations.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::platform_linux::detail {

class PreparedBoundedHwmonDiscoveryStorage;
class FailedPreparedBoundedHwmonDiscovery;
class PreparedBoundedHwmonDiscoveryAccess;
class PreparedBoundedHwmonDiscoveryTestAccess;

struct HwmonFile {
    std::string path;
    std::string contents;
};

struct BoundedHwmonEntry final {
    std::uint32_t directory{};
    std::string_view name;
    std::string_view contents;
    std::uint32_t locator{};
};
enum class BoundedHwmonDiscoveryFailure : unsigned char {
    enumeration_capacity,
    token_capacity,
    ambiguous,
};

enum class BoundedHwmonSourceFailureKind : unsigned char {
    enumerate,
    read,
    value,
    ambiguous,
    enumeration_capacity,
    token_capacity,
    contained,
    preparation_rejected,
};
struct BoundedHwmonSourceFailure final {
    BoundedHwmonSourceFailureKind kind{};
    int native_error{};
    BoundedSysfsDispositionKind disposition{BoundedSysfsDispositionKind::success};
};
struct BoundedHwmonSourceFailureSequence final {
    BoundedHwmonSourceFailure primary;
    std::size_t cleanup_count{};
    BoundedHwmonSourceFailureSequence(BoundedHwmonSourceFailure value) noexcept
        : primary{value} {}
    BoundedHwmonSourceFailureSequence(BoundedHwmonSourceFailure value,
                                      std::size_t count) noexcept
        : primary{value}, cleanup_count{count} {}
};
struct BoundedHwmonCapacity final {
    std::uint64_t maximum_directories{};
    std::uint64_t entries_per_directory{};
    std::uint64_t token_bytes{};
};

// The same checked dimensions used by discovery preparation; no allocation,
// operation-table access or injected stock-limit truncation occurs here.
struct BoundedHwmonStorageShape final {
    std::size_t token_bytes{}, token_storage{}, entry_storage{};
    std::size_t operation_capacity{}, retirement_capacity{};
};
[[nodiscard]] auto bounded_hwmon_storage_shape(BoundedHwmonCapacity) noexcept
    -> std::expected<BoundedHwmonStorageShape, BoundedHwmonSourceFailureKind>;

enum class BoundedHwmonValidatedDisposition : unsigned char {
    unsupported,
    supported,
};

class BoundedHwmonValidatedOutcome final {
  public:
    BoundedHwmonValidatedOutcome(const BoundedHwmonValidatedOutcome&) = delete;
    BoundedHwmonValidatedOutcome(BoundedHwmonValidatedOutcome&&) noexcept = default;
    [[nodiscard]] auto disposition() const noexcept { return disposition_; }
  private:
    explicit BoundedHwmonValidatedOutcome(
        BoundedHwmonValidatedDisposition disposition) noexcept
        : disposition_{disposition} {}
    BoundedHwmonValidatedDisposition disposition_{};
    friend class BoundedHwmonTemperatureSource;
    friend class BoundedHwmonValidatedOutcomeTestAccess;
};

struct BoundedHwmonValidationGate final {
    void* context{};
    bool (*accept)(void*, BoundedHwmonValidatedOutcome&&) noexcept{};
};

class BoundedHwmonTemperatureSource final {
  public:
    BoundedHwmonTemperatureSource(const BoundedHwmonTemperatureSource&) = delete;
    BoundedHwmonTemperatureSource(BoundedHwmonTemperatureSource&&) noexcept;
    auto operator=(const BoundedHwmonTemperatureSource&)
        -> BoundedHwmonTemperatureSource& = delete;
    auto operator=(BoundedHwmonTemperatureSource&&)
        -> BoundedHwmonTemperatureSource& = delete;
    ~BoundedHwmonTemperatureSource() noexcept;

    [[nodiscard]] static auto discover(
        BoundedHwmonCapacity, BoundedSysfsOperations,
        std::span<BoundedHwmonSourceFailure> cleanup_stock,
        BoundedHwmonValidationGate gate = {}) noexcept
        -> std::expected<std::optional<BoundedHwmonTemperatureSource>,
                         BoundedHwmonSourceFailureSequence>;
    [[nodiscard]] static auto discover_prepared(
        PreparedBoundedHwmonDiscoveryStorage&&,
        BoundedHwmonValidationGate gate = {}) noexcept
        -> std::expected<std::optional<BoundedHwmonTemperatureSource>,
                         FailedPreparedBoundedHwmonDiscovery>;
    [[nodiscard]] auto read(
        std::span<char>,
        std::span<BoundedHwmonSourceFailure> cleanup_stock) const noexcept
        -> std::expected<std::int64_t, BoundedHwmonSourceFailureSequence>;
    [[nodiscard]] auto retire(std::span<BoundedSysfsDisposition>) noexcept
        -> std::size_t;

  private:
    struct State;
    [[nodiscard]] static auto discover_prepared_engine(
        PreparedBoundedHwmonDiscoveryStorage&,
        BoundedHwmonValidationGate) noexcept
        -> std::expected<std::optional<BoundedHwmonTemperatureSource>,
                         BoundedHwmonSourceFailureSequence>;
    explicit BoundedHwmonTemperatureSource(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend class PreparedBoundedHwmonDiscoveryStorage;
    friend class PreparedBoundedHwmonDiscoveryAccess;
    friend class PreparedBoundedHwmonDiscoveryTestAccess;
};

class PreparedBoundedHwmonDiscoveryStorage final {
  public:
    PreparedBoundedHwmonDiscoveryStorage(
        const PreparedBoundedHwmonDiscoveryStorage&) = delete;
    PreparedBoundedHwmonDiscoveryStorage(
        PreparedBoundedHwmonDiscoveryStorage&&) noexcept;
    auto operator=(const PreparedBoundedHwmonDiscoveryStorage&)
        -> PreparedBoundedHwmonDiscoveryStorage& = delete;
    auto operator=(PreparedBoundedHwmonDiscoveryStorage&&) noexcept
        -> PreparedBoundedHwmonDiscoveryStorage& = delete;
    ~PreparedBoundedHwmonDiscoveryStorage() noexcept;

    [[nodiscard]] static auto prepare(
        BoundedHwmonCapacity, BoundedSysfsOperations) noexcept
        -> std::expected<PreparedBoundedHwmonDiscoveryStorage,
                         BoundedHwmonSourceFailureSequence>;

  private:
    explicit PreparedBoundedHwmonDiscoveryStorage(
        std::unique_ptr<BoundedHwmonTemperatureSource::State>) noexcept;
    std::unique_ptr<BoundedHwmonTemperatureSource::State> state_;
    friend class BoundedHwmonTemperatureSource;
    friend class FailedPreparedBoundedHwmonDiscovery;
    friend class PreparedBoundedHwmonDiscoveryAccess;
    friend class PreparedBoundedHwmonDiscoveryTestAccess;
};

class FailedPreparedBoundedHwmonDiscovery final {
  public:
    FailedPreparedBoundedHwmonDiscovery(
        const FailedPreparedBoundedHwmonDiscovery&) = delete;
    FailedPreparedBoundedHwmonDiscovery(
        FailedPreparedBoundedHwmonDiscovery&&) noexcept;
    auto operator=(const FailedPreparedBoundedHwmonDiscovery&)
        -> FailedPreparedBoundedHwmonDiscovery& = delete;
    auto operator=(FailedPreparedBoundedHwmonDiscovery&&) noexcept
        -> FailedPreparedBoundedHwmonDiscovery& = delete;
    ~FailedPreparedBoundedHwmonDiscovery() noexcept;

  private:
    FailedPreparedBoundedHwmonDiscovery(
        PreparedBoundedHwmonDiscoveryStorage&&,
        BoundedHwmonSourceFailureSequence) noexcept;
    std::optional<PreparedBoundedHwmonDiscoveryStorage> storage_;
    BoundedHwmonSourceFailureSequence sequence_{{}};
    friend class BoundedHwmonTemperatureSource;
    friend class PreparedBoundedHwmonDiscoveryAccess;
};

class PreparedBoundedHwmonDiscoveryAccess final {
  public:
    [[nodiscard]] static auto cleanup(
        const FailedPreparedBoundedHwmonDiscovery&) noexcept
        -> std::optional<std::span<const BoundedHwmonSourceFailure>>;
    [[nodiscard]] static auto sequence(
        const FailedPreparedBoundedHwmonDiscovery&) noexcept
        -> std::optional<BoundedHwmonSourceFailureSequence>;
};

class HwmonTemperatureAccess {
  public:
    [[nodiscard]] static auto discover(std::span<const HwmonFile> files, void* context,
                                       HwmonTemperatureSource::ReadOperation read)
        -> std::expected<std::optional<HwmonTemperatureSource>, core::Error>;
    [[nodiscard]] static auto parse(std::string_view text)
        -> std::expected<std::int64_t, core::Error>;
    [[nodiscard]] static auto parse_bounded(std::string_view text) noexcept
        -> std::optional<std::int64_t>;
    [[nodiscard]] static auto discover_bounded(
        std::uint64_t root_entries, std::uint64_t directory_count,
        std::span<const BoundedHwmonEntry> entries,
        std::uint64_t maximum_directories, std::uint64_t entries_per_directory,
        std::uint64_t token_bytes) noexcept
        -> std::expected<std::optional<std::uint32_t>, BoundedHwmonDiscoveryFailure>;
    [[nodiscard]] static auto input(const HwmonTemperatureSource& source) noexcept
        -> std::string_view { return source.input_; }
};

} // namespace epycbench::platform_linux::detail
