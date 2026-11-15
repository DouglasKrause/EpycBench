#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <limits>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace epycbench::platform_linux::detail {

struct BoundedSysfsEnumerationState final {
    alignas(std::uint64_t) std::array<char, 4096> bytes{};
    std::size_t used{};
    std::size_t cursor{};
};

struct BoundedSysfsIdentity final {
    std::uint64_t device{};
    std::uint64_t inode{};
    std::uint64_t owner{};
    std::uint64_t mode{};
    std::uint64_t links{};
    auto operator==(const BoundedSysfsIdentity&) const noexcept -> bool = default;
};
enum class BoundedSysfsDispositionKind : unsigned char {
    success,
    native_failure,
    contained_standard,
    contained_unknown,
};
struct BoundedSysfsCloseOutcome final {
    BoundedSysfsDispositionKind kind{BoundedSysfsDispositionKind::success};
    int native_error{};
    bool stock_exhausted{};
    [[nodiscard]] auto succeeded() const noexcept -> bool {
        return kind == BoundedSysfsDispositionKind::success && !stock_exhausted;
    }
};

struct BoundedSysfsStockObservation final {
    std::size_t operation_capacity{};
    std::size_t retirement_capacity{};
    std::size_t operation_count{};
    std::size_t retirement_count{};
    bool operation_exhausted{};
    bool retirement_exhausted{};
};

struct BoundedSysfsOperations final {
    void* context{};
    int (*open_root)(void*, const char*, int) noexcept{};
    int (*open_at)(void*, int, const char*, int) noexcept{};
    int (*stat_fd)(void*, int, struct stat*) noexcept{};
    int (*stat_at)(void*, int, const char*, struct stat*) noexcept{};
    int (*next)(void*, int, BoundedSysfsEnumerationState*, char*, std::size_t) noexcept{};
    std::ptrdiff_t (*read)(void*, int, void*, std::size_t, std::int64_t) noexcept{};
    BoundedSysfsCloseOutcome (*close)(void*, int) noexcept{};
    // Source-private fault limits. Production leaves both unbounded; focused
    // tests reduce either stock to exercise the real acquisition graph's
    // equality/exhaustion boundary.
    std::size_t operation_stock_limit{std::numeric_limits<std::size_t>::max()};
    std::size_t retirement_stock_limit{std::numeric_limits<std::size_t>::max()};
    BoundedSysfsStockObservation* stock_observation{};
    uid_t (*effective_uid)(void*) noexcept{};
    // Only hwmon class-entry aliases use these leaves. Attribute and CPUFreq
    // opens retain their existing no-follow operations.
    int (*open_hwmon_class_link)(void*, int, const char*, int) noexcept{};
    int (*stat_hwmon_class_target)(void*, int, const char*, struct stat*) noexcept{};
};

struct BoundedSysfsDisposition final {
    BoundedSysfsDispositionKind kind{BoundedSysfsDispositionKind::success};
    int native_error{};
    bool attempted{};
};

class BoundedSysfsDispositionStock final {
  public:
    BoundedSysfsDispositionStock(const BoundedSysfsDispositionStock&) = delete;
    BoundedSysfsDispositionStock(BoundedSysfsDispositionStock&&) noexcept = default;
    [[nodiscard]] static auto prepare(std::size_t operation_capacity,
                                      std::size_t retirement_capacity,
                                      BoundedSysfsStockObservation* = nullptr)
        -> BoundedSysfsDispositionStock;
    [[nodiscard]] auto record_operation(int native_error = 0) noexcept -> bool;
    [[nodiscard]] auto record_operation(BoundedSysfsCloseOutcome) noexcept -> bool;
    [[nodiscard]] auto record_retirement(BoundedSysfsCloseOutcome) noexcept -> bool;
    [[nodiscard]] auto operation_count() const noexcept { return operation_count_; }
    [[nodiscard]] auto retirement_count() const noexcept { return retirement_count_; }
    [[nodiscard]] auto operation_capacity() const noexcept { return operations_.size(); }
    [[nodiscard]] auto retirement_capacity() const noexcept { return retirements_.size(); }
    [[nodiscard]] auto retirements() const noexcept
        -> std::span<const BoundedSysfsDisposition> {
        return {retirements_.data(), retirement_count_};
    }
  private:
    BoundedSysfsDispositionStock(std::vector<BoundedSysfsDisposition> operations,
                                 std::vector<BoundedSysfsDisposition> retirements,
                                 BoundedSysfsStockObservation* observation) noexcept
        : operations_{std::move(operations)}, retirements_{std::move(retirements)},
          observation_{observation} {}
    std::vector<BoundedSysfsDisposition> operations_;
    std::vector<BoundedSysfsDisposition> retirements_;
    std::size_t operation_count_{};
    std::size_t retirement_count_{};
    BoundedSysfsStockObservation* observation_{};
};

[[nodiscard]] auto production_bounded_sysfs_operations() noexcept
    -> BoundedSysfsOperations;
[[nodiscard]] auto bounded_sysfs_identity(const struct stat&) noexcept
    -> BoundedSysfsIdentity;
[[nodiscard]] auto bounded_sysfs_same_identity(const struct stat&, const struct stat&) noexcept
    -> bool;
[[nodiscard]] auto bounded_sysfs_directory_identity(const struct stat&) noexcept -> bool;
[[nodiscard]] auto bounded_sysfs_file_identity(const struct stat&) noexcept -> bool;

} // namespace epycbench::platform_linux::detail
