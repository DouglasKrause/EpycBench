#include "epycbench/platform_linux/hwmon_temperature.hpp"

#include "epycbench/platform_linux/file.hpp"
#include "platform_linux/hwmon_prepared_discovery_test_access.hpp"
#include "platform_linux/hwmon_temperature_access.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::platform_linux {
namespace {
[[maybe_unused]] constexpr auto pairwise_enumeration_capacity_source_anchor =
    "linux.hwmon_temperature.enumeration_capacity_exceeded";
[[maybe_unused]] constexpr auto pairwise_token_capacity_source_anchor =
    "linux.hwmon_temperature.token_capacity_exceeded";
auto error(std::string_view code, std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Linux hwmon temperature operation failed",
            .operation = "hwmon_temperature",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto production_read(void*, const std::string& path) -> std::expected<std::string, core::Error> {
    auto value = read_text_file(path);
    if (!value) return std::unexpected{error("linux.hwmon_temperature.read_failed", "read")};
    return value;
}
} // namespace

struct detail::BoundedHwmonTemperatureSource::State final {
    State(BoundedHwmonCapacity capacity_value,
          BoundedSysfsOperations operations_value, std::size_t token_capacity,
          std::size_t entry_storage_capacity,
          std::size_t operation_capacity,
          std::size_t retirement_capacity)
        : operations{operations_value}, capacity{capacity_value},
          entry_name(token_capacity + 1U), contents(token_capacity + 1U),
          labels(entry_storage_capacity), entry_names(entry_storage_capacity),
          cleanup_stock(3U),
          disposition_stock(BoundedSysfsDispositionStock::prepare(
              std::min(operation_capacity,
                       operations_value.operation_stock_limit),
              std::min(retirement_capacity,
                       operations_value.retirement_stock_limit),
              operations_value.stock_observation)),
          content_capacity{token_capacity} {
        selected_device.reserve(token_capacity);
        selected_attribute.reserve(token_capacity);
        current_device.reserve(token_capacity);
        current_input.reserve(token_capacity);
        device_name.reserve(token_capacity);
        attribute.reserve(token_capacity);
    }

