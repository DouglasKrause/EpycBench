#include "platform_linux/host_work_admission.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <linux/magic.h>
#include <new>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux::host_work {
namespace {
using Raw = detail::RawStatusResult;
constexpr std::uint64_t fixed_global_headroom = UINT64_C(42949672960);
constexpr std::uint64_t fixed_node_headroom = UINT64_C(4294967296);
constexpr std::uint64_t duration = UINT64_C(5000000000);
constexpr std::array<std::uint64_t, 3> ceilings{
    UINT64_C(21474836480), UINT64_C(8388608), UINT64_C(1024)};
constexpr auto gibibyte = UINT64_C(1073741824);
constexpr Resources stock{4, 32, 8, 2, 1, 1024,
    core::ByteCount::from_base_units(20 * gibibyte),
    core::ByteCount::from_base_units(16 * gibibyte),
    core::ByteCount::from_base_units(4 * gibibyte),
    core::ByteCount::from_base_units(20 * gibibyte)};
auto empty(Resources value) noexcept -> bool {
    return value.processes == 0 && value.threads == 0 && value.measurement_workers == 0 &&
        value.service_roles == 0 && value.q2_helpers == 0 && value.descriptors == 0 &&
        value.stack_bytes.value() == 0 && value.mapped_bytes.value() == 0 &&
        value.ordinary_bytes.value() == 0 && value.address_space_bytes.value() == 0;
}
auto valid_roles(Resources value) noexcept -> bool {
    if (value.processes > value.threads || value.measurement_workers > value.threads) return false;
    const auto nonmeasurement = value.threads - value.measurement_workers;
    return value.service_roles <= nonmeasurement &&
        value.q2_helpers <= nonmeasurement - value.service_roles;
}
auto charged_bytes(Resources value) noexcept -> std::expected<core::ByteCount, Reason> {
    const auto stack_and_mapping = value.stack_bytes.checked_add(value.mapped_bytes);
    if (!stack_and_mapping) return std::unexpected{Reason::arithmetic};
    const auto total = stack_and_mapping->checked_add(value.ordinary_bytes);
    if (!total) return std::unexpected{Reason::arithmetic};
    return *total;
}
auto succeeded(Raw result) noexcept -> bool { return result.value == 0 && result.error_number == 0; }
class Deadline final {
  public:
    explicit Deadline(GuardOperations operations) noexcept : operations_{operations} {}
    auto start() noexcept -> std::expected<void, Reason> {
        if (!operations_.now || !operations_.cancelled) return std::unexpected{Reason::invalid_input};
        if (operations_.cancelled(operations_.context)) return std::unexpected{Reason::cancelled};
        if (!succeeded(operations_.now(operations_.context, &last_)) ||
            last_ > UINT64_MAX - duration) return std::unexpected{Reason::clock};
        end_ = last_ + duration;
        return check();
    }
    auto check() noexcept -> std::expected<void, Reason> {
        if (operations_.cancelled(operations_.context)) return std::unexpected{Reason::cancelled};
        std::uint64_t now{};
        if (!succeeded(operations_.now(operations_.context, &now)) || now < last_)
            return std::unexpected{Reason::clock};
        last_ = now;
        if (now >= end_) return std::unexpected{Reason::deadline};
        if (operations_.cancelled(operations_.context)) return std::unexpected{Reason::cancelled};
        return {};
    }
  private:
    GuardOperations operations_;
    std::uint64_t last_{}, end_{};
};
auto trim(std::string_view value) noexcept -> std::string_view {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}
auto quantity(std::string_view value) noexcept -> std::expected<std::uint64_t, Reason> {
    value = trim(value);
    const auto split = value.find_first_of(" \t");
    if (split == std::string_view::npos || trim(value.substr(split)) != "kB")
        return std::unexpected{Reason::parse};
    const auto number = value.substr(0, split);
    if (number.empty() || number.front() < '0' || number.front() > '9')
        return std::unexpected{Reason::parse};
    std::uint64_t parsed{};
    const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
    if (result.ec == std::errc::result_out_of_range || parsed > UINT64_MAX / 1024)
        return std::unexpected{Reason::arithmetic};
    if (result.ec != std::errc{} || result.ptr != number.data() + number.size())
        return std::unexpected{Reason::parse};
    return parsed * 1024;
}
auto authenticated(const Metadata& metadata, Source source, bool directory) noexcept -> bool {
    return metadata.known_filesystem && metadata.filesystem == source && metadata.uid == 0 &&
        metadata.links > 0 && (metadata.mode & (S_IWGRP | S_IWOTH)) == 0 &&
        (metadata.mode & S_IFMT) == static_cast<std::uint64_t>(directory ? S_IFDIR : S_IFREG);
}
auto native_now(void*, std::uint64_t* output) noexcept -> Raw {
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) return {-1, errno};
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L)
        return {-1, EINVAL};
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    if (seconds > (UINT64_MAX - static_cast<std::uint64_t>(value.tv_nsec)) / UINT64_C(1000000000))
        return {-1, EOVERFLOW};
    *output = seconds * UINT64_C(1000000000) + static_cast<std::uint64_t>(value.tv_nsec);
    return {0, 0};
}
auto not_cancelled(void*) noexcept -> bool { return false; }
auto native_metadata(void*, int fd, Metadata* output) noexcept -> Raw {
    struct stat info{};
    struct statfs filesystem{};
    if (::fstat(fd, &info) != 0 || ::fstatfs(fd, &filesystem) != 0) return {-1, errno};
    *output = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
        info.st_uid, info.st_mode, info.st_nlink,
        filesystem.f_type == PROC_SUPER_MAGIC ? Source::proc : Source::sysfs,
        filesystem.f_type == PROC_SUPER_MAGIC || filesystem.f_type == SYSFS_MAGIC};
    return {0, 0};
}
auto native_resource(NativeLimit resource) noexcept -> int {
    switch (resource) {
    case NativeLimit::address_space: return RLIMIT_AS;
    case NativeLimit::stack: return RLIMIT_STACK;
    case NativeLimit::descriptors: return RLIMIT_NOFILE;
    }
    return RLIMIT_AS;
}
} // namespace

