#include "monitoring/host_q4_cpufreq_inventory_authority.hpp"
#include "platform_linux/cpu_frequency_access.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/types.h>
#include <new>
#include <ranges>
#include <string_view>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr auto root_path = "/sys/devices/system/cpu/cpufreq";
constexpr std::array<std::string_view, 7> attribute_names{
    "related_cpus",     "scaling_driver",   "scaling_governor", "scaling_min_freq",
    "scaling_max_freq", "cpuinfo_cur_freq", "scaling_cur_freq"};
constexpr int cwd_descriptor = AT_FDCWD;

struct LinuxDirectoryEntry {
    std::uint64_t inode;
    std::int64_t offset;
    unsigned short record_length;
    unsigned char type;
    char name[1];
};

auto native_open_root(void*, const char* path, int flags) noexcept -> int {
    return ::open(path, flags);
}
auto native_open_at(void*, int parent, const char* name, int flags) noexcept -> int {
    return ::openat(parent, name, flags);
}
auto native_stat_fd(void*, int descriptor, struct stat* value) noexcept -> int {
    return ::fstat(descriptor, value);
}
auto native_stat_at(void*, int parent, const char* name, struct stat* value) noexcept -> int {
    return ::fstatat(parent, name, value, AT_SYMLINK_NOFOLLOW);
}
auto native_read(void*, int descriptor, void* bytes, std::size_t count,
                 std::int64_t offset) noexcept -> ssize_t {
    return ::pread(descriptor, bytes, count, static_cast<off_t>(offset));
}
auto native_close(void*, int descriptor) noexcept -> int { return ::close(descriptor); }
auto native_duplicate(void*, int descriptor) noexcept -> int {
    return ::fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
}

auto native_next(void*, int descriptor, MonitoringQ4CpuFreqEnumerationState* state, char* output,
                 std::size_t output_size) noexcept -> int {
    for (;;) {
        if (state->cursor >= state->used) {
            const auto count =
                ::syscall(SYS_getdents64, descriptor, state->bytes.data(), state->bytes.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                return -1;
            if (count == 0)
                return 0;
            state->used = static_cast<std::size_t>(count);
            state->cursor = 0;
        }
        if (state->used - state->cursor < sizeof(LinuxDirectoryEntry)) {
            errno = EIO;
            return -1;
        }
        const auto* entry =
            reinterpret_cast<const LinuxDirectoryEntry*>(state->bytes.data() + state->cursor);
        if (entry->record_length < offsetof(LinuxDirectoryEntry, name) + 1U ||
            entry->record_length > state->used - state->cursor) {
            errno = EIO;
            return -1;
        }
        state->cursor += entry->record_length;
        const auto available =
            static_cast<std::size_t>(entry->record_length) - offsetof(LinuxDirectoryEntry, name);
        const auto length = strnlen(entry->name, available);
        if (length == available || length + 1U > output_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if ((length == 1U && entry->name[0] == '.') ||
            (length == 2U && entry->name[0] == '.' && entry->name[1] == '.'))
            continue;
        std::copy_n(entry->name, length + 1U, output);
        return 1;
    }
}

auto same_identity(const struct stat& left, const struct stat& right) noexcept -> bool {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_uid == right.st_uid && left.st_mode == right.st_mode &&
           left.st_nlink == right.st_nlink;
}
auto directory_identity(const struct stat& value) noexcept -> bool {
    return S_ISDIR(value.st_mode) && value.st_nlink > 0;
}
auto file_identity(const struct stat& value) noexcept -> bool {
    return S_ISREG(value.st_mode) && value.st_nlink == 1;
}
auto retained_identity(const struct stat& value) noexcept -> MonitoringQ4CpuFreqIdentity {
    return {static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_uid), static_cast<std::uint64_t>(value.st_mode),
            static_cast<std::uint64_t>(value.st_nlink)};
}
using Reason = MonitoringQ4CapacityReason;
auto initial_failure(MonitoringQ4CpuFreqInventoryFailureKind kind, int native = 0,
                     Reason reason = Reason::none) {
    return MonitoringQ4CpuFreqInventoryFailure{kind, native, false, MonitoringQ4CpuFreqRole::none,
                                               0,    0,      {},    false, reason};
}
auto parse_policy(std::string_view name, std::uint32_t maximum)
    -> std::expected<std::uint32_t, MonitoringQ4CpuFreqInventoryFailureKind> {
    if (!name.starts_with("policy"))
        return std::unexpected{MonitoringQ4CpuFreqInventoryFailureKind::policy_name};
    const auto digits = name.substr(6U);
    if (digits.empty() || (digits.size() > 1U && digits.front() == '0'))
        return std::unexpected{MonitoringQ4CpuFreqInventoryFailureKind::policy_name};
    std::uint64_t value{};
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || value > maximum)
        return std::unexpected{MonitoringQ4CpuFreqInventoryFailureKind::policy_name};
    return static_cast<std::uint32_t>(value);
}
} // namespace