    BoundedSysfsOperations operations{};
    BoundedHwmonCapacity capacity{};
    std::vector<char> entry_name;
    std::vector<char> contents;
    std::vector<char> labels;
    std::vector<char> entry_names;
    std::string selected_device;
    std::string selected_attribute;
    std::string current_device;
    std::string current_input;
    std::vector<BoundedHwmonSourceFailure> cleanup_stock;
    BoundedSysfsDispositionStock disposition_stock;
    std::size_t cleanup_count{};
    int root{-1};
    int device{-1};
    int attribute_descriptor{-1};
    BoundedSysfsIdentity root_identity{};
    BoundedSysfsIdentity device_identity{};
    BoundedSysfsIdentity device_entry_identity{};
    BoundedSysfsIdentity attribute_identity{};
    std::string device_name;
    std::string attribute;
    std::size_t content_capacity{};
    bool active{};
    bool class_link{};
    ~State() noexcept {
        if (active && attribute_descriptor >= 0 && operations.close != nullptr)
            static_cast<void>(operations.close(operations.context,
                                               attribute_descriptor));
        if (active && device >= 0 && operations.close != nullptr)
            static_cast<void>(operations.close(operations.context, device));
        if (active && root >= 0 && operations.close != nullptr)
            static_cast<void>(operations.close(operations.context, root));
    }
};

auto detail::PreparedBoundedHwmonDiscoveryTestAccess::observe(
    const PreparedBoundedHwmonDiscoveryStorage& prepared) noexcept
    -> std::optional<PreparedBoundedHwmonDiscoveryObservation> {
    if (!prepared.state_)
        return std::nullopt;
    const auto& state = *prepared.state_;
    return PreparedBoundedHwmonDiscoveryObservation{
        .state = &state,
        .entry_name = state.entry_name.data(),
        .entry_name_size = state.entry_name.size(),
        .entry_name_capacity = state.entry_name.capacity(),
        .contents = state.contents.data(),
        .contents_size = state.contents.size(),
        .contents_capacity = state.contents.capacity(),
        .labels = state.labels.data(),
        .labels_size = state.labels.size(),
        .labels_capacity = state.labels.capacity(),
        .entry_names = state.entry_names.data(),
        .entry_names_size = state.entry_names.size(),
        .entry_names_capacity = state.entry_names.capacity(),
        .cleanup = state.cleanup_stock.data(),
        .cleanup_size = state.cleanup_stock.size(),
        .cleanup_capacity = state.cleanup_stock.capacity(),
        .selected_device = state.selected_device.data(),
        .selected_device_capacity = state.selected_device.capacity(),
        .selected_attribute = state.selected_attribute.data(),
        .selected_attribute_capacity = state.selected_attribute.capacity(),
        .current_device = state.current_device.data(),
        .current_device_capacity = state.current_device.capacity(),
        .current_input = state.current_input.data(),
        .current_input_capacity = state.current_input.capacity(),
        .final_device = state.device_name.data(),
        .final_device_capacity = state.device_name.capacity(),
        .final_attribute = state.attribute.data(),
        .final_attribute_capacity = state.attribute.capacity(),
        .operation_capacity = state.disposition_stock.operation_capacity(),
        .retirement_capacity = state.disposition_stock.retirement_capacity(),
        .cleanup_count = state.cleanup_count,
        .active = state.active};
}

auto detail::PreparedBoundedHwmonDiscoveryTestAccess::observe(
    const BoundedHwmonTemperatureSource& source) noexcept
    -> std::optional<PreparedBoundedHwmonDiscoveryObservation> {
    if (!source.state_)
        return std::nullopt;
    const auto& state = *source.state_;
    return PreparedBoundedHwmonDiscoveryObservation{
        .state = &state,
        .entry_name = state.entry_name.data(),
        .entry_name_size = state.entry_name.size(),
        .entry_name_capacity = state.entry_name.capacity(),
        .contents = state.contents.data(),
        .contents_size = state.contents.size(),
        .contents_capacity = state.contents.capacity(),
        .labels = state.labels.data(),
        .labels_size = state.labels.size(),
        .labels_capacity = state.labels.capacity(),
        .entry_names = state.entry_names.data(),
        .entry_names_size = state.entry_names.size(),
        .entry_names_capacity = state.entry_names.capacity(),
        .cleanup = state.cleanup_stock.data(),
        .cleanup_size = state.cleanup_stock.size(),
        .cleanup_capacity = state.cleanup_stock.capacity(),
        .selected_device = state.selected_device.data(),
        .selected_device_capacity = state.selected_device.capacity(),
        .selected_attribute = state.selected_attribute.data(),
        .selected_attribute_capacity = state.selected_attribute.capacity(),
        .current_device = state.current_device.data(),
        .current_device_capacity = state.current_device.capacity(),
        .current_input = state.current_input.data(),
        .current_input_capacity = state.current_input.capacity(),
        .final_device = state.device_name.data(),
        .final_device_capacity = state.device_name.capacity(),
        .final_attribute = state.attribute.data(),
        .final_attribute_capacity = state.attribute.capacity(),
        .operation_capacity = state.disposition_stock.operation_capacity(),
        .retirement_capacity = state.disposition_stock.retirement_capacity(),
        .cleanup_count = state.cleanup_count,
        .active = state.active};
}

namespace detail {
namespace {
constexpr auto hwmon_root = "/sys/class/hwmon";

[[nodiscard]] auto checked_add(std::size_t left, std::size_t right,
                               std::size_t& result) noexcept -> bool {
    if (left > std::numeric_limits<std::size_t>::max() - right)
        return false;
    result = left + right;
    return true;
}

[[nodiscard]] auto checked_multiply(std::size_t left, std::size_t right,
                                    std::size_t& result) noexcept -> bool {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)
        return false;
    result = left * right;
    return true;
}

[[nodiscard]] auto checked_size(std::uint64_t value,
                                std::size_t& result) noexcept -> bool {
    if (value > std::numeric_limits<std::size_t>::max())
        return false;
    result = static_cast<std::size_t>(value);
    return true;
}

class DescriptorRole final {
  public:
    DescriptorRole(BoundedSysfsOperations operations, int descriptor,
                   BoundedSysfsDispositionStock* stock = nullptr) noexcept
        : operations_{operations}, descriptor_{descriptor}, stock_{stock} {}
    DescriptorRole(const DescriptorRole&) = delete;
    DescriptorRole(DescriptorRole&&) = delete;
    ~DescriptorRole() noexcept { static_cast<void>(retire()); }
    [[nodiscard]] auto get() const noexcept { return descriptor_; }
    [[nodiscard]] auto retire() noexcept -> BoundedSysfsCloseOutcome {
        if (descriptor_ < 0)
            return {};
        const auto descriptor = std::exchange(descriptor_, -1);
        auto outcome = operations_.close(operations_.context, descriptor);
        if (stock_ != nullptr) {
            const auto operation_recorded = stock_->record_operation(outcome);
            const auto retirement_recorded = stock_->record_retirement(outcome);
            stock_exhausted_ = !operation_recorded || !retirement_recorded;
            outcome.stock_exhausted = stock_exhausted_;
        }
        return outcome;
    }
    [[nodiscard]] auto stock_exhausted() const noexcept { return stock_exhausted_; }
    [[nodiscard]] auto release() noexcept { return std::exchange(descriptor_, -1); }
  private:
    BoundedSysfsOperations operations_{};
    int descriptor_{-1};
    BoundedSysfsDispositionStock* stock_{};
    bool stock_exhausted_{};
};

[[nodiscard]] auto source_failure(BoundedHwmonSourceFailureKind kind,
                                  int native = 0) noexcept
    -> BoundedHwmonSourceFailure {
    return {kind, native,
            native == 0 ? BoundedSysfsDispositionKind::success
                        : BoundedSysfsDispositionKind::native_failure};
}

[[nodiscard]] auto close_failure(BoundedHwmonSourceFailureKind kind,
                                 BoundedSysfsCloseOutcome outcome) noexcept
    -> BoundedHwmonSourceFailure {
    return {kind, outcome.native_error, outcome.kind};
}

[[nodiscard]] auto retirement_primary(BoundedHwmonSourceFailureKind kind,
                                      BoundedSysfsCloseOutcome outcome) noexcept
    -> BoundedHwmonSourceFailure {
    return outcome.kind == BoundedSysfsDispositionKind::success &&
                   outcome.stock_exhausted
               ? source_failure(BoundedHwmonSourceFailureKind::enumeration_capacity)
               : close_failure(kind, outcome);
}

struct HwmonDeviceOpen final {
    int descriptor{-1};
    int error{};
    bool class_link{};
    BoundedSysfsIdentity entry_identity{};
};

[[nodiscard]] auto hwmon_class_name(std::string_view name) noexcept -> bool {
    if (!name.starts_with("hwmon")) return false;
    const auto number = name.substr(5U);
    return !number.empty() && (number.size() == 1U || number.front() != '0') &&
        std::ranges::all_of(number, [](char value) { return value >= '0' && value <= '9'; });
}

[[nodiscard]] auto hwmon_link_identity(const struct stat& value,
                                      std::uint64_t token_bytes) noexcept -> bool {
    return S_ISLNK(value.st_mode) && value.st_uid == 0U && value.st_nlink == 1U &&
        value.st_size >= 0 && static_cast<std::uint64_t>(value.st_size) <= token_bytes;
}

[[nodiscard]] auto open_hwmon_device(BoundedSysfsOperations operations, int parent,
    const char* name, std::uint64_t token_bytes,
    BoundedSysfsDispositionStock* stock = nullptr) noexcept
    -> std::expected<HwmonDeviceOpen, BoundedHwmonSourceFailure> {
    constexpr int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_DIRECTORY;
    const int descriptor = operations.open_at(operations.context, parent, name,
                                               flags | O_NOFOLLOW);
    if (descriptor >= 0) return HwmonDeviceOpen{descriptor};
    const int saved = errno;
    if (saved != ENOTDIR && saved != ELOOP) return HwmonDeviceOpen{-1, saved};
    if (stock != nullptr && !stock->record_operation(saved))
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::enumeration_capacity)};
    struct stat entry{};
    const int observed = operations.stat_at(operations.context, parent, name, &entry);
    const int stat_error = observed == 0 ? 0 : errno;
    if (stock != nullptr && !stock->record_operation(stat_error))
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::enumeration_capacity)};
    if (observed != 0)
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::enumerate, stat_error)};
    if (!S_ISLNK(entry.st_mode)) return HwmonDeviceOpen{-1, saved};
    if (!hwmon_class_name(name) || !hwmon_link_identity(entry, token_bytes))
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::enumerate, EINVAL)};
    if (operations.open_hwmon_class_link == nullptr ||
        operations.stat_hwmon_class_target == nullptr)
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::enumerate, ENOTSUP)};
    const int resolved = operations.open_hwmon_class_link(operations.context, parent, name, flags);
    return HwmonDeviceOpen{resolved, resolved < 0 ? errno : 0, true,
                            bounded_sysfs_identity(entry)};
}

[[nodiscard]] auto hwmon_named_device_matches(BoundedSysfsOperations operations,
    int parent, const char* name, const struct stat& held, const struct stat& named,
    bool class_link, BoundedSysfsIdentity entry_identity,
    BoundedSysfsDispositionStock* stock = nullptr, bool* exhausted = nullptr) noexcept -> bool {
    if (!class_link) return bounded_sysfs_same_identity(held, named);
    if (held.st_uid != 0U || bounded_sysfs_identity(named) != entry_identity) return false;
    if (stock != nullptr && !stock->record_operation()) {
        if (exhausted != nullptr) *exhausted = true;
        return false;
    }
    struct stat target{};
    return operations.stat_hwmon_class_target != nullptr &&
        operations.stat_hwmon_class_target(operations.context, parent, name, &target) == 0 &&
        bounded_sysfs_same_identity(held, target);
}