auto parse_memory_bytes(std::string_view input, std::optional<core::LinuxNumaNodeId> node) noexcept
    -> std::expected<core::ByteCount, Reason> {
    if (input.size() > 65536) return std::unexpected{Reason::capacity};
    if (input.find('\0') != std::string_view::npos) return std::unexpected{Reason::parse};
    std::optional<std::uint64_t> selected;
    while (!input.empty()) {
        const auto end = input.find('\n');
        auto line = input.substr(0, end);
        if (end == std::string_view::npos) input = {};
        else input.remove_prefix(end + 1);
        std::string_view value;
        if (!node) {
            if (!line.starts_with("MemAvailable:")) continue;
            value = line.substr(13);
        } else {
            if (line.find("MemFree:") == std::string_view::npos) continue;
            if (!line.starts_with("Node ")) return std::unexpected{Reason::parse};
            line.remove_prefix(5);
            const auto split = line.find(' ');
            if (split == std::string_view::npos) return std::unexpected{Reason::parse};
            std::uint32_t observed{};
            const auto parsed = std::from_chars(line.data(), line.data() + split, observed);
            if (parsed.ec != std::errc{} || parsed.ptr != line.data() + split)
                return std::unexpected{Reason::parse};
            if (observed != node->value()) return std::unexpected{Reason::wrong_node};
            line = trim(line.substr(split));
            if (!line.starts_with("MemFree:")) return std::unexpected{Reason::parse};
            value = line.substr(8);
        }
        if (selected) return std::unexpected{Reason::duplicate};
        auto parsed = quantity(value);
        if (!parsed) return std::unexpected{parsed.error()};
        selected = *parsed;
    }
    if (!selected) return std::unexpected{Reason::missing};
    return core::ByteCount::from_base_units(*selected);
}

