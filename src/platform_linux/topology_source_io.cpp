#include "platform_linux/topology_source_io.hpp"

#include "epycbench/platform_linux/file_descriptor.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/magic.h>
#include <new>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux::detail {
namespace {
using Root = TopologySourceRoot;
using Reason = TopologySourceReason;
using Operation = TopologySourceOperation;
using Failure = TopologySourceFailure;
using Raw = RawStatusResult;
constexpr std::size_t path_capacity = 4096, name_capacity = 256, io_capacity = 4096;
constexpr std::size_t enumeration_slot = 3, first_walk = 4, second_walk = 5;
auto success(Raw value) noexcept -> bool { return value.value == 0 && value.error_number == 0; }
auto valid(const TopologySourceLimits& l) noexcept -> bool {
    return l.acquisitions && l.io_calls >= 6U && l.guard_checks && l.conversion_work &&
        l.path_bytes && l.path_bytes < path_capacity && l.component_bytes &&
        l.component_bytes < name_capacity && l.path_depth &&
        l.file_bytes && l.file_bytes < std::numeric_limits<std::size_t>::max() &&
        l.total_text_bytes && l.refills;
}
auto valid(const TopologySourceOperations& o) noexcept -> bool {
    return o.open_root && o.open_at && o.stat_fd && o.stat_at && o.filesystem &&
        o.readlink && o.entries && o.read && o.close && o.pid && o.tid;
}
auto same(const struct stat& a, const struct stat& b) noexcept -> bool {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_uid == b.st_uid &&
        a.st_mode == b.st_mode && a.st_nlink == b.st_nlink;
}
auto native(long value) noexcept -> Raw { return {value, value < 0 ? errno : 0}; }
} // namespace

auto topology_source_limits_valid(const TopologySourceLimits& limits) noexcept -> bool { return valid(limits); }

auto production_topology_source_operations() noexcept -> TopologySourceOperations {
    return {nullptr,
        +[](void*, const char* path, int flags) noexcept { return native(::open(path, flags)); },
        +[](void*, int parent, const char* name, int flags) noexcept { return native(::openat(parent, name, flags)); },
        +[](void*, int fd, struct stat* value) noexcept { return native(::fstat(fd, value)); },
        +[](void*, int fd, const char* name, struct stat* value) noexcept {
            return native(::fstatat(fd, name, value, AT_SYMLINK_NOFOLLOW));
        },
        +[](void*, int fd, TopologySourceFilesystem* value) noexcept {
            struct statfs fs{};
            const auto result = native(::fstatfs(fd, &fs));
            if (success(result)) {
                *value = fs.f_type == SYSFS_MAGIC ? TopologySourceFilesystem::sysfs
                    : fs.f_type == PROC_SUPER_MAGIC ? TopologySourceFilesystem::proc
                    : fs.f_type == CGROUP_SUPER_MAGIC ? TopologySourceFilesystem::cgroup
                    : fs.f_type == CGROUP2_SUPER_MAGIC ? TopologySourceFilesystem::cgroup2
                    : fs.f_type == TMPFS_MAGIC ? TopologySourceFilesystem::tmpfs
                    : TopologySourceFilesystem::other;
            }
            return result;
        },
        +[](void*, int fd, char* output, std::size_t size) noexcept {
            return native(::readlinkat(fd, "", output, size));
        },
        +[](void*, int fd, std::span<char> output) noexcept {
            return native(::syscall(SYS_getdents64, fd, output.data(), output.size()));
        },
        +[](void*, int fd, std::span<char> output) noexcept { return native(::read(fd, output.data(), output.size())); },
        +[](void*, int fd) noexcept { return native(::close(fd)); },
        +[](void*) noexcept { return Raw{::getpid(), 0}; },
        +[](void*) noexcept { return native(::syscall(SYS_gettid)); }};
}

struct PreparedTopologySourceIO::State final {
    State(TopologySourceLimits input, TopologySourceOperations leaves, host_work::GuardOperations guard_ops,
          std::uint64_t actor) noexcept : allowance{input}, operations{leaves}, guard{guard_ops}, main{actor} {}
    TopologySourceLimits allowance;
    TopologySourceOperations operations;
    host_work::GuardOperations guard;
    std::uint64_t main{}, end{}, last{}, acquisitions{}, calls{}, checks{}, work_units{};
    std::uint64_t text_bytes{}, entries_seen{}, refills_seen{};
    std::optional<Failure> primary;
    std::array<TopologySourceCloseFact, 6> close_facts{};
    std::size_t close_count{};
    std::array<std::optional<FileDescriptor>, 6> descriptors;
    std::array<int, 6> registered{-1, -1, -1, -1, -1, -1};
    std::array<std::array<char, path_capacity>, 3> roots{};
    std::array<char, path_capacity> pending{}, prefix{}, combined{}, target{};
    std::array<char, name_capacity> name{};
    alignas(std::uint64_t) std::array<char, io_capacity> directory_bytes{};
    std::array<char, io_capacity> read_bytes{};
    Root root{};
    std::array<char, path_capacity> logical{};
    std::size_t used{}, cursor{};
    int final_parent{-1};
    bool running{}, close_uncertain{};
    Raw last_native{};