[[nodiscard]] auto read_attribute(BoundedSysfsOperations operations, int parent,
                                  const char* name, std::span<char> buffer,
                                  std::size_t content_capacity,
                                  BoundedSysfsDispositionStock& stock,
                                  std::span<BoundedHwmonSourceFailure> cleanup,
                                  std::size_t& cleanup_count,
    BoundedSysfsIdentity* retained_identity = nullptr) noexcept
    -> std::expected<std::string_view, BoundedHwmonSourceFailureSequence> {
    const auto append_cleanup = [&](BoundedSysfsCloseOutcome outcome) noexcept {
        if (outcome.kind != BoundedSysfsDispositionKind::success &&
            cleanup_count < cleanup.size())
            cleanup[cleanup_count++] =
                close_failure(BoundedHwmonSourceFailureKind::read, outcome);
        if (outcome.stock_exhausted && cleanup_count < cleanup.size())
            cleanup[cleanup_count++] = source_failure(
                BoundedHwmonSourceFailureKind::enumeration_capacity);
    };
    const auto capacity_failure = [&]() noexcept {
        return BoundedHwmonSourceFailureSequence{
            source_failure(BoundedHwmonSourceFailureKind::enumeration_capacity),
            cleanup_count};
    };
    const int descriptor = operations.open_at(
        operations.context, parent, name,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::read, errno)};
    DescriptorRole role{operations, descriptor, &stock};
    struct stat held{}, named{}, final_held{}, final_named{};
    errno = 0;
    if (operations.stat_fd(operations.context, descriptor, &held) != 0 ||
        operations.stat_at(operations.context, parent, name, &named) != 0 ||
        !bounded_sysfs_file_identity(held) || !bounded_sysfs_same_identity(held, named)) {
        const auto saved = errno == 0 ? ESTALE : errno;
        if (!stock.record_operation(saved)) {
            if (cleanup_count < cleanup.size())
                cleanup[cleanup_count++] = source_failure(
                    BoundedHwmonSourceFailureKind::enumeration_capacity);
            append_cleanup(role.retire());
            return std::unexpected{BoundedHwmonSourceFailureSequence{
                source_failure(BoundedHwmonSourceFailureKind::read, saved),
                cleanup_count}};
        }
        append_cleanup(role.retire());
        if (role.stock_exhausted())
            return std::unexpected{capacity_failure()};
        return std::unexpected{BoundedHwmonSourceFailureSequence{
            source_failure(BoundedHwmonSourceFailureKind::read, saved),
            cleanup_count}};
    }
    if (!stock.record_operation()) {
        append_cleanup(role.retire());
        return std::unexpected{capacity_failure()};
    }
    std::size_t used{};
    while (used < buffer.size()) {
        std::ptrdiff_t count;
        do {
            count = operations.read(
                operations.context, descriptor, buffer.data() + used,
                buffer.size() - used, static_cast<std::int64_t>(used));
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            const auto saved = errno;
            if (!stock.record_operation(saved)) {
                if (cleanup_count < cleanup.size())
                    cleanup[cleanup_count++] = source_failure(
                        BoundedHwmonSourceFailureKind::enumeration_capacity);
                append_cleanup(role.retire());
                return std::unexpected{BoundedHwmonSourceFailureSequence{
                    source_failure(BoundedHwmonSourceFailureKind::read, saved),
                    cleanup_count}};
            }
            append_cleanup(role.retire());
            if (role.stock_exhausted())
                return std::unexpected{capacity_failure()};
            return std::unexpected{BoundedHwmonSourceFailureSequence{
                source_failure(BoundedHwmonSourceFailureKind::read, saved),
                cleanup_count}};
        }
        if (count == 0)
            break;
        used += static_cast<std::size_t>(count);
    }
    if (!stock.record_operation()) {
        append_cleanup(role.retire());
        return std::unexpected{capacity_failure()};
    }
    errno = 0;
    if (operations.stat_fd(operations.context, descriptor, &final_held) != 0 ||
        operations.stat_at(operations.context, parent, name, &final_named) != 0 ||
        !bounded_sysfs_same_identity(held, final_held) ||
        !bounded_sysfs_same_identity(named, final_named)) {
        const auto saved = errno == 0 ? ESTALE : errno;
        if (!stock.record_operation(saved)) {
            if (cleanup_count < cleanup.size())
                cleanup[cleanup_count++] = source_failure(
                    BoundedHwmonSourceFailureKind::enumeration_capacity);
            append_cleanup(role.retire());
            return std::unexpected{BoundedHwmonSourceFailureSequence{
                source_failure(BoundedHwmonSourceFailureKind::read, saved),
                cleanup_count}};
        }
        append_cleanup(role.retire());
        if (role.stock_exhausted())
            return std::unexpected{capacity_failure()};
        return std::unexpected{BoundedHwmonSourceFailureSequence{
            source_failure(BoundedHwmonSourceFailureKind::read, saved),
            cleanup_count}};
    }
    if (!stock.record_operation()) {
        append_cleanup(role.retire());
        return std::unexpected{capacity_failure()};
    }
    const auto close_error = role.retire();
    if (role.stock_exhausted())
        return std::unexpected{capacity_failure()};
    if (!close_error.succeeded())
        return std::unexpected{BoundedHwmonSourceFailureSequence{
            retirement_primary(BoundedHwmonSourceFailureKind::read, close_error),
            cleanup_count}};
    if (used > content_capacity)
        return std::unexpected{
            source_failure(BoundedHwmonSourceFailureKind::token_capacity)};
    if (retained_identity != nullptr)
        *retained_identity = bounded_sysfs_identity(held);
    return std::string_view{buffer.data(), used};
}

} // namespace