MonitoringQ4CpuFreqInventoryAuthority::MonitoringQ4CpuFreqInventoryAuthority(
    int root, struct stat identity, Operations operations, Storage storage,
    MonitoringQ4StructuralCapacity capacity) noexcept
    : root_{root}, root_identity_{identity}, operations_{operations}, storage_{std::move(storage)},
      capacity_{std::move(capacity)} {}

MonitoringQ4CpuFreqInventoryAuthority::MonitoringQ4CpuFreqInventoryAuthority(
    MonitoringQ4CpuFreqInventoryAuthority&& other) noexcept
    : root_{std::exchange(other.root_, -1)}, root_identity_{other.root_identity_},
      operations_{other.operations_}, storage_{std::move(other.storage_)},
      capacity_{std::move(other.capacity_)}, enumeration_{other.enumeration_},
      terminal_{std::exchange(other.terminal_, true)}, binding_{std::move(other.binding_)} {}

MonitoringQ4CpuFreqInventoryAuthority::~MonitoringQ4CpuFreqInventoryAuthority() noexcept {
    if (root_ >= 0) {
        const int descriptor = std::exchange(root_, -1);
        const int result = operations_.close(operations_.context, descriptor);
        observe_close(MonitoringQ4CpuFreqRole::root, 0, 0, result == 0 ? 0 : errno);
    }
}

auto MonitoringQ4CpuFreqInventoryAuthority::retain(const MonitoringQ4SourceReady& ready) noexcept
    -> std::expected<MonitoringQ4CpuFreqInventoryAuthority, MonitoringQ4CpuFreqInventoryFailure> {
    auto result = retain_with(ready.inputs(), native_operations());
    if (result)
        result->binding_ = ready.authority().kernel_maximum_binding_;
    return result;
}

auto MonitoringQ4CpuFreqInventoryAuthority::native_operations() noexcept -> Operations {
    return {nullptr, native_open_root, native_open_at, native_stat_fd, native_stat_at,
            native_next, native_read, native_close, nullptr, native_duplicate, nullptr, nullptr,
            [](void*) noexcept { return ::geteuid(); }};
}
void MonitoringQ4CpuFreqInventoryAuthority::bind_ready(
    MonitoringQ4CpuFreqInventoryAuthority& value, const MonitoringQ4SourceReady& ready) noexcept {
    value.binding_ = ready.authority().kernel_maximum_binding_;
}

auto MonitoringQ4CpuFreqInventoryAuthority::retain_with(
    const MonitoringQ4CollectionCapacityInputs& inputs, Operations operations) noexcept
    -> std::expected<MonitoringQ4CpuFreqInventoryAuthority, MonitoringQ4CpuFreqInventoryFailure> {
    auto workspace = prepare_workspace(inputs, operations);
    if (!workspace) return std::unexpected{std::move(workspace.error())};
    return retain_prepared(inputs, operations, std::move(*workspace));
}

