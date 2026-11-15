#include "monitoring/host_q1_git_bounded_child_test_access.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <utility>
namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr std::size_t cap = 1U << 20U;
constexpr std::size_t maximum_stdout_cap = 2U << 20U;
enum class Kind : unsigned char {
    none,
    poll,
    read,
    limit,
    setup,
    exit,
    signal,
    drain,
    reap,
    close
};
struct Record {
    Kind kind{};
    int error{};
};
struct SetupRecord final {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t disposition;
    std::int32_t error;
};
static_assert(sizeof(SetupRecord) == 12);
static_assert(std::is_trivially_copyable_v<SetupRecord> && std::is_standard_layout_v<SetupRecord>);
constexpr std::uint32_t setup_magic = 0x51473147U;
constexpr short accepted_revents = POLLIN | POLLHUP | POLLERR | POLLNVAL;
auto index(Kind kind) noexcept -> std::size_t { return static_cast<std::size_t>(kind) - 1U; }
auto valid_events(const MonitoringQ1GitPollEvent* events, std::size_t count,
                  const std::array<int, 3>& descriptors, const std::array<bool, 3>& open) noexcept
    -> bool {
    std::array<bool, 3> seen{};
    for (std::size_t j = 0; j < count; ++j) {
        std::size_t i = 0;
        while (i < descriptors.size() && descriptors[i] != events[j].descriptor)
            ++i;
        if (i == descriptors.size() || !open[i] || seen[i] ||
            (events[j].revents & static_cast<short>(~accepted_revents)) != 0)
            return false;
        seen[i] = true;
    }
    return true;
}
auto make(Kind k, int e = 0) -> core::Error {
    std::string_view c = "git_result";
    if (k == Kind::poll)
        c = "git_poll";
    if (k == Kind::read)
        c = "git_read";
    if (k == Kind::limit)
        c = "git_output_limit";
    if (k == Kind::setup)
        c = "git_setup";
    if (k == Kind::exit)
        c = "git_exit";
    if (k == Kind::signal || k == Kind::drain || k == Kind::reap || k == Kind::close)
        c = "cleanup_failure";
    std::optional<core::OsError> os;
    if (e)
        os = core::OsError::capture(std::error_code{e, std::generic_category()});
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{k >= Kind::signal
                                              ? "monitoring.q1_plan_authority.cleanup_failed"
                                              : "monitoring.q1_plan_authority.repository_rejected"},
            .message = "Q1 Git child failed",
            .operation = "authenticate_monitoring_q1_plan",
            .os_error = std::move(os),
            .context = {{"classification", c}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
class ActiveOwner final {
  public:
    ActiveOwner(std::array<int, 4> raw, MonitoringQ1GitBoundedChildOperations operations) noexcept
        : raw_{raw}, operations_{operations} {}
    ActiveOwner(const ActiveOwner&) = delete;
    ActiveOwner(ActiveOwner&& other) noexcept
        : raw_{other.raw_}, operations_{other.operations_}, active_{other.active_} {
        other.active_ = false;
    }
    auto operator=(const ActiveOwner&) -> ActiveOwner& = delete;
    auto operator=(ActiveOwner&&) -> ActiveOwner& = delete;
    ~ActiveOwner() noexcept {
        if (!active_)
            return;
        static_cast<void>(operations_.kill(operations_.context, raw_[0], SIGKILL));
        std::array<bool, 3> open{true, true, true};
        std::array<char, 4096> discard{};
        while (open[0] || open[1] || open[2]) {
            std::array<MonitoringQ1GitPollEvent, 3> events{};
            std::size_t count = 0;
            for (std::size_t i = 0; i < 3; ++i)
                if (open[i])
                    events[count++] = {raw_[i + 1U], 0};
            if (operations_.poll(operations_.context, events.data(), count) <= 0)
                break;
            if (!valid_events(events.data(), count, {raw_[1], raw_[2], raw_[3]}, open))
                break;
            bool invalid = false;
            for (std::size_t j = 0; j < count; ++j)
                invalid = invalid || (events[j].revents & POLLNVAL) != 0;
            if (invalid)
                break;
            for (std::size_t j = 0; j < count; ++j) {
                if ((events[j].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0)
                    continue;
                std::size_t i = 0;
                while (raw_[i + 1U] != events[j].descriptor)
                    ++i;
                if (operations_.read(operations_.context, events[j].descriptor, discard.data(),
                                     discard.size()) <= 0)
                    open[i] = false;
            }
        }
        int status{};
        for (;;) {
            const int waited = operations_.wait(operations_.context, raw_[0], &status);
            if (waited == raw_[0])
                break;
            if (waited < 0 && errno == EINTR)
                continue;
            operations_.await_outer_termination(operations_.context);
            std::terminate();
        }
        for (auto it = raw_.rbegin(); it != raw_.rend() - 1; ++it)
            static_cast<void>(operations_.close(operations_.context, *it));
        active_ = false;
    }
    void disarm() noexcept { active_ = false; }

  private:
    std::array<int, 4> raw_;
    MonitoringQ1GitBoundedChildOperations operations_;
    bool active_{true};
};
} // namespace
struct MonitoringQ1GitBoundedChild::Prepared::State final {
    MonitoringQ1GitBoundedOutput out;
    std::array<std::array<core::Error, 9>, 12> stocks;
    std::vector<core::Error> projected;
    std::array<Record, 11> cleanup{};
    std::size_t stdout_limit;
    explicit State(const std::size_t limit) : stdout_limit{limit} {
        out.standard_output.reserve(limit);
        out.standard_error.reserve(cap);
        for (auto& slot : stocks)
            for (std::size_t i = 0; i < slot.size(); ++i)
                slot[i] = make(static_cast<Kind>(i + 1U));
        projected.reserve(11);
    }
};
MonitoringQ1GitBoundedChild::Prepared::Prepared(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}
MonitoringQ1GitBoundedChild::Prepared::Prepared(Prepared&&) noexcept = default;
MonitoringQ1GitBoundedChild::Prepared::~Prepared() = default;
auto MonitoringQ1GitBoundedChild::prepare(const std::size_t stdout_limit) -> Prepared {
    if (stdout_limit == 0 || stdout_limit > maximum_stdout_cap)
        throw std::invalid_argument{"private Git stdout limit"};
    return Prepared{std::make_unique<Prepared::State>(stdout_limit)};
}
auto MonitoringQ1GitBoundedChild::collect(Prepared&& prepared, std::array<int, 4> raw,
                                          const MonitoringQ1GitBoundedChildOperations& o,
                                          MonitoringQ1GitInitialCloseFailures initial)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    if (raw[0] <= 0 || raw[1] < 0 || raw[2] < 0 || raw[3] < 0 || raw[1] == raw[2] ||
        raw[1] == raw[3] || raw[2] == raw[3] || !o.poll || !o.read || !o.kill || !o.wait ||
        !o.await_outer_termination || !o.close)
        return std::unexpected{MonitoringQ1GitCommandFailure{make(Kind::setup), {}}};
    auto& out = prepared.state_->out;
    auto& stocks = prepared.state_->stocks;
    auto& projected = prepared.state_->projected;
    auto& cleanup = prepared.state_->cleanup;
    const auto stdout_limit = prepared.state_->stdout_limit;
    std::size_t cleanup_count = 0;
    Record primary{initial.count == 0 ? Kind::none : Kind::close,
                   initial.count == 0 ? 0 : initial.errors[0]};
    for (std::size_t i = 1; i < initial.count; ++i)
        cleanup[cleanup_count++] = {Kind::close, initial.errors[i]};
    std::array<int, 3> fd{raw[1], raw[2], raw[3]};
    std::array<bool, 3> open{true, true, true};
    bool active = true;
    if (o.adopt)
        o.adopt(o.adopt_context ? o.adopt_context : o.context);
    ActiveOwner owner{raw, o};
    auto note = [&](Kind k, int e = 0) noexcept {
        if (primary.kind == Kind::none)
            primary = {k, e};
        else if (cleanup_count < cleanup.size())
            cleanup[cleanup_count++] = {k, e};
        else
            std::terminate();
    };
    std::array<char, 4096> buffer{};
    std::array<char, sizeof(SetupRecord)> setup_bytes{};
    std::size_t setup_size = 0;
    while (primary.kind == Kind::none && (open[0] || open[1] || open[2])) {
        std::array<MonitoringQ1GitPollEvent, 3> events{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < 3; ++i)
            if (open[i])
                events[count++] = {fd[i], 0};
        const int p = o.poll(o.context, events.data(), count);
        if (p < 0) {
            note(Kind::poll, errno);
            break;
        }
        if (p == 0) {
            note(Kind::poll);
            break;
        }
        if (!valid_events(events.data(), count, fd, open)) {
            note(Kind::poll);
            break;
        }
        bool invalid = false;
        for (std::size_t j = 0; j < count; ++j)
            invalid = invalid || (events[j].revents & POLLNVAL) != 0;
        if (invalid) {
            note(Kind::poll);
            break;
        }
        bool progressed = false;
        for (std::size_t j = 0; j < count; ++j) {
            if ((events[j].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0)
                continue;
            progressed = true;
            std::size_t i = 0;
            while (fd[i] != events[j].descriptor)
                ++i;
            const long n = o.read(o.context, fd[i], buffer.data(), buffer.size());
            if (n < 0) {
                note(Kind::read, errno);
                break;
            }
            if (n == 0) {
                open[i] = false;
                if (i == 2 && setup_size != 0) {
                    SetupRecord record{};
                    if (setup_size == sizeof(record))
                        std::memcpy(&record, setup_bytes.data(), sizeof(record));
                    if (setup_size != sizeof(record) || record.magic != setup_magic ||
                        record.version != 1U || record.disposition == 0U ||
                        record.disposition > 4U || record.error <= 0)
                        note(Kind::setup);
                    else
                        note(Kind::setup, record.error);
                }
                continue;
            }
            if (i == 2) {
                const auto size = static_cast<std::size_t>(n);
                if (size > setup_bytes.size() - setup_size) {
                    note(Kind::setup);
                    break;
                }
                std::memcpy(setup_bytes.data() + setup_size, buffer.data(), size);
                setup_size += size;
                continue;
            }
            auto& target = i == 0 ? out.standard_output : out.standard_error;
            const auto size = static_cast<std::size_t>(n);
            const auto limit = i == 0 ? stdout_limit : cap;
            if (target.size() > limit || size > limit - target.size()) {
                note(Kind::limit);
                break;
            }
            target.append(buffer.data(), size);
        }
        if (primary.kind != Kind::none)
            break;
        if (!progressed) {
            note(Kind::poll);
            break;
        }
    }
    if (primary.kind != Kind::none && o.kill(o.context, raw[0], SIGKILL) != 0)
        note(Kind::signal, errno);
    if (primary.kind != Kind::none) {
        while (open[0] || open[1] || open[2]) {
            std::array<MonitoringQ1GitPollEvent, 3> events{};
            std::size_t count = 0;
            for (std::size_t i = 0; i < 3; ++i)
                if (open[i])
                    events[count++] = {fd[i], 0};
            const int p = o.poll(o.context, events.data(), count);
            if (p <= 0) {
                note(Kind::drain, p < 0 ? errno : 0);
                break;
            }
            if (!valid_events(events.data(), count, fd, open)) {
                note(Kind::drain);
                break;
            }
            bool invalid = false;
            for (std::size_t j = 0; j < count; ++j)
                invalid = invalid || (events[j].revents & POLLNVAL) != 0;
            if (invalid) {
                note(Kind::drain);
                break;
            }
            bool progressed = false;
            for (std::size_t j = 0; j < count; ++j) {
                if ((events[j].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0)
                    continue;
                progressed = true;
                std::size_t i = 0;
                while (fd[i] != events[j].descriptor)
                    ++i;
                const long n = o.read(o.context, fd[i], buffer.data(), buffer.size());
                if (n <= 0) {
                    if (n < 0)
                        note(Kind::drain, errno);
                    open[i] = false;
                }
            }
            if (!progressed) {
                note(Kind::drain);
                break;
            }
        }
    }
    int status{};
    for (;;) {
        const int w = o.wait(o.context, raw[0], &status);
        if (w == raw[0]) {
            active = false;
            break;
        }
        if (w < 0 && errno == EINTR)
            continue;
        note(Kind::reap, w < 0 ? errno : 0);
        o.await_outer_termination(o.context);
        std::terminate();
    }
    if (active)
        std::terminate();
    if (primary.kind == Kind::none && status != 0)
        primary = {Kind::exit, 0};
    for (auto it = fd.rbegin(); it != fd.rend(); ++it)
        if (o.close(o.context, *it) != 0)
            note(Kind::close, errno);
    owner.disarm();
    if (primary.kind == Kind::none)
        return out;
    for (std::size_t i = 0; i < cleanup_count; ++i)
        projected.push_back(cleanup[i].error == 0
                                ? std::move(stocks[i + 1U][index(cleanup[i].kind)])
                                : make(cleanup[i].kind, cleanup[i].error));
    return std::unexpected{
        MonitoringQ1GitCommandFailure{primary.error == 0 ? std::move(stocks[0][index(primary.kind)])
                                                         : make(primary.kind, primary.error),
                                      std::move(projected)}};
}

auto MonitoringQ1GitBoundedChildTestAccess::setup_record(std::int32_t error,
                                                         std::uint16_t disposition)
    -> std::array<char, 12> {
    SetupRecord record{setup_magic, 1U, disposition, error};
    std::array<char, sizeof(record)> bytes{};
    std::memcpy(bytes.data(), &record, sizeof(record));
    return bytes;
}
void MonitoringQ1GitBoundedChildTestAccess::emergency_retire(
    std::array<int, 4> raw, const MonitoringQ1GitBoundedChildTestOperations& operations) noexcept {
    ActiveOwner owner{raw, operations};
}
} // namespace epycbench::monitoring::qualification::detail