auto check_memory_headroom(core::ByteCount bytes, bool node) noexcept -> std::expected<void, Reason> {
    const auto required = node ? fixed_node_headroom : fixed_global_headroom;
    if (bytes.value() < required) return std::unexpected{Reason::headroom};
    return {};
}
auto production_read_operations() noexcept -> ReadOperations {
    return {nullptr,
        [](void*, Source source) noexcept -> Raw {
            const int fd = ::open(source == Source::proc ? "/proc" : "/sys/devices/system/node",
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            return {fd, fd < 0 ? errno : 0};
        },
        [](void*, int parent, const char* name, bool directory) noexcept -> Raw {
            const int fd = ::openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                                               (directory ? O_DIRECTORY : 0));
            return {fd, fd < 0 ? errno : 0};
        }, native_metadata,
        [](void*, int fd, std::span<char> output) noexcept -> Raw {
            const auto count = ::read(fd, output.data(), output.size());
            return {count, count < 0 ? errno : 0};
        },
        [](void*, int fd) noexcept -> Raw {
            const auto result = ::close(fd); return {result, result < 0 ? errno : 0};
        }, {nullptr, native_now, not_cancelled}};
}

auto read_headroom(core::ByteCount minimum_global, std::span<const NodeMemoryDemand> nodes,
    const ReadOperations& operations, ReadLimits limits) noexcept
    -> std::expected<HeadroomReceipt, Failure> {
    if (nodes.size() > 1024 || !operations.open_root || !operations.open_at ||
        !operations.metadata || !operations.read || !operations.close ||
        limits.bytes == 0 || limits.bytes > 65536 || limits.calls == 0 || limits.calls > 1024)
        return std::unexpected{Failure{Reason::invalid_input}};
    for (std::size_t i = 0; i < nodes.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (nodes[i].node == nodes[j].node) return std::unexpected{Failure{Reason::invalid_input}};
    Deadline deadline{operations.guard};
    auto ready = deadline.start();
    if (!ready) return std::unexpected{Failure{ready.error()}};
    std::array<int, 4> owned{-1, -1, -1, -1};
    std::optional<Failure> failure;
    const auto fail = [&](Reason reason) {
        if (!failure) failure = Failure{reason};
    };
    const auto close = [&](std::size_t slot) {
        const int fd = std::exchange(owned[slot], -1);
        if (fd < 0) return;
        const auto before = deadline.check();
        if (!before) fail(before.error());
        // Expiry/cancellation never suppresses the one consuming close attempt.
        if (!succeeded(operations.close(operations.context, fd))) {
            if (!failure) failure = Failure{Reason::close};
            else if (failure->cleanup_count < failure->cleanup.size())
                failure->cleanup[failure->cleanup_count++] = Reason::close;
        }
        const auto checked = deadline.check();
        if (!checked) fail(checked.error());
    };
    const auto open = [&](Raw result, std::size_t slot) -> bool {
        if (result.value >= 0 && result.value <= std::numeric_limits<int>::max()) {
            const auto value = static_cast<int>(result.value);
            if (std::ranges::find(owned, value) != owned.end()) { fail(Reason::progress); return false; }
            owned[slot] = value;
        }
        if (owned[slot] < 0 || result.error_number) fail(Reason::open);
        const auto checked = deadline.check();
        if (!checked) { fail(checked.error()); return false; }
        return !failure;
    };
    const auto metadata = [&](std::size_t slot, Source source, bool directory,
                              Metadata& output) -> bool {
        const auto before = deadline.check();
        if (!before) { fail(before.error()); return false; }
        if (!succeeded(operations.metadata(operations.context, owned[slot], &output))) fail(Reason::metadata);
        const auto after = deadline.check();
        if (!after) { fail(after.error()); return false; }
        if (failure) return false;
        if (!authenticated(output, source, directory)) { fail(Reason::authentication); return false; }
        return true;
    };
    const auto file = [&](std::size_t parent, Source source,
                          std::optional<core::LinuxNumaNodeId> node, core::ByteCount minimum) -> bool {
        auto checked = deadline.check();
        if (!checked) { fail(checked.error()); return false; }
        if (!open(operations.open_at(operations.context, owned[parent], "meminfo", false), 3)) return false;
        Metadata initial{}, final{};
        if (!metadata(3, source, false, initial)) return false;
        std::array<char, 65537> text{};
        std::size_t used{}, calls{};
        for (;;) {
            checked = deadline.check();
            if (!checked) { fail(checked.error()); return false; }
            if (calls++ == limits.calls) { fail(Reason::capacity); return false; }
            const auto available = limits.bytes + 1 - used;
            const auto result = operations.read(operations.context, owned[3],
                                                std::span{text}.subspan(used, available));
            if (result.value == -1 && result.error_number == EINTR) {
                checked = deadline.check();
                if (!checked) { fail(checked.error()); return false; }
                continue;
            }
            if (result.value < 0 || result.error_number) fail(Reason::read);
            else if (static_cast<std::uint64_t>(result.value) > available) fail(Reason::progress);
            checked = deadline.check();
            if (!checked) { fail(checked.error()); return false; }
            if (failure) return false;
            used += static_cast<std::size_t>(result.value);
            if (used > limits.bytes) { fail(Reason::capacity); return false; }
            if (result.value == 0) break;
        }
        if (!metadata(3, source, false, final)) return false;
        if (initial != final) { fail(Reason::authentication); return false; }
        const auto parsed = parse_memory_bytes({text.data(), used}, node);
        if (!parsed) { fail(parsed.error()); return false; }
        if (*parsed < minimum) { fail(Reason::headroom); return false; }
        close(3);
        return !failure;
    };
    const auto run = [&]() {
        Metadata ignored{};
        if (!open(operations.open_root(operations.context, Source::proc), 0) ||
            !metadata(0, Source::proc, true, ignored) || !file(0, Source::proc, std::nullopt, minimum_global)) return;
        close(0);
        if (failure || nodes.empty()) return;
        const auto checked = deadline.check();
        if (!checked) { fail(checked.error()); return; }
        if (!open(operations.open_root(operations.context, Source::sysfs), 1) ||
            !metadata(1, Source::sysfs, true, ignored)) return;
        for (const auto& demand : nodes) {
            auto next = deadline.check();
            if (!next) { fail(next.error()); return; }
            std::array<char, 32> name{'n', 'o', 'd', 'e'};
            const auto rendered = std::to_chars(name.data() + 4, name.data() + name.size() - 1, demand.node.value());
            if (rendered.ec != std::errc{}) { fail(Reason::arithmetic); return; }
            if (!open(operations.open_at(operations.context, owned[1], name.data(), true), 2) ||
                !metadata(2, Source::sysfs, true, ignored) || !file(2, Source::sysfs, demand.node, demand.minimum)) return;
            close(2);
            if (failure) return;
        }
    };
    run();
    for (std::size_t i = owned.size(); i != 0; --i) close(i - 1);
    if (failure) return std::unexpected{*failure};
    return HeadroomReceipt{nodes.size(), true, true};
}

auto read_headroom(std::span<const core::LinuxNumaNodeId> nodes,
    const ReadOperations& operations, ReadLimits limits) noexcept
    -> std::expected<HeadroomReceipt, Failure> {
    if (nodes.empty() || nodes.size() > 8) return std::unexpected{Failure{Reason::invalid_input}};
    using Node = core::LinuxNumaNodeId;
    const NodeMemoryDemand empty{Node{0}, core::ByteCount::from_base_units(0)};
    std::array<NodeMemoryDemand, 8> demands{empty, empty, empty, empty, empty, empty, empty, empty};
    for (std::size_t i = 0; i < nodes.size(); ++i)
        demands[i] = {nodes[i], core::ByteCount::from_base_units(fixed_node_headroom)};
    return read_headroom(core::ByteCount::from_base_units(fixed_global_headroom),
                         std::span{demands}.first(nodes.size()), operations, limits);
}

struct ResourceLease::State final { Resources outstanding; Resources maximum{stock}; bool poisoned{}; };
ResourceLease::ResourceLease(std::shared_ptr<State> state, Resources resources) noexcept
    : state_{std::move(state)}, charged_{resources} {}
ResourceLease::ResourceLease(ResourceLease&& other) noexcept
    : state_{std::move(other.state_)}, charged_{other.charged_} {}
auto ResourceLease::operator=(ResourceLease&& other) noexcept -> ResourceLease& {
    if (this != &other) { abandon(); state_ = std::move(other.state_); charged_ = other.charged_; }
    return *this;
}
void ResourceLease::abandon() noexcept { if (state_) { state_->poisoned = true; state_.reset(); } }
ResourceLease::~ResourceLease() noexcept { abandon(); }
auto ResourceLease::active() const noexcept -> bool { return state_ != nullptr; }
auto ResourceLease::retire() noexcept -> std::expected<void, Reason> {
    if (!state_) return std::unexpected{Reason::invalid_state};
    auto& value = state_->outstanding;
    value.processes -= charged_.processes; value.threads -= charged_.threads;
    value.measurement_workers -= charged_.measurement_workers;
    value.service_roles -= charged_.service_roles; value.q2_helpers -= charged_.q2_helpers;
    value.descriptors -= charged_.descriptors;
    value.stack_bytes = core::ByteCount::from_base_units(value.stack_bytes.value() - charged_.stack_bytes.value());
    value.mapped_bytes = core::ByteCount::from_base_units(value.mapped_bytes.value() - charged_.mapped_bytes.value());
    value.ordinary_bytes = core::ByteCount::from_base_units(value.ordinary_bytes.value() - charged_.ordinary_bytes.value());
    value.address_space_bytes = core::ByteCount::from_base_units(
        value.address_space_bytes.value() - charged_.address_space_bytes.value());
    state_.reset();
    return {};
}
ResourceLedger::ResourceLedger(std::shared_ptr<ResourceLease::State> state) noexcept : state_{std::move(state)} {}
auto ResourceLedger::prepare() noexcept -> std::expected<ResourceLedger, Reason> {
    return prepare(stock);
}
auto ResourceLedger::prepare(Resources quota) noexcept -> std::expected<ResourceLedger, Reason> {
    if (quota.processes > stock.processes || quota.threads > stock.threads ||
        quota.measurement_workers > stock.measurement_workers ||
        quota.service_roles > stock.service_roles || quota.q2_helpers > stock.q2_helpers ||
        quota.descriptors > stock.descriptors || quota.stack_bytes > stock.stack_bytes ||
        quota.mapped_bytes > stock.mapped_bytes || quota.ordinary_bytes > stock.ordinary_bytes ||
        quota.address_space_bytes > stock.address_space_bytes)
        return std::unexpected{Reason::capacity};
    if (empty(quota) || !valid_roles(quota) || quota.stack_bytes > quota.address_space_bytes ||
        quota.mapped_bytes > quota.address_space_bytes || quota.ordinary_bytes > quota.address_space_bytes)
        return std::unexpected{Reason::invalid_input};
    try {
        auto state = std::make_shared<ResourceLease::State>();
        state->maximum = quota;
        return ResourceLedger{std::move(state)};
    }
    catch (const std::bad_alloc&) { return std::unexpected{Reason::allocation}; }
}
auto ResourceLedger::outstanding() const noexcept -> Resources { return state_ ? state_->outstanding : Resources{}; }
auto ResourceLedger::poisoned() const noexcept -> bool { return !state_ || state_->poisoned; }
auto ResourceLedger::reserve(Resources requested) noexcept -> std::expected<ResourceLease, Reason> {
    if (!state_) return std::unexpected{Reason::invalid_state};
    if (state_->poisoned) return std::unexpected{Reason::abandoned};
    const auto total = charged_bytes(requested);
    if (!total) return std::unexpected{total.error()};
    if (requested.address_space_bytes != *total) return std::unexpected{Reason::invalid_input};
    const std::array wanted{requested.processes, requested.threads, requested.measurement_workers,
                            requested.service_roles, requested.q2_helpers, requested.descriptors};
    const auto ceiling = state_->maximum;
    const std::array maximum{ceiling.processes, ceiling.threads, ceiling.measurement_workers,
        ceiling.service_roles, ceiling.q2_helpers, ceiling.descriptors};
    const auto current = state_->outstanding;
    const std::array used{current.processes, current.threads, current.measurement_workers,
        current.service_roles, current.q2_helpers, current.descriptors};
    for (std::size_t i = 0; i < wanted.size(); ++i)
        if (wanted[i] > maximum[i] - used[i]) return std::unexpected{Reason::capacity};
    const std::array byte_wanted{requested.stack_bytes, requested.mapped_bytes,
        requested.ordinary_bytes, requested.address_space_bytes};
    const std::array byte_maximum{ceiling.stack_bytes, ceiling.mapped_bytes,
        ceiling.ordinary_bytes, ceiling.address_space_bytes};
    const std::array byte_used{current.stack_bytes, current.mapped_bytes,
        current.ordinary_bytes, current.address_space_bytes};
    for (std::size_t i = 0; i < byte_wanted.size(); ++i)
        if (byte_wanted[i].value() > byte_maximum[i].value() - byte_used[i].value())
            return std::unexpected{Reason::capacity};
    if (empty(requested) || !valid_roles(requested))
        return std::unexpected{Reason::invalid_input};
    state_->outstanding = {current.processes + requested.processes, current.threads + requested.threads,
        current.measurement_workers + requested.measurement_workers, current.service_roles + requested.service_roles,
        current.q2_helpers + requested.q2_helpers, current.descriptors + requested.descriptors,
        core::ByteCount::from_base_units(current.stack_bytes.value() + requested.stack_bytes.value()),
        core::ByteCount::from_base_units(current.mapped_bytes.value() + requested.mapped_bytes.value()),
        core::ByteCount::from_base_units(current.ordinary_bytes.value() + requested.ordinary_bytes.value()),
        core::ByteCount::from_base_units(current.address_space_bytes.value() + requested.address_space_bytes.value())};
    return ResourceLease{state_, requested};
}

ChildLimitAdmission::ChildLimitAdmission(int parent, LimitDemand demand) noexcept : parent_{parent}, demand_{demand} {}
ChildLimitAdmission::ChildLimitAdmission(ChildLimitAdmission&& other) noexcept
    : parent_{std::exchange(other.parent_, 0)}, demand_{other.demand_} {}
auto ChildLimitAdmission::operator=(ChildLimitAdmission&& other) noexcept -> ChildLimitAdmission& {
    if (this != &other) { parent_ = std::exchange(other.parent_, 0); demand_ = other.demand_; }
    return *this;
}
auto check_limit_demand(LimitDemand demand) noexcept -> std::expected<void, Reason> {
    if (demand.address_space.value() == 0 || demand.stack.value() == 0 || demand.descriptors == 0)
        return std::unexpected{Reason::invalid_input};
    if (demand.address_space.value() > ceilings[0] || demand.stack.value() > ceilings[1] || demand.descriptors > ceilings[2])
        return std::unexpected{Reason::capacity};
    return {};
}
auto ChildLimitAdmission::prepare(int parent, LimitDemand demand) noexcept
    -> std::expected<ChildLimitAdmission, Reason> {
    if (parent <= 0) return std::unexpected{Reason::invalid_input};
    const auto checked = check_limit_demand(demand);
    if (!checked) return std::unexpected{checked.error()};
    return ChildLimitAdmission{parent, demand};
}
auto ChildLimitAdmission::apply(const LimitOperations& operations) noexcept -> std::expected<LimitReceipt, Reason> {
    const int expected_parent = std::exchange(parent_, 0);
    if (!expected_parent) return std::unexpected{Reason::invalid_state};
    if (!operations.identity || !operations.get || !operations.set || operations.maximum_finite == 0)
        return std::unexpected{Reason::invalid_input};
    Deadline deadline{operations.guard};
    auto checked = deadline.start(); if (!checked) return std::unexpected{checked.error()};
    int pid{}, parent{}; std::uint64_t uid{};
    if (!succeeded(operations.identity(operations.context, &pid, &parent, &uid)))
        return std::unexpected{Reason::authentication};
    checked = deadline.check(); if (!checked) return std::unexpected{checked.error()};
    if (pid <= 0 || parent != expected_parent || pid == parent || uid == 0)
        return std::unexpected{Reason::authentication};
    LimitReceipt receipt;
    const std::array demands{demand_.address_space.value(), demand_.stack.value(),
                            static_cast<std::uint64_t>(demand_.descriptors)};
    for (std::size_t i = 0; i < receipt.limits.size(); ++i) {
        checked = deadline.check(); if (!checked) return std::unexpected{checked.error()};
        LimitPair existing{};
        if (!succeeded(operations.get(operations.context, static_cast<NativeLimit>(i), &existing)))
            return std::unexpected{Reason::limit_get};
        checked = deadline.check(); if (!checked) return std::unexpected{checked.error()};
        if (existing.soft > existing.hard ||
            (existing.hard != UINT64_MAX && existing.hard > operations.maximum_finite) ||
            (existing.soft != UINT64_MAX && existing.soft > operations.maximum_finite))
            return std::unexpected{Reason::authentication};
        receipt.limits[i] = {std::min(existing.soft, ceilings[i]), std::min(existing.hard, ceilings[i])};
        if (receipt.limits[i].hard > operations.maximum_finite || demands[i] > receipt.limits[i].soft)
            return std::unexpected{Reason::capacity};
    }
    for (std::size_t i = 0; i < receipt.limits.size(); ++i) {
        checked = deadline.check(); if (!checked) return std::unexpected{checked.error()};
        if (!succeeded(operations.set(operations.context, static_cast<NativeLimit>(i), receipt.limits[i])))
            return std::unexpected{Reason::limit_set};
        checked = deadline.check(); if (!checked) return std::unexpected{checked.error()};
        LimitPair actual{};
        if (!succeeded(operations.get(operations.context, static_cast<NativeLimit>(i), &actual)) || actual != receipt.limits[i])
            return std::unexpected{Reason::limit_verify};
        checked = deadline.check(); if (!checked) return std::unexpected{checked.error()};
    }
    return receipt;
}
auto production_limit_operations() noexcept -> LimitOperations {
    return {nullptr,
        [](void*, int* pid, int* parent, std::uint64_t* uid) noexcept -> Raw {
            *pid = ::getpid(); *parent = ::getppid(); *uid = ::geteuid(); return {0, 0};
        },
        [](void*, NativeLimit resource, LimitPair* pair) noexcept -> Raw {
            rlimit value{}; const int result = ::getrlimit(native_resource(resource), &value);
            if (result != 0) return {result, errno};
            *pair = {value.rlim_cur == RLIM_INFINITY ? UINT64_MAX : value.rlim_cur,
                     value.rlim_max == RLIM_INFINITY ? UINT64_MAX : value.rlim_max};
            return {0, 0};
        },
        [](void*, NativeLimit resource, LimitPair pair) noexcept -> Raw {
            const rlimit value{static_cast<rlim_t>(pair.soft), static_cast<rlim_t>(pair.hard)};
            const int result = ::setrlimit(native_resource(resource), &value);
            return {result, result != 0 ? errno : 0};
        }, static_cast<std::uint64_t>(RLIM_INFINITY - 1), {nullptr, native_now, not_cancelled}};
}
} // namespace epycbench::platform_linux::host_work