auto MonitoringQ4CpuFreqInventoryAuthority::prepare_workspace(
    const MonitoringQ4CollectionCapacityInputs& inputs, Operations operations) noexcept
    -> std::expected<Workspace, MonitoringQ4CpuFreqInventoryFailure> {
    Workspace workspace;
    auto& storage = workspace.storage;
    const auto& capacity = inputs.structural();
    auto allocation_reason = Reason::inventory_workspace_entry_allocation;
    try {
        if (operations.fault)
            operations.fault(operations.context, 1U);
        const auto policies = capacity.maximum_policy_directories().value;
        const auto contents = capacity.attribute_owned_bytes().value;
        const auto names = capacity.root_entry_name_bytes().value;
        if (operations.observe_cleanup &&
            policies > (std::numeric_limits<std::uint64_t>::max() - 1U) / 8U)
            return std::unexpected{
                initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::capacity, 0, Reason::inventory_cleanup_formula)};
        const auto cleanup = operations.observe_cleanup ? 1U + 8U * policies
                                                        : capacity.cleanup_events().value;
        if (policies > std::numeric_limits<std::size_t>::max())
            return std::unexpected{initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::capacity, 0, Reason::inventory_policies_size)};
        if (contents > std::numeric_limits<std::size_t>::max())
            return std::unexpected{initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::capacity, 0, Reason::inventory_contents_size)};
        if (names > std::numeric_limits<std::size_t>::max())
            return std::unexpected{initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::capacity, 0, Reason::inventory_names_size)};
        if (cleanup > std::numeric_limits<std::size_t>::max())
            return std::unexpected{initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::capacity, 0, Reason::inventory_cleanup_size)};
        allocation_reason = Reason::inventory_workspace_policies_allocation;
        storage.policies.reserve(static_cast<std::size_t>(policies));
        allocation_reason = Reason::inventory_workspace_contents_allocation;
        storage.contents.resize(static_cast<std::size_t>(contents));
        allocation_reason = Reason::inventory_workspace_names_allocation;
        storage.root_names.resize(static_cast<std::size_t>(names));
        allocation_reason = Reason::inventory_workspace_cleanup_allocation;
        storage.cleanup.reserve(static_cast<std::size_t>(cleanup));
        storage.cleanup_limit = static_cast<std::size_t>(cleanup);
        allocation_reason = Reason::inventory_workspace_finish_allocation;
        if (operations.fault)
            operations.fault(operations.context, 2U);
    } catch (const std::bad_alloc&) {
        return std::unexpected{initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::allocation, 0, allocation_reason)};
    } catch (...) {
        return std::unexpected{initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::contained)};
    }
    return workspace;
}

auto MonitoringQ4CpuFreqInventoryAuthority::retain_prepared(
    const MonitoringQ4CollectionCapacityInputs& inputs, Operations operations,
    Workspace&& workspace) noexcept
    -> std::expected<MonitoringQ4CpuFreqInventoryAuthority, MonitoringQ4CpuFreqInventoryFailure> {
    auto storage = std::move(workspace.storage);
    const auto& capacity = inputs.structural();

    const int root =
        operations.open_root(operations.context, root_path,
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_DIRECTORY);
    if (root < 0)
        return std::unexpected{
            initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::open_root, errno)};
    auto reject = [&](MonitoringQ4CpuFreqInventoryFailure value) noexcept
        -> std::expected<MonitoringQ4CpuFreqInventoryAuthority,
                         MonitoringQ4CpuFreqInventoryFailure> {
        const int saved = errno;
        const int close_result = operations.close(operations.context, root);
        if (operations.observe_cleanup)
            operations.observe_cleanup(operations.cleanup_context,
                static_cast<unsigned>(MonitoringQ4CpuFreqRole::root), 0, 0,
                close_result == 0 ? 0 : errno, close_result != 0);
        if (close_result != 0) {
            if (storage.cleanup.size() < storage.cleanup_limit)
                storage.cleanup.push_back({MonitoringQ4CpuFreqRole::root, 0, 0, errno, true});
            else
                value.cleanup_overflowed = true;
        }
        errno = saved;
        value.cleanup = std::move(storage.cleanup);
        return std::unexpected{std::move(value)};
    };
    try {
        struct stat held{}, named{};
        if (operations.stat_fd(operations.context, root, &held) != 0)
            return reject(
                initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::held_stat, errno));
        if (operations.stat_at(operations.context, cwd_descriptor, root_path, &named) != 0)
            return reject(
                initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::named_stat, errno));
        if (!directory_identity(held) || !same_identity(held, named))
            return reject(initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::identity));
        if (operations.fault)
            operations.fault(operations.context, 3U);
        return MonitoringQ4CpuFreqInventoryAuthority{root, held, operations, std::move(storage),
                                                     capacity};
    } catch (...) {
        return reject(initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::contained));
    }
}