    auto fail(Reason reason, Operation operation, Raw raw = {}) noexcept -> std::unexpected<Failure> {
        if (!primary) {
            primary.emplace();
            primary->reason = reason; primary->operation = operation;
            primary->native_value = raw.value; primary->native_error = raw.error_number;
            primary->root = root; primary->relative = logical;
        }
        return std::unexpected{*primary};
    }
    auto check() noexcept -> bool {
        const bool cancelled_before = guard.cancelled(guard.context);
        if (cancelled_before) static_cast<void>(fail(Reason::cancelled, Operation::guard));
        if (checks == allowance.guard_checks) {
            static_cast<void>(fail(Reason::capacity, Operation::guard)); return false;
        }
        ++checks;
        std::uint64_t now{};
        const auto observed = guard.now(guard.context, &now);
        if (!success(observed) || now < last) {
            // The checked projection controls validity. Copy the genuine
            // actor-local syscall pair before a consuming close samples again.
            const auto actual = guard.actual_raw_now ? *guard.actual_raw_now : observed;
            static_cast<void>(fail(Reason::clock, Operation::guard, actual));
        }
        else {
            last = now;
            if (!end || now >= end) static_cast<void>(fail(Reason::deadline, Operation::guard));
        }
        if (guard.cancelled(guard.context)) static_cast<void>(fail(Reason::cancelled, Operation::guard));
        return !primary;
    }
    auto owned() const noexcept -> std::size_t {
        std::size_t count{};
        for (std::size_t i{}; i < descriptors.size(); ++i)
            if ((descriptors[i] && descriptors[i]->active()) || registered[i] >= 0) ++count;
        return count;
    }
    auto before(Operation operation, bool opening = false) noexcept -> bool {
        if (primary || !check()) return false;
        const auto retirement = static_cast<std::uint64_t>(owned()) + (opening ? 1U : 0U);
        // Reserve one consuming-close call for every registered owner, including
        // the acquisition about to occur. Exhaustion never suppresses cleanup.
        if (calls >= allowance.io_calls || retirement >= allowance.io_calls - calls) {
            static_cast<void>(fail(Reason::capacity, operation, last_native)); return false;
        }
        ++calls;
        return true;
    }
    auto after(Raw raw, Operation operation, bool allow_eintr = true) noexcept -> bool {
        last_native = raw;
        if ((operation == Operation::stat || operation == Operation::filesystem) && raw.value > 0)
            static_cast<void>(fail(Reason::malformed, operation, raw));
        else if (raw.value < 0 && raw.error_number == 0)
            static_cast<void>(fail(Reason::malformed, operation, raw));
        else if (raw.value < 0 && !(allow_eintr && raw.error_number == EINTR))
            static_cast<void>(fail(Reason::native, operation, raw));
        else if (raw.value >= 0 && raw.error_number)
            static_cast<void>(fail(Reason::malformed, operation, raw));
        static_cast<void>(check());
        return !primary;
    }
    auto descriptor(std::size_t slot) const noexcept -> int {
        return descriptors[slot] ? descriptors[slot]->native_handle() : registered[slot];
    }
    auto register_fd(std::size_t slot, Raw raw, bool defer_adoption = false) noexcept -> bool {
        if (raw.value < 0) return false;
        if (raw.value > INT_MAX || descriptor(slot) >= 0) {
            close_uncertain = true; static_cast<void>(fail(Reason::uncertain, Operation::open, raw)); return false;
        }
        for (std::size_t i{}; i < descriptors.size(); ++i)
            if (descriptor(i) == raw.value) {
                close_uncertain = true; static_cast<void>(fail(Reason::uncertain, Operation::open, raw)); return false;
            }
        registered[slot] = static_cast<int>(raw.value); // first registration, no allocation
        if (defer_adoption) return true;
        const DescriptorOperations close_operations{this,
            +[](void* opaque, int fd) noexcept {
                auto& state = *static_cast<State*>(opaque);
                return state.operations.close(state.operations.context, fd);
            }, nullptr};
        // Positive validated adoption constructs/moves only the existing owner.
        auto wrapped = DescriptorAccess::adopt(registered[slot], close_operations);
        descriptors[slot].emplace(std::move(*wrapped));
        registered[slot] = -1;
        return true;
    }
    auto close_slot(std::size_t slot) noexcept -> bool {
        const auto fd = descriptor(slot);
        if (fd < 0) { descriptors[slot].reset(); return true; }
        static_cast<void>(check());
        ++calls; // the call reserved before acquisition, even after terminal guard failure
        const auto raw = descriptors[slot]
            ? DescriptorAccess::close_raw(*descriptors[slot])
            : operations.close(operations.context, std::exchange(registered[slot], -1));
        descriptors[slot].reset();
        last_native = raw;
        if (!success(raw)) {
            close_uncertain = true;
            if (!primary) static_cast<void>(fail(Reason::uncertain, Operation::close, raw));
            if (close_count < close_facts.size())
                close_facts[close_count++] = {static_cast<unsigned char>(slot), fd, raw.value, raw.error_number};
        }
        static_cast<void>(check());
        return success(raw);
    }
    void close_walk() noexcept { static_cast<void>(close_slot(second_walk)); static_cast<void>(close_slot(first_walk)); }
    auto transfer(std::size_t from, std::size_t to) noexcept -> bool {
        if (close_uncertain || descriptor(to) >= 0 || !descriptors[from]) {
            static_cast<void>(fail(Reason::invalid_state, Operation::state)); return false;
        }
        descriptors[to].emplace(std::move(*descriptors[from]));
        descriptors[from].reset();
        return true;
    }
    auto consume_work(std::uint64_t amount) noexcept -> bool {
        if (primary || amount > allowance.conversion_work - work_units) {
            static_cast<void>(fail(Reason::capacity, Operation::conversion)); return false;
        }
        work_units += amount;
        return check();
    }
    auto select(Root selected, std::string_view relative) noexcept -> bool {
        if (static_cast<unsigned>(selected) >= roots.size()) {
            static_cast<void>(fail(Reason::invalid_input, Operation::path)); return false;
        }
        root = selected;
        if (relative.empty() || relative.front() == '/' || relative.size() > allowance.path_bytes ||
            relative.find('\0') != relative.npos) {
            static_cast<void>(fail(Reason::path, Operation::path)); return false;
        }
        logical.fill(0); std::copy(relative.begin(), relative.end(), logical.begin());
        return consume_work(relative.size());
    }
    auto normalize(std::string_view input, std::array<char, path_capacity>& output) noexcept -> bool {
        if (input.size() > allowance.path_bytes || !consume_work(input.size())) return false;
        output.fill(0);
        std::size_t length{}, depth{};
        for (std::size_t start{}; start < input.size();) {
            const auto slash = input.find('/', start);
            const auto piece = input.substr(start, slash == input.npos ? input.size() - start : slash - start);
            if (piece == "..") {
                if (!depth) { static_cast<void>(fail(Reason::path, Operation::path)); return false; }
                while (length && output[length - 1] != '/') output[--length] = 0;
                if (length) output[--length] = 0;
                --depth;
            } else if (!piece.empty() && piece != ".") {
                if (piece.size() > allowance.component_bytes || ++depth > allowance.path_depth) {
                    static_cast<void>(fail(Reason::capacity, Operation::path)); return false;
                }
                if (length) output[length++] = '/';
                std::copy(piece.begin(), piece.end(), output.begin() + static_cast<std::ptrdiff_t>(length));
                length += piece.size();
            }
            if (slash == input.npos) break;
            start = slash + 1;
        }
        return true;
    }
    template<class Call> auto status(Operation operation, Call&& call) noexcept -> Raw {
        for (;;) {
            if (!before(operation)) return {-1, 0};
            const auto raw = call();
            if (!after(raw, operation) || raw.error_number != EINTR) return raw;
        }
    }
    auto authenticate(std::size_t slot, Root selected, bool directory, bool anchor = false) noexcept -> bool {
        struct stat metadata{};
        if (!success(status(Operation::stat, [&] { return operations.stat_fd(operations.context, descriptor(slot), &metadata); }))) return false;
        TopologySourceFilesystem filesystem{};
        if (!success(status(Operation::filesystem, [&] { return operations.filesystem(operations.context, descriptor(slot), &filesystem); }))) return false;
        const bool type = directory ? S_ISDIR(metadata.st_mode) : S_ISREG(metadata.st_mode);
        const bool source = selected == Root::sys ? filesystem == TopologySourceFilesystem::sysfs
            : selected == Root::proc ? filesystem == TopologySourceFilesystem::proc
            : filesystem == TopologySourceFilesystem::cgroup || filesystem == TopologySourceFilesystem::cgroup2 ||
                (anchor && filesystem == TopologySourceFilesystem::tmpfs);
        if (!type || !metadata.st_nlink || !source ||
            (selected == Root::sys && metadata.st_uid != 0)) {
            static_cast<void>(fail(Reason::identity, Operation::filesystem)); return false;
        }
        return check();
    }
    auto open(std::size_t slot, int parent, const char* component, int flags, bool required,
              bool defer_adoption = false) noexcept -> bool {
        for (;;) {
            if (!before(Operation::open, true)) return false;
            const auto raw = parent < 0 ? operations.open_root(operations.context, component, flags)
                                        : operations.open_at(operations.context, parent, component, flags);
            last_native = raw;
            if (raw.value >= 0) static_cast<void>(register_fd(slot, raw, defer_adoption));
            if (raw.value < 0 && raw.error_number == ENOENT && !required) {
                static_cast<void>(check()); return false;
            }
            if (!after(raw, Operation::open)) return false;
            if (raw.error_number == EINTR) continue;
            return descriptor(slot) >= 0;
        }
    }
    auto bootstrap(std::size_t anchor) noexcept -> bool {
        root = static_cast<Root>(anchor); logical.fill(0);
        const auto path = std::string_view{roots[anchor].data()};
        if (!normalize(path.substr(1), pending)) return false;
        if (std::string_view{pending.data()} != path.substr(1)) {
            static_cast<void>(fail(Reason::path, Operation::path)); return false;
        }
        if (!open(first_walk, -1, "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, true)) return false;
        for (std::size_t start{}; start < std::strlen(pending.data());) {
            const auto all = std::string_view{pending.data()};
            const auto slash = all.find('/', start);
            const auto part = all.substr(start, slash == all.npos ? all.size() - start : slash - start);
            name.fill(0); std::copy(part.begin(), part.end(), name.begin());
            if (!open(second_walk, descriptor(first_walk), name.data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, true)) return false;
            struct stat metadata{};
            if (!success(status(Operation::stat, [&] { return operations.stat_fd(operations.context, descriptor(second_walk), &metadata); })) ||
                !S_ISDIR(metadata.st_mode)) { static_cast<void>(fail(Reason::identity, Operation::stat)); return false; }
            if (!close_slot(first_walk) || !transfer(second_walk, first_walk)) return false;
            if (slash == all.npos) break;
            start = slash + 1;
        }
        if (!authenticate(first_walk, root, true, true) || !transfer(first_walk, anchor)) return false;
        return true;
    }
    auto resolve(Root selected, std::string_view relative, bool directory, bool required,
                 bool forbid_final_link = false, bool defer_final_adoption = false,
                 int held_restart_anchor = -1, std::string_view absolute_anchor = {}) noexcept -> bool {
        if (!running) { static_cast<void>(fail(Reason::invalid_state, Operation::state)); return false; }
        if (primary || close_uncertain || !select(selected, relative)) return false;
        if (!normalize(relative, pending)) return false;
        prefix.fill(0);
        std::uint64_t links{};
        for (std::size_t start{};;) {
            const auto all = std::string_view{pending.data()};
            const auto slash = all.find('/', start);
            const auto part = all.substr(start, slash == all.npos ? all.size() - start : slash - start);
            const bool final = slash == all.npos;
            if (part.empty() || !consume_work(1)) { static_cast<void>(fail(Reason::path, Operation::path)); return false; }
            name.fill(0); std::copy(part.begin(), part.end(), name.begin());
            const int parent = descriptor(first_walk) >= 0 ? descriptor(first_walk)
                : held_restart_anchor >= 0 ? held_restart_anchor : descriptor(static_cast<std::size_t>(selected));
            struct stat named{};
            for (;;) {
                if (!before(Operation::stat)) return false;
                const auto raw = operations.stat_at(operations.context, parent, name.data(), &named);
                last_native = raw;
                if (raw.value < 0 && raw.error_number == ENOENT && !required) { static_cast<void>(check()); return false; }
                if (!after(raw, Operation::stat)) return false;
                if (raw.error_number != EINTR) break;
            }
            if (S_ISLNK(named.st_mode)) {
                if ((final && forbid_final_link) || ++links > allowance.link_expansions) {
                    static_cast<void>(fail(final && forbid_final_link ? Reason::path : Reason::capacity, Operation::path)); return false;
                }
                if (!open(second_walk, parent, name.data(), O_PATH | O_CLOEXEC | O_NOFOLLOW, required)) return false;
                struct stat held{};
                if (!success(status(Operation::stat, [&] { return operations.stat_fd(operations.context, descriptor(second_walk), &held); })) || !same(held, named)) {
                    static_cast<void>(fail(Reason::identity, Operation::stat)); return false;
                }
                Raw link{};
                do {
                    if (!before(Operation::readlink)) return false;
                    link = operations.readlink(operations.context, descriptor(second_walk), target.data(),
                                               static_cast<std::size_t>(allowance.path_bytes) + 1U);
                    if (!after(link, Operation::readlink)) return false;
                } while (link.error_number == EINTR);
                if (link.value <= 0 || static_cast<std::uint64_t>(link.value) > allowance.path_bytes) {
                    static_cast<void>(fail(Reason::capacity, Operation::readlink, link)); return false;
                }
                struct stat still_named{};
                if (!success(status(Operation::stat, [&] { return operations.stat_at(operations.context, parent, name.data(), &still_named); })) || !same(held, still_named)) {
                    static_cast<void>(fail(Reason::identity, Operation::stat)); return false;
                }
                const auto target_text = std::string_view{target.data(), static_cast<std::size_t>(link.value)};
                if (target_text.find('\0') != target_text.npos) {
                    static_cast<void>(fail(Reason::path, Operation::readlink)); return false;
                }
                auto within = target_text;
                if (within.front() == '/') {
                    const auto anchor = absolute_anchor.empty()
                        ? std::string_view{roots[static_cast<std::size_t>(selected)].data()} : absolute_anchor;
                    if (!within.starts_with(anchor) || (anchor != "/" && within.size() > anchor.size() && within[anchor.size()] != '/')) {
                        static_cast<void>(fail(Reason::path, Operation::readlink)); return false;
                    }
                    within.remove_prefix(anchor.size());
                    if (!within.empty() && anchor != "/") within.remove_prefix(1);
                }
                if (selected == Root::proc && prefix[0] == 0 && part == "self") {
                    std::array<char, 32> process{};
                    const auto written = std::to_chars(process.data(), process.data() + process.size(), main);
                    const auto expected = std::string_view{process.data(), static_cast<std::size_t>(written.ptr - process.data())};
                    if (written.ec != std::errc{} || !normalize(within, combined) || std::string_view{combined.data()} != expected) {
                        static_cast<void>(fail(Reason::identity, Operation::readlink)); return false;
                    }
                }
                const auto parent_text = target_text.front() == '/' ? std::string_view{} : std::string_view{prefix.data()};
                const auto remainder = final ? std::string_view{} : all.substr(slash + 1);
                const auto pieces = static_cast<unsigned>(!parent_text.empty()) + static_cast<unsigned>(!within.empty()) + static_cast<unsigned>(!remainder.empty());
                const auto total = parent_text.size() + within.size() + remainder.size() + (pieces ? pieces - 1U : 0U);
                if (total > allowance.path_bytes) { static_cast<void>(fail(Reason::capacity, Operation::path)); return false; }
                combined.fill(0); std::size_t length{};
                for (const auto piece : {parent_text, within, remainder}) if (!piece.empty()) {
                    if (length) combined[length++] = '/';
                    std::copy(piece.begin(), piece.end(), combined.begin() + static_cast<std::ptrdiff_t>(length)); length += piece.size();
                }
                // A held symlink is consumed before the target walk is restarted.
                if (!close_slot(second_walk) || !close_slot(first_walk)) return false;
                if (!normalize({combined.data(), length}, pending)) return false;
                prefix.fill(0); start = 0; continue;
            }
            const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | ((!final || directory) ? O_DIRECTORY : 0);
            if (!open(second_walk, parent, name.data(), flags, required, final && defer_final_adoption)) return false;
            struct stat held{};
            if (!success(status(Operation::stat, [&] { return operations.stat_fd(operations.context, descriptor(second_walk), &held); })) || !same(held, named)) {
                static_cast<void>(fail(Reason::identity, Operation::stat)); return false;
            }
            if (final) {
                final_parent = parent;
                return authenticate(second_walk, selected, directory);
            }
            if (!S_ISDIR(held.st_mode)) { static_cast<void>(fail(Reason::identity, Operation::stat)); return false; }
            if (!close_slot(first_walk) || !transfer(second_walk, first_walk)) return false;
            auto length = std::strlen(prefix.data());
            if (length) prefix[length++] = '/';
            std::copy(part.begin(), part.end(), prefix.begin() + static_cast<std::ptrdiff_t>(length));
            start = slash + 1;
        }
    }
    auto finish() noexcept -> bool {
        close_walk();
        for (std::size_t slot = enumeration_slot + 1; slot != 0; --slot) static_cast<void>(close_slot(slot - 1));
        running = false;
        return !primary && !close_uncertain;
    }
};

PreparedTopologySourceIO::PreparedTopologySourceIO(std::unique_ptr<State> state, host_work::ResourceLease lease) noexcept
    : state_{std::move(state)}, lease_{std::move(lease)} {}
PreparedTopologySourceIO::PreparedTopologySourceIO(PreparedTopologySourceIO&&) noexcept = default;
PreparedTopologySourceIO::~PreparedTopologySourceIO() noexcept {
    const bool uncertain = state_ && (static_cast<void>(state_->finish()), state_->close_uncertain);
    state_.reset(); // scratch and every actual owner end before the enclosing lease
    if (lease_.active() && !uncertain && !lease_.retire()) std::terminate();
    // Uncertain consuming close leaves the reservation active; its destructor
    // poisons the enclosing ledger. No retry or later activation is authorized.
}
auto PreparedTopologySourceIO::prepare(host_work::ResourceLedger& ledger, TopologySourceLimits limits,
    TopologySourceOperations operations, host_work::GuardOperations guard,
    std::array<std::string_view, 3> roots, std::uint64_t main) noexcept
    -> std::expected<PreparedTopologySourceIO, Failure> {
    if (!valid(limits) || !valid(operations) || !guard.now || !guard.cancelled || !main)
        return std::unexpected{Failure{Reason::invalid_input, Operation::state}};
    for (auto root : roots)
        if (root.size() < 2U || root.front() != '/' || root.size() > limits.path_bytes ||
            root.back() == '/' || root.find('\0') != root.npos)
            return std::unexpected{Failure{Reason::invalid_input, Operation::path}};
    host_work::Resources charge{}; charge.descriptors = 6U;
    charge.ordinary_bytes = core::ByteCount::from_base_units(sizeof(State));
    charge.address_space_bytes = charge.ordinary_bytes;
    auto lease = ledger.reserve(charge);
    if (!lease) return std::unexpected{Failure{Reason::capacity, Operation::reserve}};
    try {
        auto state = std::make_unique<State>(limits, operations, guard, main);
        for (std::size_t i{}; i < roots.size(); ++i) std::copy(roots[i].begin(), roots[i].end(), state->roots[i].begin());
        return PreparedTopologySourceIO{std::move(state), std::move(*lease)};
    } catch (const std::bad_alloc&) {
        if (!lease->retire()) std::terminate();
        return std::unexpected{Failure{Reason::allocation, Operation::allocation}};
    }
}
auto PreparedTopologySourceIO::invocation_ready() const noexcept -> std::expected<void, Failure> {
    if (!state_ || !lease_.active())
        return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    const auto& s = *state_;
    if (s.primary) return std::unexpected{*s.primary};
    if (s.running || s.close_uncertain || s.owned())
        return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    if (s.acquisitions >= s.allowance.acquisitions)
        return std::unexpected{Failure{Reason::capacity, Operation::state}};
    return {};
}
auto PreparedTopologySourceIO::begin(std::uint64_t end) noexcept -> std::expected<void, Failure> {
    if (!state_ || !end) return std::unexpected{Failure{Reason::invalid_input, Operation::state}};
    auto& s = *state_;
    const auto ready = invocation_ready();
    if (!ready) return s.fail(ready.error().reason, ready.error().operation);
    ++s.acquisitions; s.end = end; s.running = true;
    if (!s.check()) return std::unexpected{*s.primary};
    const auto pid = s.status(Operation::identity, [&] { return s.operations.pid(s.operations.context); });
    const auto tid = s.status(Operation::identity, [&] { return s.operations.tid(s.operations.context); });
    if (pid.value <= 0 || pid.error_number || tid.value != pid.value || static_cast<std::uint64_t>(pid.value) != s.main)
        return s.fail(Reason::identity, Operation::identity);
    for (std::size_t i{}; i < s.roots.size(); ++i) if (!s.bootstrap(i)) return std::unexpected{*s.primary};
    return {};
}
auto PreparedTopologySourceIO::finish() noexcept -> bool { return state_ && state_->finish(); }
auto PreparedTopologySourceIO::read(Root root, std::string_view relative, bool required)
    -> std::expected<std::optional<std::string>, Failure> {
    if (!state_) return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    auto& s = *state_;
    struct WalkCleanup { State& state; ~WalkCleanup() noexcept { state.close_walk(); } } cleanup{s};
    if (!s.resolve(root, relative, false, required)) {
        const bool missing = !s.primary && !required && s.last_native.error_number == ENOENT;
        s.close_walk();
        if (missing && !s.primary) return std::optional<std::string>{};
        if (!s.primary) static_cast<void>(s.fail(Reason::missing, Operation::open));
        return std::unexpected{*s.primary};
    }
    std::string result;
    struct stat first{}, last{}, named{};
    const auto fd = s.descriptor(second_walk);
    if (!success(s.status(Operation::stat, [&] { return s.operations.stat_fd(s.operations.context, fd, &first); }))) return std::unexpected{*s.primary};
    try {
        for (;;) {
            if (!s.before(Operation::read)) return std::unexpected{*s.primary};
            const auto remaining = s.allowance.file_bytes - result.size();
            const auto offered = static_cast<std::size_t>(std::min<std::uint64_t>(s.read_bytes.size(), remaining + 1U));
            const auto raw = s.operations.read(s.operations.context, fd, {s.read_bytes.data(), offered});
            if (!s.after(raw, Operation::read)) return std::unexpected{*s.primary};
            if (raw.error_number == EINTR) continue;
            if (static_cast<std::uint64_t>(raw.value) > offered) return s.fail(Reason::malformed, Operation::read, raw);
            if (raw.value == 0) break;
            const auto count = static_cast<std::uint64_t>(raw.value);
            if (count > remaining || count > s.allowance.total_text_bytes - s.text_bytes)
                return s.fail(Reason::capacity, Operation::read, raw);
            s.text_bytes += count;
            result.append(s.read_bytes.data(), static_cast<std::size_t>(count));
        }
    } catch (...) { static_cast<void>(s.fail(Reason::allocation, Operation::allocation)); throw; }
    if (!success(s.status(Operation::stat, [&] { return s.operations.stat_fd(s.operations.context, fd, &last); })) ||
        !success(s.status(Operation::stat, [&] { return s.operations.stat_at(s.operations.context, s.final_parent, s.name.data(), &named); })) ||
        !same(first, last) || !same(last, named)) return s.fail(Reason::identity, Operation::stat);
    if (!s.close_slot(second_walk) || !s.close_slot(first_walk) || s.primary) return std::unexpected{*s.primary};
    return std::optional<std::string>{std::move(result)};
}
auto PreparedTopologySourceIO::entries(Root root, std::string_view relative)
    -> std::expected<std::vector<std::string>, Failure> {
    if (!state_) return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    auto& s = *state_;
    struct ScanCleanup { State& state; ~ScanCleanup() noexcept {
        state.close_walk(); static_cast<void>(state.close_slot(enumeration_slot));
    }} cleanup{s};
    if (!s.resolve(root, relative, true, true) || !s.close_slot(first_walk) || !s.transfer(second_walk, enumeration_slot))
        return std::unexpected{*s.primary};
    struct stat initial{};
    if (!success(s.status(Operation::stat, [&] { return s.operations.stat_fd(s.operations.context, s.descriptor(enumeration_slot), &initial); }))) return std::unexpected{*s.primary};
    s.used = 0; s.cursor = 0; std::uint64_t entries{};
    std::vector<std::string> result;
    try {
        for (;;) {
            if (s.cursor == s.used) {
                if (s.refills_seen == s.allowance.refills) return s.fail(Reason::capacity, Operation::enumerate);
                if (!s.before(Operation::enumerate)) return std::unexpected{*s.primary};
                ++s.refills_seen;
                const auto raw = s.operations.entries(s.operations.context, s.descriptor(enumeration_slot), s.directory_bytes);
                if (!s.after(raw, Operation::enumerate)) return std::unexpected{*s.primary};
                if (raw.error_number == EINTR) continue;
                if (static_cast<std::uint64_t>(raw.value) > s.directory_bytes.size()) return s.fail(Reason::malformed, Operation::enumerate, raw);
                if (raw.value == 0) break;
                s.used = static_cast<std::size_t>(raw.value); s.cursor = 0;
            }
            if (!s.consume_work(1)) return std::unexpected{*s.primary};
            if (entries == s.allowance.entries_per_directory || s.entries_seen == s.allowance.total_entries)
                return s.fail(Reason::capacity, Operation::enumerate);
            ++entries; ++s.entries_seen;
            constexpr std::size_t name_offset = 19U, record_offset = 16U;
            if (s.used - s.cursor < name_offset + 1U) return s.fail(Reason::malformed, Operation::enumerate);
            unsigned short record{};
            std::memcpy(&record, s.directory_bytes.data() + s.cursor + record_offset, sizeof(record));
            if (record < name_offset + 1U || record > s.used - s.cursor) return s.fail(Reason::malformed, Operation::enumerate);
            const auto* text = s.directory_bytes.data() + s.cursor + name_offset;
            const auto length = ::strnlen(text, record - name_offset);
            if (length == record - name_offset || length > s.allowance.component_bytes)
                return s.fail(Reason::capacity, Operation::enumerate);
            s.cursor += record;
            const auto name = std::string_view{text, length};
            if (name == "." || name == "..") continue;
            if (name.empty() || name.find('/') != name.npos) return s.fail(Reason::malformed, Operation::enumerate);
            result.emplace_back(name);
        }
    } catch (...) { static_cast<void>(s.fail(Reason::allocation, Operation::allocation)); throw; }
    struct stat observed_after{};
    if (!success(s.status(Operation::stat, [&] { return s.operations.stat_fd(s.operations.context, s.descriptor(enumeration_slot), &observed_after); })) || !same(initial, observed_after)) return s.fail(Reason::identity, Operation::stat);
    // Rewalk the named directory while the enumeration owner remains held; the
    // two walking roles still fit the six-role stock and cannot duplicate it.
    if (!s.resolve(root, relative, true, true)) return std::unexpected{*s.primary};
    struct stat named{};
    if (!success(s.status(Operation::stat, [&] { return s.operations.stat_fd(s.operations.context, s.descriptor(second_walk), &named); })) || !same(observed_after, named)) return s.fail(Reason::identity, Operation::stat);
    s.close_walk();
    if (!s.close_slot(enumeration_slot) || s.primary) return std::unexpected{*s.primary};
    return result;
}
auto PreparedTopologySourceIO::directory(Root root, std::string_view relative, bool forbid_link) noexcept
    -> std::expected<void, Failure> {
    if (!state_) return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    auto& s = *state_;
    const bool found = s.resolve(root, relative, true, true, forbid_link);
    s.close_walk();
    if (!found || s.primary) return std::unexpected{*s.primary};
    return {};
}
auto PreparedTopologySourceIO::native_open(TopologySourceNativePath selected,
    std::string_view absolute, bool directory, bool required, int& destination,
    std::span<const int> native_owned) noexcept -> std::expected<bool, Failure> {
    if (!state_) return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    auto& s = *state_;
    if (destination != -1 || native_owned.size() > 1024U || s.descriptor(enumeration_slot) >= 0 ||
        s.descriptor(first_walk) >= 0 || s.descriptor(second_walk) >= 0)
        return s.fail(Reason::invalid_state, Operation::state);
    if (absolute.empty() || absolute.front() != '/' || absolute.find('\0') != absolute.npos ||
        absolute.size() > s.allowance.path_bytes)
        return s.fail(Reason::path, Operation::path);
    // Runtime data is not sysfs/proc/cgroup data. The exact qualified artifact
    // and runtime filesystem/backing admission must be connected before this
    // route becomes available. A caller string alone grants no runtime reads.
    if (selected == TopologySourceNativePath::runtime_hwdata)
        return s.fail(Reason::invalid_input, Operation::path);
    Root root{}; std::string_view relative{}, anchor{};
    int restart{-1};
    if (selected == TopologySourceNativePath::sys && absolute.starts_with("/sys/")) {
        root = Root::sys; relative = absolute.substr(5);
    } else if (selected == TopologySourceNativePath::proc && absolute.starts_with("/proc/")) {
        root = Root::proc; relative = absolute.substr(6);
    } else if (selected == TopologySourceNativePath::cgroup_mount && absolute.size() > 1U) {
        root = Root::cgroup; relative = absolute.substr(1); anchor = "/";
        // Enumeration's existing role owns the genuine slash restart anchor.
        // Three held roots + this anchor + two walkers still occupy six roles.
        if (!s.open(enumeration_slot, -1, "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, true)) {
            static_cast<void>(s.close_slot(enumeration_slot));
            return std::unexpected{*s.primary};
        }
        struct stat metadata{};
        if (!success(s.status(Operation::stat, [&] {
                return s.operations.stat_fd(s.operations.context, s.descriptor(enumeration_slot), &metadata);
            })) || !S_ISDIR(metadata.st_mode) || !metadata.st_nlink) {
            if (!s.primary) static_cast<void>(s.fail(Reason::identity, Operation::stat));
            static_cast<void>(s.close_slot(enumeration_slot));
            return std::unexpected{*s.primary};
        }
        restart = s.descriptor(enumeration_slot);
    } else {
        return s.fail(Reason::path, Operation::path);
    }
    bool found = s.resolve(root, relative, directory, required, false, true, restart, anchor);
    const bool missing = !found && !s.primary && !required && s.last_native.error_number == ENOENT;
    if (found) {
        const auto fd = s.registered[second_walk];
        if (fd < 0 || s.descriptors[second_walk]) {
            static_cast<void>(s.fail(Reason::invalid_state, Operation::state)); found = false;
        } else {
            bool alias{};
            for (const auto owned : native_owned) {
                if (!s.consume_work(1)) { found = false; break; }
                alias = alias || owned == fd;
            }
            if (alias) {
                // A malformed returned alias belongs to the existing native
                // owner. Do not consume that same descriptor a second time.
                s.registered[second_walk] = -1;
                s.close_uncertain = true;
                static_cast<void>(s.fail(Reason::malformed, Operation::open, {fd, 0}));
                found = false;
            } else if (found) {
                destination = fd;                 // native registration first
                s.registered[second_walk] = -1;    // nonthrowing lower release
            }
        }
    }
    s.close_walk();
    if (restart >= 0) static_cast<void>(s.close_slot(enumeration_slot));
    static_cast<void>(s.check());
    if (s.primary) return std::unexpected{*s.primary};
    if (missing) return false;
    if (!found) return s.fail(Reason::missing, Operation::open);
    return true;
}
auto PreparedTopologySourceIO::count(TopologySourceCount axis, std::uint64_t value) noexcept
    -> std::expected<void, Failure> {
    if (!state_) return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    auto& s = *state_; std::uint64_t maximum{};
    switch (axis) {
    case TopologySourceCount::online_cpus: maximum = s.allowance.online_cpus; break;
    case TopologySourceCount::enumerated_cpus: maximum = s.allowance.enumerated_cpus; break;
    case TopologySourceCount::cache_entries_per_cpu: maximum = s.allowance.cache_entries_per_cpu; break;
    case TopologySourceCount::cache_entries: maximum = s.allowance.cache_entries; break;
    case TopologySourceCount::online_nodes: maximum = s.allowance.online_nodes; break;
    case TopologySourceCount::pci_entries: maximum = s.allowance.pci_entries; break;
    case TopologySourceCount::memberships: maximum = s.allowance.memberships; break;
    case TopologySourceCount::bitmap_groups: maximum = s.allowance.bitmap_groups; break;
    case TopologySourceCount::cpu_index: maximum = s.allowance.maximum_cpu_index; break;
    case TopologySourceCount::node_index: maximum = s.allowance.maximum_node_index; break;
    case TopologySourceCount::cache_index: maximum = s.allowance.maximum_cache_index; break;
    case TopologySourceCount::cluster_work: maximum = s.allowance.cluster_work; break;
    case TopologySourceCount::distance_values: maximum = s.allowance.distance_values; break;
    }
    if (value > maximum) return s.fail(Reason::capacity, Operation::conversion);
    if (!s.check()) return std::unexpected{*s.primary};
    return {};
}
auto PreparedTopologySourceIO::cluster_work_for(std::uint64_t cpus, std::uint64_t members) noexcept
    -> std::expected<void, Failure> {
    if (!state_) return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    if ((cpus && cpus > UINT64_MAX / cpus) || (members && cpus * cpus > UINT64_MAX / members))
        return state_->fail(Reason::arithmetic, Operation::conversion);
    return count(TopologySourceCount::cluster_work, cpus * cpus * members);
}
auto PreparedTopologySourceIO::distance_values_for(std::uint64_t nodes) noexcept
    -> std::expected<void, Failure> {
    if (!state_) return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    if (nodes && nodes > UINT64_MAX / nodes) return state_->fail(Reason::arithmetic, Operation::conversion);
    return count(TopologySourceCount::distance_values, nodes * nodes);
}
auto PreparedTopologySourceIO::reject(Reason reason) noexcept -> std::expected<void, Failure> {
    if (!state_) return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    return state_->fail(reason, Operation::conversion);
}
auto PreparedTopologySourceIO::work(std::uint64_t amount) noexcept -> std::expected<void, Failure> {
    if (!state_) return std::unexpected{Failure{Reason::invalid_state, Operation::state}};
    if (!state_->consume_work(amount)) return std::unexpected{*state_->primary};
    return {};
}
auto PreparedTopologySourceIO::limits() const noexcept -> const TopologySourceLimits& {
    static constexpr TopologySourceLimits empty{};
    return state_ ? state_->allowance : empty;
}
auto PreparedTopologySourceIO::failure() const noexcept -> const Failure* { return state_ && state_->primary ? &*state_->primary : nullptr; }
auto PreparedTopologySourceIO::cleanup() const noexcept -> std::span<const TopologySourceCloseFact> {
    return state_ ? std::span<const TopologySourceCloseFact>{state_->close_facts.data(), state_->close_count}
                  : std::span<const TopologySourceCloseFact>{};
}
auto PreparedTopologySourceIO::uncertain() const noexcept -> bool { return state_ && state_->close_uncertain; }
auto PreparedTopologySourceIO::active() const noexcept -> bool { return state_ && state_->running; }

} // namespace epycbench::platform_linux::detail