BoundedHwmonTemperatureSource::BoundedHwmonTemperatureSource(
    std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
BoundedHwmonTemperatureSource::BoundedHwmonTemperatureSource(
    BoundedHwmonTemperatureSource&&) noexcept = default;
BoundedHwmonTemperatureSource::~BoundedHwmonTemperatureSource() noexcept = default;

PreparedBoundedHwmonDiscoveryStorage::PreparedBoundedHwmonDiscoveryStorage(
    std::unique_ptr<BoundedHwmonTemperatureSource::State> state) noexcept
    : state_{std::move(state)} {}
PreparedBoundedHwmonDiscoveryStorage::PreparedBoundedHwmonDiscoveryStorage(
    PreparedBoundedHwmonDiscoveryStorage&&) noexcept = default;
PreparedBoundedHwmonDiscoveryStorage::~PreparedBoundedHwmonDiscoveryStorage() noexcept =
    default;

auto bounded_hwmon_storage_shape(BoundedHwmonCapacity capacity) noexcept
    -> std::expected<BoundedHwmonStorageShape, BoundedHwmonSourceFailureKind> {
    if (capacity.maximum_directories == 0U || capacity.entries_per_directory == 0U ||
        capacity.token_bytes == 0U)
        return std::unexpected{BoundedHwmonSourceFailureKind::enumerate};
    std::size_t directories{}, entries{}, token_bytes{}, token_storage{};
    std::size_t entry_storage{}, directory_entries{}, retirement_capacity{};
    std::size_t five_entries{}, per_directory_operations{};
    std::size_t directory_operations{}, root_operations{}, operation_base{}, operation_capacity{};
    if (!checked_size(capacity.maximum_directories, directories) ||
        !checked_size(capacity.entries_per_directory, entries) ||
        !checked_size(capacity.token_bytes, token_bytes) ||
        !checked_add(token_bytes, 1U, token_storage) ||
        !checked_multiply(entries, token_storage, entry_storage) ||
        !checked_multiply(directories, entries, directory_entries) ||
        !checked_add(1U, directories, retirement_capacity) ||
        !checked_add(retirement_capacity, directory_entries,
                     retirement_capacity) ||
        !checked_multiply(5U, entries, five_entries) ||
        !checked_add(five_entries, 5U, per_directory_operations) ||
        !checked_multiply(directories, per_directory_operations,
                          directory_operations) ||
        !checked_multiply(3U, entries, root_operations) ||
        !checked_add(root_operations, 3U, operation_base) ||
        !checked_add(operation_base, directory_operations,
                     operation_capacity))
        return std::unexpected{BoundedHwmonSourceFailureKind::enumeration_capacity};

    return BoundedHwmonStorageShape{token_bytes, token_storage, entry_storage,
                                   operation_capacity, retirement_capacity};
}

auto PreparedBoundedHwmonDiscoveryStorage::prepare(
    BoundedHwmonCapacity capacity, BoundedSysfsOperations operations) noexcept
    -> std::expected<PreparedBoundedHwmonDiscoveryStorage,
                     BoundedHwmonSourceFailureSequence> {
    if (capacity.maximum_directories == 0U ||
        capacity.entries_per_directory == 0U || capacity.token_bytes == 0U ||
        operations.open_root == nullptr || operations.open_at == nullptr ||
        operations.stat_fd == nullptr || operations.stat_at == nullptr ||
        operations.next == nullptr || operations.read == nullptr ||
        operations.close == nullptr)
        return std::unexpected{
            source_failure(BoundedHwmonSourceFailureKind::enumerate)};

    const auto shape = bounded_hwmon_storage_shape(capacity);
    if (!shape) return std::unexpected{source_failure(shape.error())};

    try {
        return PreparedBoundedHwmonDiscoveryStorage{std::make_unique<
            BoundedHwmonTemperatureSource::State>(
            capacity, operations, shape->token_bytes, shape->entry_storage,
            shape->operation_capacity, shape->retirement_capacity)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{
            source_failure(BoundedHwmonSourceFailureKind::enumerate)};
    } catch (...) {
        return std::unexpected{
            source_failure(BoundedHwmonSourceFailureKind::contained)};
    }
}

FailedPreparedBoundedHwmonDiscovery::FailedPreparedBoundedHwmonDiscovery(
    PreparedBoundedHwmonDiscoveryStorage&& storage,
    BoundedHwmonSourceFailureSequence sequence) noexcept
    : storage_{std::move(storage)}, sequence_{sequence} {}
FailedPreparedBoundedHwmonDiscovery::FailedPreparedBoundedHwmonDiscovery(
    FailedPreparedBoundedHwmonDiscovery&& other) noexcept
    : storage_{std::move(other.storage_)}, sequence_{other.sequence_} {
    other.storage_.reset();
}
FailedPreparedBoundedHwmonDiscovery::~FailedPreparedBoundedHwmonDiscovery() noexcept =
    default;

auto PreparedBoundedHwmonDiscoveryAccess::cleanup(
    const FailedPreparedBoundedHwmonDiscovery& failed) noexcept
    -> std::optional<std::span<const BoundedHwmonSourceFailure>> {
    if (!failed.storage_ || !failed.storage_->state_ ||
        failed.sequence_.cleanup_count >
            failed.storage_->state_->cleanup_stock.size())
        return std::nullopt;
    return std::span<const BoundedHwmonSourceFailure>{
        failed.storage_->state_->cleanup_stock.data(),
        failed.sequence_.cleanup_count};
}

auto PreparedBoundedHwmonDiscoveryAccess::sequence(
    const FailedPreparedBoundedHwmonDiscovery& failed) noexcept
    -> std::optional<BoundedHwmonSourceFailureSequence> {
    if (!failed.storage_ || !failed.storage_->state_)
        return std::nullopt;
    return failed.sequence_;
}

auto BoundedHwmonTemperatureSource::discover_prepared_engine(
    PreparedBoundedHwmonDiscoveryStorage& prepared,
    BoundedHwmonValidationGate gate) noexcept
    -> std::expected<std::optional<BoundedHwmonTemperatureSource>,
                     BoundedHwmonSourceFailureSequence> {
    if (!prepared.state_)
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::enumerate)};
    auto* state = prepared.state_.get();
    auto& capacity = state->capacity;
    auto& operations = state->operations;
    auto& entry_name = state->entry_name;
    auto& contents = state->contents;
    auto& labels = state->labels;
    auto& entry_names = state->entry_names;
    auto& selected_device = state->selected_device;
    auto& selected_attribute = state->selected_attribute;
    auto& current_device = state->current_device;
    auto& current_input = state->current_input;
    auto& cleanup_stock = state->cleanup_stock;
    auto& cleanup_count = state->cleanup_count;
    auto& disposition_stock = state->disposition_stock;
    if (cleanup_count != 0U || !selected_device.empty() ||
        !selected_attribute.empty() || !current_device.empty() ||
        !current_input.empty() || state->active || state->root >= 0 ||
        state->device >= 0 || state->attribute_descriptor >= 0 ||
        disposition_stock.operation_count() != 0U ||
        disposition_stock.retirement_count() != 0U)
        return std::unexpected{
            source_failure(BoundedHwmonSourceFailureKind::enumerate)};
    {
        const auto append_cleanup = [&](BoundedSysfsCloseOutcome outcome) noexcept {
            if (outcome.kind != BoundedSysfsDispositionKind::success &&
                cleanup_count < cleanup_stock.size())
                cleanup_stock[cleanup_count++] = close_failure(
                    BoundedHwmonSourceFailureKind::enumerate, outcome);
            if (outcome.stock_exhausted && cleanup_count < cleanup_stock.size())
                cleanup_stock[cleanup_count++] = source_failure(
                    BoundedHwmonSourceFailureKind::enumeration_capacity);
        };
        const auto sequence = [&](BoundedHwmonSourceFailure primary) noexcept {
            return BoundedHwmonSourceFailureSequence{primary, cleanup_count};
        };
        const auto append_capacity_cleanup = [&]() noexcept {
            if (cleanup_count < cleanup_stock.size())
                cleanup_stock[cleanup_count++] = source_failure(
                    BoundedHwmonSourceFailureKind::enumeration_capacity);
        };
        const int root_descriptor = operations.open_root(
            operations.context, hwmon_root,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_DIRECTORY);
        if (root_descriptor < 0)
            return std::unexpected{
                source_failure(BoundedHwmonSourceFailureKind::enumerate, errno)};
        DescriptorRole root{operations, root_descriptor, &disposition_stock};
        struct stat root_held{}, root_named{};
        errno = 0;
        if (operations.stat_fd(operations.context, root.get(), &root_held) != 0 ||
            operations.stat_at(operations.context, AT_FDCWD, hwmon_root, &root_named) != 0 ||
            !bounded_sysfs_directory_identity(root_held) ||
            !bounded_sysfs_same_identity(root_held, root_named)) {
            const auto saved = errno == 0 ? ESTALE : errno;
            const auto closed = root.retire();
            append_cleanup(closed);
            return std::unexpected{sequence(source_failure(
                BoundedHwmonSourceFailureKind::enumerate, saved))};
        }
        if (!disposition_stock.record_operation()) {
            append_cleanup(root.retire());
            return std::unexpected{sequence(source_failure(
                BoundedHwmonSourceFailureKind::enumeration_capacity))};
        }

        BoundedSysfsEnumerationState root_enumeration{};
        std::uint64_t root_count{}, directory_count{};
        bool ambiguous{};
        BoundedSysfsIdentity selected_device_identity{};
        BoundedSysfsIdentity selected_entry_identity{};
        BoundedSysfsIdentity selected_attribute_identity{};
        bool selected_class_link{};
        for (;;) {
            const auto next = operations.next(operations.context, root.get(), &root_enumeration,
                                              entry_name.data(), entry_name.size());
            if (next < 0) {
                const auto saved = errno;
                const auto closed = root.retire();
                append_cleanup(closed);
                return std::unexpected{sequence(source_failure(
                    saved == ENAMETOOLONG ? BoundedHwmonSourceFailureKind::token_capacity
                                         : BoundedHwmonSourceFailureKind::enumerate,
                    saved == ENAMETOOLONG ? 0 : saved))};
            }
            if (next == 0)
                break;
            if (!disposition_stock.record_operation()) {
                append_cleanup(root.retire());
                return std::unexpected{sequence(source_failure(
                    BoundedHwmonSourceFailureKind::enumeration_capacity))};
            }
            if (++root_count > capacity.entries_per_directory) {
                const auto closed = root.retire();
                append_cleanup(closed);
                return std::unexpected{sequence(source_failure(
                    BoundedHwmonSourceFailureKind::enumeration_capacity))};
            }
            const auto opened = open_hwmon_device(operations, root.get(), entry_name.data(),
                                                   capacity.token_bytes, &disposition_stock);
            if (!opened) {
                append_cleanup(root.retire());
                return std::unexpected{sequence(opened.error())};
            }
            const int device = opened->descriptor;
            if (device < 0) {
                if (!opened->class_link && (opened->error == ENOTDIR || opened->error == ENOENT))
                    continue;
                const auto saved = opened->error;
                const auto closed = root.retire();
                append_cleanup(closed);
                return std::unexpected{sequence(source_failure(
                    BoundedHwmonSourceFailureKind::enumerate, saved))};
            }
            DescriptorRole device_role{operations, device, &disposition_stock};
            if (++directory_count > capacity.maximum_directories) {
                const auto device_close = device_role.retire();
                const auto root_close = root.retire();
                append_cleanup(device_close);
                append_cleanup(root_close);
                return std::unexpected{sequence(source_failure(
                    BoundedHwmonSourceFailureKind::enumeration_capacity))};
            }
            if (!disposition_stock.record_operation()) {
                append_cleanup(device_role.retire());
                append_cleanup(root.retire());
                return std::unexpected{sequence(source_failure(
                    BoundedHwmonSourceFailureKind::enumeration_capacity))};
            }
            struct stat device_held{}, device_named{};
            bool device_stock_exhausted{};
            errno = 0;
            if (operations.stat_fd(operations.context, device, &device_held) != 0 ||
                operations.stat_at(operations.context, root.get(), entry_name.data(),
                                   &device_named) != 0 ||
                !bounded_sysfs_directory_identity(device_held) ||
                !hwmon_named_device_matches(operations, root.get(), entry_name.data(),
                    device_held, device_named, opened->class_link, opened->entry_identity,
                    &disposition_stock, &device_stock_exhausted)) {
                const auto saved = errno == 0 ? ESTALE : errno;
                const auto close_device = device_role.retire();
                const auto close_root = root.retire();
                append_cleanup(close_device);
                append_cleanup(close_root);
                return std::unexpected{sequence(source_failure(
                    device_stock_exhausted ? BoundedHwmonSourceFailureKind::enumeration_capacity
                                           : BoundedHwmonSourceFailureKind::enumerate,
                    device_stock_exhausted ? 0 : saved))};
            }
            current_device.assign(entry_name.data());
            BoundedSysfsEnumerationState device_enumeration{};
            std::uint64_t inner_count{}, label_count{};
            bool k10temp{};
            for (;;) {
                const auto inner = operations.next(
                    operations.context, device, &device_enumeration,
                    entry_name.data(), entry_name.size());
                if (inner < 0) {
                    const auto saved = errno;
                    const auto close_device = device_role.retire();
                    const auto close_root = root.retire();
                    append_cleanup(close_device);
                    append_cleanup(close_root);
                    return std::unexpected{sequence(source_failure(
                        saved == ENAMETOOLONG
                            ? BoundedHwmonSourceFailureKind::token_capacity
                            : BoundedHwmonSourceFailureKind::enumerate,
                        saved == ENAMETOOLONG ? 0 : saved))};
                }
                if (inner == 0)
                    break;
                if (!disposition_stock.record_operation()) {
                    append_cleanup(device_role.retire());
                    append_cleanup(root.retire());
                    return std::unexpected{sequence(source_failure(
                        BoundedHwmonSourceFailureKind::enumeration_capacity))};
                }
                if (++inner_count > capacity.entries_per_directory) {
                    const auto close_device = device_role.retire();
                    const auto close_root = root.retire();
                    append_cleanup(close_device);
                    append_cleanup(close_root);
                    return std::unexpected{sequence(source_failure(
                        BoundedHwmonSourceFailureKind::enumeration_capacity))};
                }
                const std::string_view name{entry_name.data()};
                auto* entry_slot = entry_names.data() + static_cast<std::ptrdiff_t>(
                    (inner_count - 1U) * (capacity.token_bytes + 1U));
                std::copy_n(name.begin(), static_cast<std::ptrdiff_t>(name.size()),
                            entry_slot);
                entry_slot[name.size()] = '\0';
                if (name != "name" &&
                    !(name.starts_with("temp") && name.ends_with("_label")))
                    continue;
                auto text = read_attribute(operations, device, entry_name.data(), contents,
                                           static_cast<std::size_t>(capacity.token_bytes),
                                           disposition_stock, cleanup_stock,
                                           cleanup_count);
                if (!text) {
                    const auto close_device = device_role.retire();
                    const auto close_root = root.retire();
                    append_cleanup(close_device);
                    append_cleanup(close_root);
                    auto failure = text.error();
                    failure.cleanup_count = cleanup_count;
                    return std::unexpected{failure};
                }
                if (name == "name") {
                    k10temp = trim_ascii_space(*text) == "k10temp";
                } else if (trim_ascii_space(*text) == "Tctl") {
                    if (label_count == capacity.entries_per_directory) {
                        const auto close_device = device_role.retire();
                        const auto close_root = root.retire();
                        append_cleanup(close_device);
                        append_cleanup(close_root);
                        return std::unexpected{sequence(source_failure(
                            BoundedHwmonSourceFailureKind::enumeration_capacity))};
                    }
                    auto* slot = labels.data() + static_cast<std::ptrdiff_t>(
                        label_count * (capacity.token_bytes + 1U));
                    std::copy_n(name.begin(), static_cast<std::ptrdiff_t>(name.size()), slot);
                    slot[name.size()] = '\0';
                    ++label_count;
                }
            }
            if (k10temp) {
                for (std::uint64_t index{}; index < label_count; ++index) {
                    const auto* label = labels.data() + static_cast<std::ptrdiff_t>(
                        index * (capacity.token_bytes + 1U));
                    const std::string_view label_name{label};
                    current_input.assign(label_name.substr(0U, label_name.size() - 6U));
                    current_input += "_input";
                    bool input_present{};
                    for (std::uint64_t entry{}; entry < inner_count; ++entry) {
                        const auto* candidate_name =
                            entry_names.data() + static_cast<std::ptrdiff_t>(
                                entry * (capacity.token_bytes + 1U));
                        input_present = input_present ||
                                        std::string_view{candidate_name} == current_input;
                    }
                    if (!input_present)
                        continue;
                    if (!selected_device.empty()) {
                        ambiguous = true;
                        continue;
                    }
                    auto candidate = read_attribute(
                        operations, device, current_input.c_str(), contents,
                        static_cast<std::size_t>(capacity.token_bytes),
                        disposition_stock, cleanup_stock, cleanup_count,
                        &selected_attribute_identity);
                    if (!candidate) {
                        const auto close_device = device_role.retire();
                        const auto close_root = root.retire();
                        append_cleanup(close_device);
                        append_cleanup(close_root);
                        auto candidate_failure = candidate.error();
                        candidate_failure.cleanup_count = cleanup_count;
                        return std::unexpected{candidate_failure};
                    }
                    selected_device = current_device;
                    selected_attribute = current_input;
                    selected_device_identity = bounded_sysfs_identity(device_held);
                    selected_entry_identity = opened->class_link ? opened->entry_identity
                        : bounded_sysfs_identity(device_named);
                    selected_class_link = opened->class_link;
                }
            }
            struct stat device_final{}, device_final_named{};
            errno = 0;
            if (operations.stat_fd(operations.context, device,
                                   &device_final) != 0 ||
                operations.stat_at(operations.context, root.get(),
                                   current_device.c_str(),
                                   &device_final_named) != 0 ||
                !bounded_sysfs_same_identity(device_held, device_final) ||
                !bounded_sysfs_same_identity(device_named, device_final_named) ||
                !hwmon_named_device_matches(operations, root.get(), current_device.c_str(),
                    device_final, device_final_named, opened->class_link, opened->entry_identity,
                    &disposition_stock, &device_stock_exhausted)) {
                const auto saved = errno == 0 ? ESTALE : errno;
                if (!disposition_stock.record_operation(saved))
                    append_capacity_cleanup();
                const auto close_device = device_role.retire();
                const auto close_root = root.retire();
                append_cleanup(close_device);
                append_cleanup(close_root);
                return std::unexpected{BoundedHwmonSourceFailureSequence{
                    source_failure(device_stock_exhausted
                                       ? BoundedHwmonSourceFailureKind::enumeration_capacity
                                       : BoundedHwmonSourceFailureKind::enumerate,
                                   device_stock_exhausted ? 0 : saved),
                    cleanup_count}};
            }
            if (!disposition_stock.record_operation()) {
                append_cleanup(device_role.retire());
                append_cleanup(root.retire());
                return std::unexpected{sequence(source_failure(
                    BoundedHwmonSourceFailureKind::enumeration_capacity))};
            }
            if (const auto saved = device_role.retire(); !saved.succeeded()) {
                const auto root_close = root.retire();
                append_cleanup(root_close);
                return std::unexpected{sequence(retirement_primary(
                    BoundedHwmonSourceFailureKind::enumerate, saved))};
            }
        }
        struct stat root_final{}, root_final_named{};
        errno = 0;
        if (operations.stat_fd(operations.context, root.get(), &root_final) != 0 ||
            operations.stat_at(operations.context, AT_FDCWD, hwmon_root,
                               &root_final_named) != 0 ||
            !bounded_sysfs_same_identity(root_held, root_final) ||
            !bounded_sysfs_same_identity(root_named, root_final_named)) {
            const auto saved = errno == 0 ? ESTALE : errno;
            if (!disposition_stock.record_operation(saved))
                append_capacity_cleanup();
            const auto close_error = root.retire();
            append_cleanup(close_error);
            return std::unexpected{BoundedHwmonSourceFailureSequence{
                source_failure(BoundedHwmonSourceFailureKind::enumerate, saved),
                cleanup_count}};
        }
        if (!disposition_stock.record_operation()) {
            append_cleanup(root.retire());
            return std::unexpected{sequence(source_failure(
                BoundedHwmonSourceFailureKind::enumeration_capacity))};
        }
        if (const auto close_error = root.retire(); !close_error.succeeded())
            return std::unexpected{retirement_primary(
                BoundedHwmonSourceFailureKind::enumerate, close_error)};
        if (ambiguous)
            return std::unexpected{
                source_failure(BoundedHwmonSourceFailureKind::ambiguous)};
        if (selected_device.empty()) {
            if (gate.accept != nullptr &&
                !gate.accept(gate.context, BoundedHwmonValidatedOutcome{
                    BoundedHwmonValidatedDisposition::unsupported}))
                return std::unexpected{source_failure(
                    BoundedHwmonSourceFailureKind::preparation_rejected)};
            prepared.state_.reset();
            return std::optional<BoundedHwmonTemperatureSource>{};
        }

        if (gate.accept != nullptr &&
            !gate.accept(gate.context, BoundedHwmonValidatedOutcome{
                BoundedHwmonValidatedDisposition::supported}))
            return std::unexpected{source_failure(
                BoundedHwmonSourceFailureKind::preparation_rejected)};

        const int persistent_root = operations.open_root(
            operations.context, hwmon_root,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_DIRECTORY);
        if (persistent_root < 0)
            return std::unexpected{
                source_failure(BoundedHwmonSourceFailureKind::enumerate, errno)};
        DescriptorRole persistent_root_role{operations, persistent_root};
        struct stat persistent_root_held{}, persistent_root_named{};
        errno = 0;
        if (operations.stat_fd(operations.context, persistent_root,
                               &persistent_root_held) != 0 ||
            operations.stat_at(operations.context, AT_FDCWD, hwmon_root,
                               &persistent_root_named) != 0 ||
            bounded_sysfs_identity(persistent_root_held) !=
                bounded_sysfs_identity(root_held) ||
            bounded_sysfs_identity(persistent_root_named) !=
                bounded_sysfs_identity(root_named)) {
            const auto saved = errno == 0 ? ESTALE : errno;
            append_cleanup(persistent_root_role.retire());
            return std::unexpected{sequence(source_failure(
                BoundedHwmonSourceFailureKind::enumerate, saved))};
        }
        const auto persistent_opened = open_hwmon_device(operations, persistent_root,
            selected_device.c_str(), capacity.token_bytes);
        if (!persistent_opened) {
            append_cleanup(persistent_root_role.retire());
            return std::unexpected{sequence(persistent_opened.error())};
        }
        const int persistent_device = persistent_opened->descriptor;
        if (persistent_device < 0) {
            const auto saved = persistent_opened->error;
            append_cleanup(persistent_root_role.retire());
            return std::unexpected{sequence(source_failure(
                BoundedHwmonSourceFailureKind::enumerate, saved))};
        }
        DescriptorRole persistent_device_role{operations, persistent_device};
        struct stat persistent_identity{}, persistent_named_identity{};
        errno = 0;
        if (operations.stat_fd(operations.context, persistent_device,
                               &persistent_identity) != 0 ||
            operations.stat_at(operations.context, persistent_root,
                               selected_device.c_str(),
                               &persistent_named_identity) != 0 ||
            persistent_opened->class_link != selected_class_link ||
            bounded_sysfs_identity(persistent_named_identity) != selected_entry_identity ||
            !hwmon_named_device_matches(operations, persistent_root, selected_device.c_str(),
                persistent_identity, persistent_named_identity, selected_class_link,
                selected_entry_identity) ||
            !bounded_sysfs_directory_identity(persistent_identity) ||
            bounded_sysfs_identity(persistent_identity) !=
                selected_device_identity) {
            const auto saved = errno == 0 ? ESTALE : errno;
            append_cleanup(persistent_device_role.retire());
            append_cleanup(persistent_root_role.retire());
            return std::unexpected{sequence(source_failure(
                BoundedHwmonSourceFailureKind::enumerate, saved))};
        }
        const int persistent_attribute = operations.open_at(
            operations.context, persistent_device, selected_attribute.c_str(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (persistent_attribute < 0) {
            const auto saved = errno;
            append_cleanup(persistent_device_role.retire());
            append_cleanup(persistent_root_role.retire());
            return std::unexpected{sequence(source_failure(
                BoundedHwmonSourceFailureKind::enumerate, saved))};
        }
        DescriptorRole persistent_attribute_role{operations, persistent_attribute};
        struct stat persistent_attribute_identity{}, persistent_named_attribute{};
        errno = 0;
        if (operations.stat_fd(operations.context, persistent_attribute,
                               &persistent_attribute_identity) != 0 ||
            operations.stat_at(operations.context, persistent_device,
                               selected_attribute.c_str(),
                               &persistent_named_attribute) != 0 ||
            !bounded_sysfs_same_identity(persistent_attribute_identity,
                                         persistent_named_attribute) ||
            bounded_sysfs_identity(persistent_attribute_identity) !=
                selected_attribute_identity) {
            const auto saved = errno == 0 ? ESTALE : errno;
            append_cleanup(persistent_attribute_role.retire());
            append_cleanup(persistent_device_role.retire());
            append_cleanup(persistent_root_role.retire());
            return std::unexpected{sequence(source_failure(
                BoundedHwmonSourceFailureKind::enumerate, saved))};
        }
        state->root = persistent_root_role.release();
        state->root_identity = bounded_sysfs_identity(persistent_root_held);
        state->device = persistent_device_role.release();
        state->device_identity = bounded_sysfs_identity(persistent_identity);
        state->device_entry_identity = selected_entry_identity;
        state->class_link = selected_class_link;
        state->attribute_descriptor = persistent_attribute_role.release();
        state->attribute_identity =
            bounded_sysfs_identity(persistent_attribute_identity);
        state->device_name.assign(selected_device);
        state->attribute.assign(selected_attribute);
        state->active = true;
        return std::optional<BoundedHwmonTemperatureSource>{
            BoundedHwmonTemperatureSource{std::move(prepared.state_)}};
    }
}

auto BoundedHwmonTemperatureSource::discover_prepared(
    PreparedBoundedHwmonDiscoveryStorage&& prepared,
    BoundedHwmonValidationGate gate) noexcept
    -> std::expected<std::optional<BoundedHwmonTemperatureSource>,
                     FailedPreparedBoundedHwmonDiscovery> {
    auto result = discover_prepared_engine(prepared, gate);
    if (!result)
        return std::unexpected{FailedPreparedBoundedHwmonDiscovery{
            std::move(prepared), result.error()}};
    return std::move(*result);
}

auto BoundedHwmonTemperatureSource::discover(
    BoundedHwmonCapacity capacity, BoundedSysfsOperations operations,
    std::span<BoundedHwmonSourceFailure> cleanup_stock,
    BoundedHwmonValidationGate gate) noexcept
    -> std::expected<std::optional<BoundedHwmonTemperatureSource>,
                     BoundedHwmonSourceFailureSequence> {
    if (cleanup_stock.size() < 3U)
        return std::unexpected{source_failure(
            BoundedHwmonSourceFailureKind::enumeration_capacity)};
    auto prepared = PreparedBoundedHwmonDiscoveryStorage::prepare(
        capacity, operations);
    if (!prepared)
        return std::unexpected{prepared.error()};
    auto result = discover_prepared(std::move(*prepared), gate);
    if (result)
        return std::move(*result);
    const auto sequence = PreparedBoundedHwmonDiscoveryAccess::sequence(
        result.error());
    const auto cleanup = PreparedBoundedHwmonDiscoveryAccess::cleanup(
        result.error());
    if (!sequence || !cleanup)
        return std::unexpected{
            source_failure(BoundedHwmonSourceFailureKind::contained)};
    std::copy(cleanup->begin(), cleanup->end(), cleanup_stock.begin());
    return std::unexpected{*sequence};
}

auto BoundedHwmonTemperatureSource::read(
    std::span<char> workspace,
    std::span<BoundedHwmonSourceFailure> cleanup_stock) const noexcept
    -> std::expected<std::int64_t, BoundedHwmonSourceFailureSequence> {
    if (cleanup_stock.empty())
        return std::unexpected{source_failure(
            BoundedHwmonSourceFailureKind::enumeration_capacity)};
    if (!state_ || !state_->active ||
        workspace.size() <= state_->content_capacity)
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::read, EINVAL)};
    struct stat root{}, held{}, named_device{};
    errno = 0;
    if (state_->operations.stat_fd(state_->operations.context, state_->root, &root) != 0 ||
        bounded_sysfs_identity(root) != state_->root_identity ||
        state_->operations.stat_fd(state_->operations.context, state_->device, &held) != 0 ||
        state_->operations.stat_at(state_->operations.context, state_->root,
                                   state_->device_name.c_str(), &named_device) != 0 ||
        bounded_sysfs_identity(named_device) != state_->device_entry_identity ||
        !hwmon_named_device_matches(state_->operations, state_->root, state_->device_name.c_str(),
            held, named_device, state_->class_link, state_->device_entry_identity) ||
        bounded_sysfs_identity(held) != state_->device_identity)
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::read,
                                              errno == 0 ? ESTALE : errno)};
    struct stat attribute{}, named_attribute{};
    errno = 0;
    if (state_->operations.stat_fd(state_->operations.context,
                                   state_->attribute_descriptor,
                                   &attribute) != 0 ||
        state_->operations.stat_at(state_->operations.context, state_->device,
                                   state_->attribute.c_str(),
                                   &named_attribute) != 0 ||
        !bounded_sysfs_same_identity(attribute, named_attribute) ||
        bounded_sysfs_identity(attribute) != state_->attribute_identity)
        return std::unexpected{source_failure(
            BoundedHwmonSourceFailureKind::read,
            errno == 0 ? ESTALE : errno)};
    std::size_t used{};
    while (used < workspace.size()) {
        std::ptrdiff_t count{};
        do {
            count = state_->operations.read(
                state_->operations.context, state_->attribute_descriptor,
                workspace.data() + used, workspace.size() - used,
                static_cast<std::int64_t>(used));
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            return std::unexpected{source_failure(
                BoundedHwmonSourceFailureKind::read, errno)};
        if (count == 0)
            break;
        used += static_cast<std::size_t>(count);
    }
    struct stat final_root{}, final_device{}, final_named_device{},
        final_attribute{}, final_named_attribute{};
    errno = 0;
    if (state_->operations.stat_fd(state_->operations.context, state_->root,
                                   &final_root) != 0 ||
        bounded_sysfs_identity(final_root) != state_->root_identity ||
        state_->operations.stat_fd(state_->operations.context, state_->device,
                                   &final_device) != 0 ||
        state_->operations.stat_at(state_->operations.context, state_->root,
                                   state_->device_name.c_str(),
                                   &final_named_device) != 0 ||
        bounded_sysfs_identity(final_named_device) != state_->device_entry_identity ||
        !hwmon_named_device_matches(state_->operations, state_->root, state_->device_name.c_str(),
            final_device, final_named_device, state_->class_link, state_->device_entry_identity) ||
        bounded_sysfs_identity(final_device) != state_->device_identity ||
        state_->operations.stat_fd(state_->operations.context,
                                   state_->attribute_descriptor,
                                   &final_attribute) != 0 ||
        state_->operations.stat_at(state_->operations.context, state_->device,
                                   state_->attribute.c_str(),
                                   &final_named_attribute) != 0 ||
        !bounded_sysfs_same_identity(final_attribute, final_named_attribute) ||
        bounded_sysfs_identity(final_attribute) != state_->attribute_identity)
        return std::unexpected{source_failure(
            BoundedHwmonSourceFailureKind::read,
            errno == 0 ? ESTALE : errno)};
    if (used > state_->content_capacity)
        return std::unexpected{source_failure(
            BoundedHwmonSourceFailureKind::read, EOVERFLOW)};
    auto value = HwmonTemperatureAccess::parse_bounded(
        std::string_view{workspace.data(), used});
    if (!value)
        return std::unexpected{source_failure(BoundedHwmonSourceFailureKind::value)};
    return *value;
}