void MonitoringQ4CpuFreqInventoryAuthority::record_close(MonitoringQ4CpuFreqRole role,
                                                         std::uint64_t policy,
                                                         std::uint64_t attribute,
                                                         int native) noexcept {
    if (storage_.cleanup.size() < storage_.cleanup_limit)
        storage_.cleanup.push_back({role, policy, attribute, native, true});
    else
        cleanup_overflowed_ = true;
}
void MonitoringQ4CpuFreqInventoryAuthority::observe_close(MonitoringQ4CpuFreqRole role,
                                                           std::uint64_t policy,
                                                           std::uint64_t attribute,
                                                           int native) noexcept {
    if (operations_.observe_cleanup)
        operations_.observe_cleanup(operations_.cleanup_context, static_cast<unsigned>(role),
                                    policy, attribute, native, native != 0);
}

auto MonitoringQ4CpuFreqInventoryAuthority::fail(MonitoringQ4CpuFreqInventoryFailureKind kind,
                                                 int native, bool uncertain,
                                                 MonitoringQ4CpuFreqRole role, std::uint64_t policy,
                                                 std::uint64_t attribute, Reason reason) noexcept
    -> std::expected<MonitoringQ4CpuFreqInventory, MonitoringQ4CpuFreqInventoryFailure> {
    auto value = initial_failure(kind, native, reason);
    value.primary_uncertain = uncertain;
    if (kind == MonitoringQ4CpuFreqInventoryFailureKind::close && uncertain) {
        value.primary_role = role;
        value.primary_policy_ordinal = policy;
        value.primary_attribute_ordinal = attribute;
    }
    terminal_ = true;
    if (root_ >= 0) {
        const int held = std::exchange(root_, -1);
        const int close_result = operations_.close(operations_.context, held);
        observe_close(MonitoringQ4CpuFreqRole::root, 0, 0,
                      close_result == 0 ? 0 : errno);
        if (close_result != 0)
            record_close(MonitoringQ4CpuFreqRole::root, 0, 0, errno);
    }
    value.cleanup = std::move(storage_.cleanup);
    value.cleanup_overflowed = cleanup_overflowed_;
    return std::unexpected{std::move(value)};
}

auto MonitoringQ4CpuFreqInventoryAuthority::capture_and_retire() noexcept
    -> std::expected<MonitoringQ4CpuFreqInventory, MonitoringQ4CpuFreqInventoryFailure> {
    return capture_with(nullptr, nullptr);
}