auto BoundedHwmonTemperatureSource::retire(
    std::span<BoundedSysfsDisposition> dispositions) noexcept -> std::size_t {
    if (!state_ || !state_->active)
        return 0U;
    state_->active = false;
    std::size_t count{};
    const auto record = [&](BoundedSysfsCloseOutcome outcome) noexcept {
        if (count < dispositions.size())
            dispositions[count] = {outcome.kind, outcome.native_error, true};
        ++count;
    };
    if (state_->attribute_descriptor >= 0) {
        const auto attribute = std::exchange(state_->attribute_descriptor, -1);
        record(state_->operations.close(state_->operations.context, attribute));
    }
    const auto descriptor = std::exchange(state_->device, -1);
    record(state_->operations.close(state_->operations.context, descriptor));
    const auto root = std::exchange(state_->root, -1);
    record(state_->operations.close(state_->operations.context, root));
    return count;
}

} // namespace detail

HwmonTemperatureSource::HwmonTemperatureSource(std::string input, void* context,
                                               ReadOperation read_operation) noexcept
    : input_{std::move(input)}, context_{context}, read_{read_operation} {}
HwmonTemperatureSource::HwmonTemperatureSource(HwmonTemperatureSource&&) noexcept = default;
auto HwmonTemperatureSource::operator=(HwmonTemperatureSource&&) noexcept
    -> HwmonTemperatureSource& = default;