auto MonitoringQ4CpuFreqInventoryAuthority::capture_with(void* callback_context,
                                                          PolicyCallback callback) noexcept
    -> std::expected<MonitoringQ4CpuFreqInventory, MonitoringQ4CpuFreqInventoryFailure> {
    if (terminal_ || root_ < 0)
        return std::unexpected{initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::inactive)};
    std::size_t name_bytes{};
    try {
        struct LocalOwner final {
            MonitoringQ4CpuFreqInventoryAuthority* owner;
            int descriptor;
            MonitoringQ4CpuFreqRole role;
            std::uint64_t policy;
            std::uint64_t attribute;
            ~LocalOwner() noexcept { cleanup(); }
            void cleanup() noexcept {
                if (descriptor < 0)
                    return;
                const int held = std::exchange(descriptor, -1);
                const int result = owner->operations_.close(owner->operations_.context, held);
                owner->observe_close(role, policy, attribute, result == 0 ? 0 : errno);
                if (result != 0)
                    owner->record_close(role, policy, attribute, errno);
            }
            auto close_primary() noexcept -> int {
                const int held = std::exchange(descriptor, -1);
                const int result = owner->operations_.close(owner->operations_.context, held);
                owner->observe_close(role, policy, attribute, result == 0 ? 0 : errno);
                return result;
            }
        };
        if (operations_.fault)
            operations_.fault(operations_.context, 4U);
        std::array<char, 256> name{};
        std::uint64_t entries{};
        for (;;) {
            int result{};
            do {
                result = operations_.next(operations_.context, root_, &enumeration_, name.data(),
                                          name.size());
            } while (result < 0 && errno == EINTR);
            if (result < 0)
                return fail(MonitoringQ4CpuFreqInventoryFailureKind::enumerate, errno);
            if (result == 0)
                break;
            if (++entries > capacity_.maximum_root_entries().value)
                return fail(MonitoringQ4CpuFreqInventoryFailureKind::root_entry_limit, 0, false, MonitoringQ4CpuFreqRole::none, 0, 0, Reason::inventory_root_entries);
            const std::string_view entry{name.data()};
            if (entry.size() > storage_.root_names.size() - name_bytes)
                return fail(MonitoringQ4CpuFreqInventoryFailureKind::root_name_limit, 0, false, MonitoringQ4CpuFreqRole::none, 0, 0, Reason::inventory_root_names);
            std::copy(entry.begin(), entry.end(),
                      storage_.root_names.begin() + static_cast<std::ptrdiff_t>(name_bytes));
            name_bytes += entry.size();
            if (entry == "boost")
                continue;
            if (!entry.starts_with("policy")) {
                continue;
            }
            const auto parsed =
                parse_policy(entry, static_cast<std::uint32_t>(capacity_.maximum_cpu_id().value));
            if (!parsed)
                return fail(parsed.error());
            if (storage_.policies.size() >= storage_.policies.capacity())
                return fail(MonitoringQ4CpuFreqInventoryFailureKind::policy_limit, 0, false, MonitoringQ4CpuFreqRole::none, 0, 0, Reason::inventory_policy_slots);
            if (std::ranges::any_of(storage_.policies, [&](const auto& policy) {
                    return policy.policy_number == *parsed;
                }))
                return fail(MonitoringQ4CpuFreqInventoryFailureKind::policy_name);
            storage_.policies.push_back({*parsed, {}, {}});
        }
        std::ranges::sort(storage_.policies, {},
                          &MonitoringQ4CpuFreqPolicyInventory::policy_number);

        std::size_t content_slot{};
        const auto stride = static_cast<std::size_t>(capacity_.per_file_probe_bytes().value);
        for (std::size_t policy_index{}; policy_index < storage_.policies.size(); ++policy_index) {
            const auto number = storage_.policies[policy_index].policy_number;
            std::array<char, 17> policy_name{};
            const auto encoded = std::to_chars(policy_name.data() + 6U,
                                               policy_name.data() + policy_name.size(), number);
            std::copy_n("policy", 6U, policy_name.data());
            *encoded.ptr = '\0';
            const int policy =
                operations_.open_at(operations_.context, root_, policy_name.data(),
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_DIRECTORY);
            if (policy < 0)
                return fail(MonitoringQ4CpuFreqInventoryFailureKind::open_policy, errno);
            LocalOwner policy_owner{this, policy, MonitoringQ4CpuFreqRole::policy,
                                    policy_index + 1U, 0};
            auto retire_policy = [&](MonitoringQ4CpuFreqInventoryFailureKind kind,
                                     int native = 0, Reason reason = Reason::none) noexcept {
                policy_owner.cleanup();
                return fail(kind, native, false, MonitoringQ4CpuFreqRole::none, 0, 0, reason);
            };
            if (operations_.fault)
                operations_.fault(operations_.context, 5U);
            struct stat policy_held{}, policy_named{};
            if (operations_.stat_fd(operations_.context, policy, &policy_held) != 0)
                return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::held_stat, errno);
            if (operations_.stat_at(operations_.context, root_, policy_name.data(),
                                    &policy_named) != 0)
                return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::named_stat, errno);
            if (!directory_identity(policy_held) || !same_identity(policy_held, policy_named))
                return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::identity);
            storage_.policies[policy_index].identity = retained_identity(policy_held);

            for (std::size_t attribute_index{}; attribute_index < attribute_names.size();
                 ++attribute_index, ++content_slot) {
                const auto attribute_name = attribute_names[attribute_index];
                const int attribute =
                    operations_.open_at(operations_.context, policy, attribute_name.data(),
                                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                if (attribute < 0) {
                    const int open_error = errno;
                    if (open_error == ENOENT && attribute_index >= 5U)
                        continue;
                    const auto effective_uid = open_error == EACCES && attribute_index == 5U &&
                                                       operations_.effective_uid
                                                   ? operations_.effective_uid(operations_.context)
                                                   : uid_t{0};
                    if (open_error == EACCES && attribute_index == 5U && effective_uid != 0) {
                        struct stat first{}, second{};
                        if (operations_.stat_at(operations_.context, policy, attribute_name.data(),
                                                &first) != 0)
                            return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::named_stat,
                                                 errno);
                        if (operations_.stat_at(operations_.context, policy, attribute_name.data(),
                                                &second) != 0)
                            return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::named_stat,
                                                 errno);
                        if (!platform_linux::detail::cpu_frequency_unreadable_hardware_identity(
                                first, second, effective_uid))
                            return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::identity);
                        auto& stored = storage_.policies[policy_index].attributes[attribute_index];
                        stored.present = true;
                        stored.hardware_unreadable = true;
                        stored.identity = retained_identity(first);
                        continue;
                    }
                    return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::open_attribute,
                                         open_error);
                }
                LocalOwner attribute_owner{this, attribute, MonitoringQ4CpuFreqRole::attribute,
                                           policy_index + 1U, attribute_index + 1U};
                auto retire_attribute = [&](MonitoringQ4CpuFreqInventoryFailureKind kind,
                                            int native = 0, Reason reason = Reason::none) noexcept {
                    attribute_owner.cleanup();
                    return retire_policy(kind, native, reason);
                };
                struct stat held{}, named{};
                if (operations_.stat_fd(operations_.context, attribute, &held) != 0)
                    return retire_attribute(MonitoringQ4CpuFreqInventoryFailureKind::held_stat,
                                            errno);
                if (operations_.stat_at(operations_.context, policy, attribute_name.data(),
                                        &named) != 0)
                    return retire_attribute(MonitoringQ4CpuFreqInventoryFailureKind::named_stat,
                                            errno);
                if (!file_identity(held) || !same_identity(held, named))
                    return retire_attribute(MonitoringQ4CpuFreqInventoryFailureKind::identity);
                const auto offset = content_slot * stride;
                std::size_t used{};
                while (used < stride - 1U) {
                    const auto count = operations_.read(
                        operations_.context, attribute, storage_.contents.data() + offset + used,
                        stride - 1U - used, static_cast<std::int64_t>(used));
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count < 0)
                        return retire_attribute(MonitoringQ4CpuFreqInventoryFailureKind::read,
                                                errno);
                    if (count == 0)
                        break;
                    if (static_cast<std::size_t>(count) > stride - 1U - used)
                        return retire_attribute(
                            MonitoringQ4CpuFreqInventoryFailureKind::read_contract);
                    used += static_cast<std::size_t>(count);
                }
                char probe{};
                for (;;) {
                    const auto count = operations_.read(operations_.context, attribute, &probe, 1,
                                                        static_cast<std::int64_t>(used));
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count < 0)
                        return retire_attribute(MonitoringQ4CpuFreqInventoryFailureKind::read,
                                                errno);
                    if (count > 1)
                        return retire_attribute(
                            MonitoringQ4CpuFreqInventoryFailureKind::read_contract);
                    if (count != 0)
                        return retire_attribute(
                            MonitoringQ4CpuFreqInventoryFailureKind::byte_limit, 0, Reason::inventory_attribute_bytes);
                    break;
                }
                struct stat after{}, named_after{};
                if (operations_.stat_fd(operations_.context, attribute, &after) != 0)
                    return retire_attribute(MonitoringQ4CpuFreqInventoryFailureKind::held_stat,
                                            errno);
                if (operations_.stat_at(operations_.context, policy, attribute_name.data(),
                                        &named_after) != 0)
                    return retire_attribute(MonitoringQ4CpuFreqInventoryFailureKind::named_stat,
                                            errno);
                if (!file_identity(after) || !same_identity(held, after) ||
                    !same_identity(held, named_after))
                    return retire_attribute(MonitoringQ4CpuFreqInventoryFailureKind::identity);
                if (operations_.fault)
                    operations_.fault(operations_.context, 6U);
                if (attribute_owner.close_primary() != 0) {
                    const int native = errno;
                    policy_owner.cleanup();
                    return fail(MonitoringQ4CpuFreqInventoryFailureKind::close, native, true,
                                MonitoringQ4CpuFreqRole::attribute, policy_index + 1U,
                                attribute_index + 1U);
                }
                storage_.policies[policy_index].attributes[attribute_index] = {
                    true, false, static_cast<std::uint64_t>(offset), static_cast<std::uint64_t>(used),
                    retained_identity(held)};
            }
            if (callback && callback(callback_context, policy, policy_index + 1U,
                                     storage_.policies[policy_index], storage_.contents) != 0)
                return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::policy_callback,
                                     errno);
            struct stat after{}, named_after{};
            if (operations_.stat_fd(operations_.context, policy, &after) != 0)
                return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::held_stat, errno);
            if (operations_.stat_at(operations_.context, root_, policy_name.data(), &named_after) !=
                0)
                return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::named_stat, errno);
            if (!directory_identity(after) || !same_identity(policy_held, after) ||
                !same_identity(policy_held, named_after))
                return retire_policy(MonitoringQ4CpuFreqInventoryFailureKind::identity);
            if (policy_owner.close_primary() != 0) {
                return fail(MonitoringQ4CpuFreqInventoryFailureKind::close, errno, true,
                            MonitoringQ4CpuFreqRole::policy, policy_index + 1U, 0);
            }
        }
        struct stat root_after{}, root_named{};
        if (operations_.stat_fd(operations_.context, root_, &root_after) != 0)
            return fail(MonitoringQ4CpuFreqInventoryFailureKind::held_stat, errno);
        if (operations_.stat_at(operations_.context, cwd_descriptor, root_path, &root_named) != 0)
            return fail(MonitoringQ4CpuFreqInventoryFailureKind::named_stat, errno);
        if (!directory_identity(root_after) || !same_identity(root_identity_, root_after) ||
            !same_identity(root_identity_, root_named))
            return fail(MonitoringQ4CpuFreqInventoryFailureKind::identity);
        const int held_root = std::exchange(root_, -1);
        terminal_ = true;
        const int close_result = operations_.close(operations_.context, held_root);
        observe_close(MonitoringQ4CpuFreqRole::root, 0, 0,
                      close_result == 0 ? 0 : errno);
        if (close_result != 0) {
            auto value = initial_failure(MonitoringQ4CpuFreqInventoryFailureKind::close, errno);
            value.primary_uncertain = true;
            value.primary_role = MonitoringQ4CpuFreqRole::root;
            return std::unexpected{std::move(value)};
        }
        storage_.contents.resize(content_slot * stride);
        return MonitoringQ4CpuFreqInventory{retained_identity(root_identity_),
                                            std::move(storage_.policies),
                                            std::move(storage_.contents), std::move(binding_)};
    } catch (const std::bad_alloc&) {
        return fail(MonitoringQ4CpuFreqInventoryFailureKind::allocation, 0, false, MonitoringQ4CpuFreqRole::none, 0, 0, Reason::inventory_capture_allocation);
    } catch (...) {
        return fail(MonitoringQ4CpuFreqInventoryFailureKind::contained);
    }
}

} // namespace epycbench::monitoring::qualification::detail