HwmonTemperatureSource::~HwmonTemperatureSource() noexcept = default;

auto detail::HwmonTemperatureAccess::parse(std::string_view text)
    -> std::expected<std::int64_t, core::Error> {
    auto parsed = parse_integer<std::int64_t>(text, "parse_hwmon_temperature");
    if (!parsed) return std::unexpected{error("linux.hwmon_temperature.invalid_value", "value")};
    return *parsed;
}

auto detail::HwmonTemperatureAccess::parse_bounded(std::string_view text) noexcept
    -> std::optional<std::int64_t> {
    text = trim_ascii_space(text);
    std::int64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::nullopt;
    return value;
}

auto detail::HwmonTemperatureAccess::discover_bounded(
    std::uint64_t root_entries, std::uint64_t directory_count,
    std::span<const BoundedHwmonEntry> entries,
    std::uint64_t maximum_directories, std::uint64_t entries_per_directory,
    std::uint64_t token_bytes) noexcept
    -> std::expected<std::optional<std::uint32_t>, BoundedHwmonDiscoveryFailure> {
    if (root_entries > entries_per_directory || directory_count > maximum_directories)
        return std::unexpected{BoundedHwmonDiscoveryFailure::enumeration_capacity};
    std::optional<std::uint32_t> selected;
    for (std::uint64_t directory{}; directory < directory_count; ++directory) {
        std::uint64_t count{};
        bool k10temp{};
        for (const auto& entry : entries) {
            if (entry.directory != directory)
                continue;
            if (++count > entries_per_directory)
                return std::unexpected{BoundedHwmonDiscoveryFailure::enumeration_capacity};
            if (entry.name.size() > token_bytes || entry.contents.size() > token_bytes)
                return std::unexpected{BoundedHwmonDiscoveryFailure::token_capacity};
            if (entry.name == "name" && trim_ascii_space(entry.contents) == "k10temp")
                k10temp = true;
        }
        if (!k10temp)
            continue;
        for (const auto& label : entries) {
            if (label.directory != directory || !label.name.starts_with("temp") ||
                !label.name.ends_with("_label") || trim_ascii_space(label.contents) != "Tctl")
                continue;
            const auto stem = label.name.substr(0U, label.name.size() - 6U);
            const auto found = std::ranges::find_if(entries, [&](const auto& input) {
                return input.directory == directory &&
                       input.name.size() == stem.size() + 6U &&
                       input.name.starts_with(stem) && input.name.ends_with("_input");
            });
            if (found == entries.end())
                continue;
            if (selected)
                return std::unexpected{BoundedHwmonDiscoveryFailure::ambiguous};
            selected = found->locator;
        }
    }
    return selected;
}

auto detail::HwmonTemperatureAccess::discover(std::span<const HwmonFile> files, void* context,
                                              HwmonTemperatureSource::ReadOperation read)
    -> std::expected<std::optional<HwmonTemperatureSource>, core::Error> {
    try {
        std::vector<std::string> directories;
        std::vector<BoundedHwmonEntry> entries;
        std::vector<std::string> paths;
        directories.reserve(files.size());
        entries.reserve(files.size());
        paths.reserve(files.size());
        std::uint64_t token_bytes{};
        for (const auto& file : files) {
            const auto separator = file.path.find_last_of('/');
            if (separator == std::string::npos || separator + 1U == file.path.size())
                continue;
            const auto directory = file.path.substr(0U, separator + 1U);
            auto position = std::ranges::find(directories, directory);
            if (position == directories.end()) {
                directories.push_back(directory);
                position = std::prev(directories.end());
            }
            const auto locator = static_cast<std::uint32_t>(paths.size());
            paths.push_back(file.path);
            const auto name = std::string_view{file.path}.substr(separator + 1U);
            token_bytes = std::max<std::uint64_t>(
                token_bytes, std::max<std::size_t>(name.size(), file.contents.size()));
            entries.push_back({static_cast<std::uint32_t>(
                                   std::distance(directories.begin(), position)),
                               name, file.contents, locator});
        }
        const auto selected = discover_bounded(
            directories.size(), directories.size(), entries, directories.size(),
            files.size(), token_bytes);
        if (!selected) {
            if (selected.error() == BoundedHwmonDiscoveryFailure::ambiguous)
                return std::unexpected{
                    error("linux.hwmon_temperature.ambiguous_sensor", "ambiguous")};
            return std::unexpected{
                error("linux.hwmon_temperature.discovery_failed", "enumerate")};
        }
        if (!*selected)
            return std::optional<HwmonTemperatureSource>{};
        return std::optional<HwmonTemperatureSource>{HwmonTemperatureSource{
            std::move(paths[**selected]), context, read}};
    } catch (...) {
        return std::unexpected{
            error("linux.hwmon_temperature.discovery_failed", "enumerate")};
    }
}

auto HwmonTemperatureSource::discover()
    -> std::expected<std::optional<HwmonTemperatureSource>, core::Error> {
    std::vector<detail::HwmonFile> files;
    try {
        for (const auto& hwmon : std::filesystem::directory_iterator("/sys/class/hwmon")) {
            auto name = read_text_file(hwmon.path() / "name");
            if (!name) return std::unexpected{error("linux.hwmon_temperature.discovery_failed", "enumerate")};
            files.push_back({(hwmon.path() / "name").string(), std::move(*name)});
            for (const auto& item : std::filesystem::directory_iterator(hwmon.path())) {
                const auto filename = item.path().filename().string();
                if (filename.starts_with("temp") &&
                    (filename.ends_with("_label") || filename.ends_with("_input"))) {
                    auto value = read_text_file(item.path());
                    if (!value) return std::unexpected{error("linux.hwmon_temperature.discovery_failed", "enumerate")};
                    files.push_back({item.path().string(), std::move(*value)});
                }
            }
        }
    } catch (...) {
        return std::unexpected{error("linux.hwmon_temperature.discovery_failed", "enumerate")};
    }
    return detail::HwmonTemperatureAccess::discover(files, nullptr, production_read);
}

auto HwmonTemperatureSource::read() const noexcept -> std::expected<std::int64_t, core::Error> {
    auto text = read_(context_, input_);
    if (!text) return std::unexpected{std::move(text.error())};
    return detail::HwmonTemperatureAccess::parse(*text);
}
} // namespace epycbench::platform_linux
