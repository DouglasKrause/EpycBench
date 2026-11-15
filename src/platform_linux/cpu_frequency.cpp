#include "epycbench/platform_linux/cpu_frequency.hpp"

#include "epycbench/platform_linux/file.hpp"
#include "platform_linux/cpu_frequency_access.hpp"
#include "platform_linux/cpu_frequency_prepared_discovery_test_access.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <string_view>
#include <utility>

namespace epycbench::platform_linux {
namespace {

[[maybe_unused]] constexpr auto pairwise_enumeration_capacity_source_anchor =
    "linux.cpu_frequency.enumeration_capacity_exceeded";
[[maybe_unused]] constexpr auto pairwise_token_capacity_source_anchor =
    "linux.cpu_frequency.token_capacity_exceeded";

auto frequency_error(std::string_view code, std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Linux CPU frequency operation failed",
            .operation = "cpu_frequency",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto production_read(void*, const std::string& path) -> std::expected<std::string, core::Error> {
    auto value = read_text_file(path);
    if (!value) {
        return std::unexpected{frequency_error("linux.cpu_frequency.read_failed", "read")};
    }
    return value;
}

auto file_contents(std::span<const detail::CpuFrequencyFile> files, const std::string& path)
    -> const std::string* {
    const auto found = std::ranges::find(files, path, &detail::CpuFrequencyFile::path);
    return found == files.end() ? nullptr : &found->contents;
}

auto bounded_trim(std::string_view value) noexcept {
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!value.empty() && space(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && space(value.back()))
        value.remove_suffix(1);
    return value;
}

auto bounded_cpu_list(std::string_view input, std::vector<core::LinuxCpuId>& output,
                      bool& exhausted) noexcept -> bool {
    exhausted = false;
    output.clear();
    const auto text = bounded_trim(input);
    if (text.empty())
        return false;
    std::size_t start{};
    while (start < text.size()) {
        const auto comma = text.find(',', start);
        const auto end = comma == std::string_view::npos ? text.size() : comma;
        const auto dash = text.find('-', start);
        const auto parse = [&](std::size_t first, std::size_t last,
                               std::uint32_t& value) noexcept {
            if (first == last)
                return false;
            const auto result =
                std::from_chars(text.data() + first, text.data() + last, value, 10);
            return result.ec == std::errc{} && result.ptr == text.data() + last;
        };
        std::uint32_t first{}, last{};
        if (dash == std::string_view::npos || dash >= end) {
            if (!parse(start, end, first))
                return false;
            last = first;
        } else if (dash == start || dash + 1 == end || text.find('-', dash + 1) < end ||
                   !parse(start, dash, first) || !parse(dash + 1, end, last) || first > last) {
            return false;
        }
        for (std::uint64_t value = first;; ++value) {
            const auto cpu = core::LinuxCpuId{static_cast<std::uint32_t>(value)};
            const auto position = std::ranges::lower_bound(output, cpu);
            if (position == output.end() || *position != cpu) {
                if (output.size() == output.capacity()) {
                    exhausted = true;
                    return false;
                }
                output.insert(position, cpu);
            }
            if (value == last)
                break;
        }
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
        if (start == text.size())
            return false;
    }
    return !output.empty();
}

auto bounded_token(std::string_view value) noexcept -> std::optional<std::string_view> {
    if (value.ends_with("\r\n"))
        value.remove_suffix(2);
    else if (value.ends_with('\n'))
        value.remove_suffix(1);
    if (value.empty() || std::ranges::any_of(value, [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte < 0x20U || byte == 0x7fU;
        }))
        return std::nullopt;
    return value;
}

auto bounded_khz(std::string_view input) noexcept -> std::optional<std::uint64_t> {
    const auto value = bounded_trim(input);
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed == 0U || parsed > std::numeric_limits<std::uint64_t>::max() / 1000U)
        return std::nullopt;
    return parsed * 1000U;
}

constexpr auto cpufreq_root = "/sys/devices/system/cpu/cpufreq";

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

class FrequencyDescriptorRole final {
  public:
    FrequencyDescriptorRole(detail::BoundedSysfsOperations operations,
                            int descriptor,
                            detail::BoundedSysfsDispositionStock* stock = nullptr) noexcept
        : operations_{operations}, descriptor_{descriptor}, stock_{stock} {}
    FrequencyDescriptorRole(const FrequencyDescriptorRole&) = delete;
    ~FrequencyDescriptorRole() noexcept { static_cast<void>(retire()); }
    [[nodiscard]] auto get() const noexcept { return descriptor_; }
    [[nodiscard]] auto retire() noexcept
        -> detail::BoundedSysfsCloseOutcome {
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
    detail::BoundedSysfsOperations operations_{};
    int descriptor_{-1};
    detail::BoundedSysfsDispositionStock* stock_{};
    bool stock_exhausted_{};
};

[[nodiscard]] auto frequency_source_failure(
    detail::BoundedCpuFrequencySourceFailureKind kind, int native = 0) noexcept
    -> detail::BoundedCpuFrequencySourceFailure {
    return {kind, native,
            native == 0 ? detail::BoundedSysfsDispositionKind::success
                        : detail::BoundedSysfsDispositionKind::native_failure};
}

[[nodiscard]] auto frequency_close_failure(
    detail::BoundedCpuFrequencySourceFailureKind kind,
    detail::BoundedSysfsCloseOutcome outcome) noexcept
    -> detail::BoundedCpuFrequencySourceFailure {
    if (outcome.stock_exhausted)
        return frequency_source_failure(
            detail::BoundedCpuFrequencySourceFailureKind::enumeration_capacity);
    return {kind, outcome.native_error, outcome.kind};
}

[[nodiscard]] auto read_frequency_attribute(
    detail::BoundedSysfsOperations operations, int parent, const char* name,
    std::span<char> buffer, std::size_t content_capacity, bool optional,
    detail::BoundedSysfsIdentity* retained_identity = nullptr,
    detail::BoundedSysfsDispositionStock* stock = nullptr,
    std::span<detail::BoundedCpuFrequencySourceFailure> cleanup = {},
    std::size_t* cleanup_count = nullptr, bool* hardware_unreadable = nullptr) noexcept
    -> std::expected<std::optional<std::string_view>,
                     detail::BoundedCpuFrequencySourceFailure> {
    const auto append_cleanup = [&](detail::BoundedSysfsCloseOutcome outcome) noexcept {
        if (!outcome.succeeded() && cleanup_count != nullptr &&
            *cleanup_count < cleanup.size())
            cleanup[(*cleanup_count)++] = frequency_close_failure(
                detail::BoundedCpuFrequencySourceFailureKind::enumerate, outcome);
    };
    const auto capacity_failure = []() noexcept {
        return frequency_source_failure(
            detail::BoundedCpuFrequencySourceFailureKind::enumeration_capacity);
    };
    const auto descriptor = operations.open_at(
        operations.context, parent, name,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        const int open_error = errno;
        if (optional && open_error == ENOENT)
            return std::optional<std::string_view>{};
        if (optional && hardware_unreadable != nullptr && open_error == EACCES &&
            operations.effective_uid != nullptr) {
            if (stock != nullptr && !stock->record_operation())
                return std::unexpected{capacity_failure()};
            const auto effective_uid = operations.effective_uid(operations.context);
            if (effective_uid != 0) {
                struct stat first{}, second{};
                if (stock != nullptr && !stock->record_operation())
                    return std::unexpected{capacity_failure()};
                if (operations.stat_at(operations.context, parent, name, &first) != 0)
                    return std::unexpected{frequency_source_failure(
                        detail::BoundedCpuFrequencySourceFailureKind::enumerate, errno)};
                if (stock != nullptr && !stock->record_operation())
                    return std::unexpected{capacity_failure()};
                if (operations.stat_at(operations.context, parent, name, &second) != 0)
                    return std::unexpected{frequency_source_failure(
                        detail::BoundedCpuFrequencySourceFailureKind::enumerate, errno)};
                if (!detail::cpu_frequency_unreadable_hardware_identity(first, second,
                                                                         effective_uid))
                    return std::unexpected{frequency_source_failure(
                        detail::BoundedCpuFrequencySourceFailureKind::enumerate, ESTALE)};
                *hardware_unreadable = true;
                if (retained_identity != nullptr)
                    *retained_identity = detail::bounded_sysfs_identity(first);
                return std::optional<std::string_view>{};
            }
        }
        return std::unexpected{frequency_source_failure(
            detail::BoundedCpuFrequencySourceFailureKind::enumerate, open_error)};
    }
    FrequencyDescriptorRole role{operations, descriptor, stock};
    struct stat initial{}, named{}, final_held{}, final_named{};
    errno = 0;
    if (operations.stat_fd(operations.context, descriptor, &initial) != 0 ||
        operations.stat_at(operations.context, parent, name, &named) != 0 ||
        !detail::bounded_sysfs_file_identity(initial) ||
        !detail::bounded_sysfs_same_identity(initial, named)) {
        const auto saved = errno == 0 ? ESTALE : errno;
        if (stock != nullptr && !stock->record_operation(saved)) {
            append_cleanup(role.retire());
            return std::unexpected{capacity_failure()};
        }
        append_cleanup(role.retire());
        if (role.stock_exhausted())
            return std::unexpected{capacity_failure()};
        return std::unexpected{frequency_source_failure(
            detail::BoundedCpuFrequencySourceFailureKind::enumerate,
            saved)};
    }
    if (stock != nullptr && !stock->record_operation()) {
        append_cleanup(role.retire());
        return std::unexpected{capacity_failure()};
    }
    std::size_t used{};
    while (used < buffer.size()) {
        std::ptrdiff_t count{};
        do {
            count = operations.read(
                operations.context, descriptor, buffer.data() + used,
                buffer.size() - used, static_cast<std::int64_t>(used));
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            const auto saved = errno;
            if (stock != nullptr && !stock->record_operation(saved)) {
                append_cleanup(role.retire());
                return std::unexpected{capacity_failure()};
            }
            append_cleanup(role.retire());
            if (role.stock_exhausted())
                return std::unexpected{capacity_failure()};
            return std::unexpected{frequency_source_failure(
                detail::BoundedCpuFrequencySourceFailureKind::enumerate, saved)};
        }
        if (count == 0)
            break;
        used += static_cast<std::size_t>(count);
    }
    if (stock != nullptr && !stock->record_operation()) {
        append_cleanup(role.retire());
        return std::unexpected{capacity_failure()};
    }
    errno = 0;
    if (operations.stat_fd(operations.context, descriptor, &final_held) != 0 ||
        operations.stat_at(operations.context, parent, name, &final_named) != 0 ||
        !detail::bounded_sysfs_same_identity(initial, final_held) ||
        !detail::bounded_sysfs_same_identity(named, final_named)) {
        const auto saved = errno == 0 ? ESTALE : errno;
        if (stock != nullptr && !stock->record_operation(saved)) {
            append_cleanup(role.retire());
            return std::unexpected{capacity_failure()};
        }
        append_cleanup(role.retire());
        if (role.stock_exhausted())
            return std::unexpected{capacity_failure()};
        return std::unexpected{frequency_source_failure(
            detail::BoundedCpuFrequencySourceFailureKind::enumerate,
            saved)};
    }
    if (stock != nullptr && !stock->record_operation()) {
        append_cleanup(role.retire());
        return std::unexpected{capacity_failure()};
    }
    const auto closed = role.retire();
    if (role.stock_exhausted())
        return std::unexpected{capacity_failure()};
    if (!closed.succeeded())
        return std::unexpected{frequency_close_failure(
            detail::BoundedCpuFrequencySourceFailureKind::enumerate, closed)};
    if (used > content_capacity)
        return std::unexpected{frequency_source_failure(
            detail::BoundedCpuFrequencySourceFailureKind::token_capacity)};
    if (retained_identity != nullptr)
        *retained_identity = detail::bounded_sysfs_identity(initial);
    return std::optional<std::string_view>{
        std::string_view{buffer.data(), used}};
}

} // namespace

struct detail::BoundedCpuFrequencySources::State final {
    struct Domain final {
        std::size_t membership_offset{};
        std::size_t membership_count{};
        CpuFrequencyCurrentKind kind{};
        std::string driver;
        std::string governor;
        std::string policy_name;
        std::string current_attribute;
        std::uint64_t minimum{};
        std::uint64_t maximum{};
        BoundedSysfsIdentity identity{};
        BoundedSysfsIdentity attribute_identity{};
        int descriptor{-1};
        int attribute_descriptor{-1};
        bool active{};
    };
    State(BoundedSysfsOperations operations_value, std::size_t policy_capacity,
          std::size_t membership_capacity, std::size_t token_capacity)
        : operations{operations_value}, content_capacity{token_capacity},
          membership_arena(membership_capacity, core::LinuxCpuId{0U}),
          domains(policy_capacity),
          gate_memberships(policy_capacity) {
        for (auto& domain : domains) {
            domain.driver.reserve(token_capacity);
            domain.governor.reserve(token_capacity);
            domain.policy_name.reserve(token_capacity);
            domain.current_attribute.reserve(token_capacity);
        }
    }
    BoundedSysfsOperations operations{};
    int root_descriptor{-1};
    BoundedSysfsIdentity root_identity{};
    std::size_t content_capacity{};
    std::vector<core::LinuxCpuId> membership_arena;
    std::vector<Domain> domains;
    std::vector<std::size_t> gate_memberships;
    std::size_t membership_count{};
    std::size_t domain_count{};
    ~State() noexcept {
        auto live = std::span<Domain>{domains}.first(domain_count);
        for (auto domain = live.rbegin(); domain != live.rend(); ++domain) {
            if (domain->active && domain->attribute_descriptor >= 0) {
                static_cast<void>(operations.close(operations.context,
                                                   domain->attribute_descriptor));
                domain->attribute_descriptor = -1;
            }
            if (domain->active && domain->descriptor >= 0) {
                static_cast<void>(operations.close(operations.context,
                                                   domain->descriptor));
                domain->active = false;
                domain->descriptor = -1;
            }
        }
        if (root_descriptor >= 0)
            static_cast<void>(operations.close(operations.context,
                                               root_descriptor));
    }
};

struct detail::PreparedBoundedCpuFrequencyDiscoveryStorage::State final {
    struct PolicyStorage final {
        std::uint64_t number{};
        std::string name;
        std::string membership;
        std::string driver;
        std::string governor;
        std::string minimum;
        std::string maximum;
        std::string hardware;
        std::string scaling;
        bool has_hardware{};
        bool has_scaling{};
        bool hardware_unreadable{};
        BoundedSysfsIdentity identity{};
        BoundedSysfsIdentity hardware_identity{};
        BoundedSysfsIdentity scaling_identity{};
    };
    // The availability bit occupies the original padding after the two byte-presence bits.
    static_assert(sizeof(PolicyStorage) == 392);
    static_assert(alignof(PolicyStorage) == 8);

    State(BoundedCpuFrequencyCapacity capacity_value,
          BoundedSysfsOperations operations_value,
          std::span<const core::LinuxCpuId> present,
          std::span<const core::LinuxCpuId> workers,
          std::size_t policy_capacity, std::size_t membership_capacity,
          std::size_t token_capacity_value, std::size_t token_storage,
          std::size_t cleanup_capacity,
          std::size_t operation_capacity, std::size_t retirement_capacity)
        : capacity{capacity_value}, operations{operations_value},
          present_cpus(present.begin(), present.end()),
          worker_cpus(workers.begin(), workers.end()),
          names(token_storage), text(token_storage),
          policies(policy_capacity), views(policy_capacity),
          cleanup_stock(cleanup_capacity),
          disposition_stock(BoundedSysfsDispositionStock::prepare(
              std::min(operation_capacity, operations_value.operation_stock_limit),
              std::min(retirement_capacity,
                       operations_value.retirement_stock_limit),
              operations_value.stock_observation)),
          final_state(std::make_unique<BoundedCpuFrequencySources::State>(
              operations_value, policy_capacity, membership_capacity,
              token_capacity_value)), token_capacity{token_capacity_value} {
        for (auto& policy : policies) {
            policy.name.reserve(token_capacity_value);
            policy.membership.reserve(token_capacity_value);
            policy.driver.reserve(token_capacity_value);
            policy.governor.reserve(token_capacity_value);
            policy.minimum.reserve(token_capacity_value);
            policy.maximum.reserve(token_capacity_value);
            policy.hardware.reserve(token_capacity_value);
            policy.scaling.reserve(token_capacity_value);
        }
        scratch.parsed.reserve(membership_capacity);
        scratch.selected.reserve(membership_capacity);
        scratch.seen.reserve(membership_capacity);
        scratch.selected_policies.reserve(
            std::min(workers.size(), present.size()));
    }

    BoundedCpuFrequencyCapacity capacity{};
    BoundedSysfsOperations operations{};
    std::vector<core::LinuxCpuId> present_cpus;
    std::vector<core::LinuxCpuId> worker_cpus;
    std::vector<char> names;
    std::vector<char> text;
    std::vector<PolicyStorage> policies;
    std::vector<CpuFrequencyPolicyView> views;
    CpuFrequencyBoundedScratch scratch;
    std::vector<BoundedCpuFrequencySourceFailure> cleanup_stock;
    BoundedSysfsDispositionStock disposition_stock;
    std::unique_ptr<BoundedCpuFrequencySources::State> final_state;
    std::size_t token_capacity{};
    std::size_t policy_count{};
    std::size_t view_count{};
    std::size_t cleanup_count{};
};

auto detail::PreparedBoundedCpuFrequencyDiscoveryTestAccess::observe(
    const PreparedBoundedCpuFrequencyDiscoveryStorage& prepared) noexcept
    -> std::optional<PreparedBoundedCpuFrequencyDiscoveryObservation> {
    if (!prepared.state_ || !prepared.state_->final_state)
        return std::nullopt;
    const auto& state = *prepared.state_;
    const auto& final = *state.final_state;
    return PreparedBoundedCpuFrequencyDiscoveryObservation{
        .state = &state,
        .present = state.present_cpus.data(),
        .present_size = state.present_cpus.size(),
        .present_capacity = state.present_cpus.capacity(),
        .workers = state.worker_cpus.data(),
        .worker_size = state.worker_cpus.size(),
        .worker_capacity = state.worker_cpus.capacity(),
        .names = state.names.data(),
        .names_capacity = state.names.capacity(),
        .text = state.text.data(),
        .text_capacity = state.text.capacity(),
        .policies = state.policies.data(),
        .policy_capacity = state.policies.capacity(),
        .views = state.views.data(),
        .view_capacity = state.views.capacity(),
        .parsed = state.scratch.parsed.data(),
        .parsed_capacity = state.scratch.parsed.capacity(),
        .selected = state.scratch.selected.data(),
        .selected_capacity = state.scratch.selected.capacity(),
        .seen = state.scratch.seen.data(),
        .seen_capacity = state.scratch.seen.capacity(),
        .selected_policies = state.scratch.selected_policies.data(),
        .selected_policy_capacity = state.scratch.selected_policies.capacity(),
        .cleanup = state.cleanup_stock.data(),
        .cleanup_capacity = state.cleanup_stock.capacity(),
        .final_state = &final,
        .membership_arena = final.membership_arena.data(),
        .membership_size = final.membership_arena.size(),
        .membership_capacity = final.membership_arena.capacity(),
        .domains = final.domains.data(),
        .domain_size = final.domains.size(),
        .domain_capacity = final.domains.capacity(),
        .policy_count = state.policy_count,
        .view_count = state.view_count,
        .cleanup_count = state.cleanup_count,
        .membership_count = final.membership_count,
        .domain_count = final.domain_count};
}

auto detail::PreparedBoundedCpuFrequencyDiscoveryTestAccess::observe(
    const BoundedCpuFrequencySources& source) noexcept
    -> std::optional<PreparedBoundedCpuFrequencyDiscoveryObservation> {
    if (!source.state_)
        return std::nullopt;
    const auto& final = *source.state_;
    return PreparedBoundedCpuFrequencyDiscoveryObservation{
        .final_state = &final,
        .membership_arena = final.membership_arena.data(),
        .membership_size = final.membership_arena.size(),
        .membership_capacity = final.membership_arena.capacity(),
        .domains = final.domains.data(),
        .domain_size = final.domains.size(),
        .domain_capacity = final.domains.capacity(),
        .membership_count = final.membership_count,
        .domain_count = final.domain_count};
}

auto detail::PreparedBoundedCpuFrequencyDiscoveryTestAccess::pre_gate_storage(
    const PreparedBoundedCpuFrequencyDiscoveryStorage& prepared) noexcept
    -> std::optional<PreparedBoundedCpuFrequencyGateStorageObservation> {
    if (!prepared.state_ || !prepared.state_->final_state)
        return std::nullopt;
    const auto& memberships = prepared.state_->final_state->gate_memberships;
    return PreparedBoundedCpuFrequencyGateStorageObservation{
        .data = memberships.data(),
        .size = memberships.size(),
        .capacity = memberships.capacity()};
}

auto detail::PreparedBoundedCpuFrequencyDiscoveryTestAccess::strings(
    const PreparedBoundedCpuFrequencyDiscoveryStorage& prepared,
    std::size_t index) noexcept
    -> std::optional<PreparedBoundedCpuFrequencyStringObservation> {
    if (!prepared.state_ || !prepared.state_->final_state ||
        index >= prepared.state_->policies.size() ||
        index >= prepared.state_->final_state->domains.size())
        return std::nullopt;
    const auto& policy = prepared.state_->policies[index];
    const auto& domain = prepared.state_->final_state->domains[index];
    return PreparedBoundedCpuFrequencyStringObservation{
        .policy_data = {policy.name.data(), policy.membership.data(),
                        policy.driver.data(), policy.governor.data(),
                        policy.minimum.data(), policy.maximum.data(),
                        policy.hardware.data(), policy.scaling.data()},
        .policy_capacity = {
            policy.name.capacity(), policy.membership.capacity(),
            policy.driver.capacity(), policy.governor.capacity(),
            policy.minimum.capacity(), policy.maximum.capacity(),
            policy.hardware.capacity(), policy.scaling.capacity()},
        .domain_data = {domain.driver.data(), domain.governor.data(),
                        domain.policy_name.data(),
                        domain.current_attribute.data()},
        .domain_capacity = {
            domain.driver.capacity(), domain.governor.capacity(),
            domain.policy_name.capacity(),
            domain.current_attribute.capacity()}};
}

auto detail::PreparedBoundedCpuFrequencyDiscoveryTestAccess::strings(
    const BoundedCpuFrequencySources& source, std::size_t index) noexcept
    -> std::optional<PreparedBoundedCpuFrequencyStringObservation> {
    if (!source.state_ || index >= source.state_->domains.size())
        return std::nullopt;
    const auto& domain = source.state_->domains[index];
    return PreparedBoundedCpuFrequencyStringObservation{
        .domain_data = {domain.driver.data(), domain.governor.data(),
                        domain.policy_name.data(),
                        domain.current_attribute.data()},
        .domain_capacity = {
            domain.driver.capacity(), domain.governor.capacity(),
            domain.policy_name.capacity(),
            domain.current_attribute.capacity()}};
}

CpuFrequencyPolicySource::CpuFrequencyPolicySource(std::vector<core::LinuxCpuId> related_cpus,
                                                   CpuFrequencyCurrentKind kind, std::string driver,
                                                   std::string governor, std::uint64_t minimum_hz,
                                                   std::uint64_t maximum_hz,
                                                   std::string current_path, void* context,
                                                   ReadOperation read_operation) noexcept
    : related_cpus_{std::move(related_cpus)}, kind_{kind}, driver_{std::move(driver)},
      governor_{std::move(governor)}, minimum_hz_{minimum_hz}, maximum_hz_{maximum_hz},
      current_path_{std::move(current_path)}, context_{context}, read_{read_operation} {}

CpuFrequencyPolicySource::CpuFrequencyPolicySource(CpuFrequencyPolicySource&&) noexcept = default;
auto CpuFrequencyPolicySource::operator=(CpuFrequencyPolicySource&&) noexcept
    -> CpuFrequencyPolicySource& = default;
CpuFrequencyPolicySource::~CpuFrequencyPolicySource() noexcept = default;

auto detail::CpuFrequencyAccess::parse_khz(std::string_view text)
    -> std::expected<std::uint64_t, core::Error> {
    auto parsed = parse_integer<std::uint64_t>(text, "parse_cpu_frequency");
    if (!parsed || *parsed == 0U || *parsed > std::numeric_limits<std::uint64_t>::max() / 1000U) {
        return std::unexpected{frequency_error("linux.cpu_frequency.invalid_value", "value")};
    }
    return *parsed * 1000U;
}

auto detail::CpuFrequencyAccess::parse_khz_bounded(std::string_view text) noexcept
    -> std::optional<std::uint64_t> {
    return bounded_khz(text);
}

auto detail::CpuFrequencyAccess::project(core::LinuxCpuId cpu,
                                         std::span<const CpuFrequencyPolicyView> policies)
    -> std::expected<CpuFrequencyDetailedOutcome, core::Error> {
    std::size_t scratch_capacity{};
    for (const auto& policy : policies) {
        auto cpus = parse_cpu_list(policy.related_cpus);
        if (!cpus)
            return std::unexpected{
                frequency_error("linux.cpu_frequency.invalid_value", "membership")};
        const auto components = static_cast<std::uint64_t>(
                                    std::ranges::count(policy.related_cpus, ',')) +
                                1U;
        const auto cardinality = cpus->cardinality();
        if (cardinality != 0U &&
            components > std::numeric_limits<std::size_t>::max() / cardinality)
            return std::unexpected{
                frequency_error("linux.cpu_frequency.invalid_value", "membership")};
        scratch_capacity = std::max(
            scratch_capacity, static_cast<std::size_t>(components * cardinality));
    }
    CpuFrequencyBoundedScratch scratch;
    scratch.parsed.reserve(scratch_capacity);
    scratch.selected.reserve(scratch_capacity);
    auto bounded = project_bounded(cpu, policies, scratch);
    if (!bounded) {
        const auto classification = bounded.error() == CpuFrequencyBoundedFailure::ambiguous
                                        ? "ambiguous"
                                        : bounded.error() == CpuFrequencyBoundedFailure::membership
                                              ? "membership"
                                              : bounded.error() == CpuFrequencyBoundedFailure::metadata
                                                    ? "metadata"
                                                    : "value";
        const auto code = bounded.error() == CpuFrequencyBoundedFailure::ambiguous
                              ? "linux.cpu_frequency.ambiguous_policy"
                              : "linux.cpu_frequency.invalid_value";
        return std::unexpected{frequency_error(code, classification)};
    }
    if (!bounded->value)
        return CpuFrequencyDetailedOutcome{std::nullopt,
                                           bounded->unsupported};
    auto value = *bounded->value;
    return CpuFrequencyDetailedOutcome{
        CpuFrequencySemanticProjection{{value.related_cpus.begin(), value.related_cpus.end()},
                                       value.kind, std::string{value.driver},
                                       std::string{value.governor}, value.minimum_hz,
                                       value.maximum_hz, value.locator},
        std::nullopt};
}

auto detail::CpuFrequencyAccess::project_bounded(core::LinuxCpuId cpu,
                                                 std::span<const CpuFrequencyPolicyView> policies,
                                                 CpuFrequencyBoundedScratch& scratch) noexcept
    -> std::expected<CpuFrequencyBoundedOutcome, CpuFrequencyBoundedFailure> {
    const CpuFrequencyPolicyView* selected{};
    for (const auto& policy : policies) {
        bool exhausted{};
        if (!bounded_cpu_list(policy.related_cpus, scratch.parsed, exhausted))
            return std::unexpected{exhausted ? CpuFrequencyBoundedFailure::capacity
                                             : CpuFrequencyBoundedFailure::membership};
        if (std::ranges::binary_search(scratch.parsed, cpu)) {
            if (selected)
                return std::unexpected{CpuFrequencyBoundedFailure::ambiguous};
            selected = &policy;
            scratch.selected.clear();
            if (scratch.parsed.size() > scratch.selected.capacity())
                return std::unexpected{CpuFrequencyBoundedFailure::membership};
            scratch.selected.insert(scratch.selected.end(), scratch.parsed.begin(),
                                    scratch.parsed.end());
        }
    }
    if (!selected)
        return CpuFrequencyBoundedOutcome{std::nullopt,
                                          CpuFrequencyUnsupportedReason::no_resolvable_policy};
    const auto driver = bounded_token(selected->scaling_driver);
    const auto governor = bounded_token(selected->scaling_governor);
    const auto minimum = bounded_khz(selected->scaling_minimum);
    const auto maximum = bounded_khz(selected->scaling_maximum);
    if (!driver || !governor || !minimum || !maximum || *minimum > *maximum)
        return std::unexpected{CpuFrequencyBoundedFailure::metadata};
    const auto hardware = selected->hardware_current
                              ? bounded_khz(*selected->hardware_current)
                              : std::optional<std::uint64_t>{};
    const auto scaling = selected->scaling_current ? bounded_khz(*selected->scaling_current)
                                                   : std::optional<std::uint64_t>{};
    if (!hardware && !scaling && !selected->hardware_current && !selected->scaling_current)
        return CpuFrequencyBoundedOutcome{std::nullopt,
                                          CpuFrequencyUnsupportedReason::no_current_attribute};
    if (!hardware && !scaling)
        return std::unexpected{CpuFrequencyBoundedFailure::value};
    return CpuFrequencyBoundedOutcome{
        CpuFrequencyBoundedProjection{scratch.selected,
                                      hardware ? CpuFrequencyCurrentKind::hardware_current
                                               : CpuFrequencyCurrentKind::scaling_current,
                                      *driver, *governor, *minimum, *maximum, selected->locator},
        std::nullopt};
}

auto detail::CpuFrequencyAccess::discover_bounded(
    core::LinuxCpuId cpu, std::span<const CpuFrequencyPolicyView> policies,
    std::uint64_t maximum_policies, std::uint64_t maximum_memberships,
    std::uint64_t token_bytes, CpuFrequencyBoundedScratch& scratch) noexcept
    -> std::expected<CpuFrequencyBoundedOutcome,
                     CpuFrequencyBoundedDiscoveryFailure> {
    if (policies.size() > maximum_policies ||
        scratch.parsed.capacity() < maximum_memberships ||
        scratch.selected.capacity() < maximum_memberships)
        return std::unexpected{CpuFrequencyBoundedDiscoveryFailure{
            CpuFrequencyBoundedDiscoveryFailureKind::enumeration_capacity}};
    for (const auto& policy : policies) {
        const std::array tokens{policy.related_cpus, policy.scaling_driver,
                                policy.scaling_governor, policy.scaling_minimum,
                                policy.scaling_maximum};
        if (std::ranges::any_of(tokens, [&](std::string_view value) {
                return value.size() > token_bytes;
            }) ||
            (policy.hardware_current && policy.hardware_current->size() > token_bytes) ||
            (policy.scaling_current && policy.scaling_current->size() > token_bytes))
            return std::unexpected{CpuFrequencyBoundedDiscoveryFailure{
                CpuFrequencyBoundedDiscoveryFailureKind::token_capacity}};
    }
    auto value = project_bounded(cpu, policies, scratch);
    if (!value)
        return std::unexpected{CpuFrequencyBoundedDiscoveryFailure{
            CpuFrequencyBoundedDiscoveryFailureKind::semantic, value.error()}};
    if (value->value && value->value->related_cpus.size() > maximum_memberships)
        return std::unexpected{CpuFrequencyBoundedDiscoveryFailure{
            CpuFrequencyBoundedDiscoveryFailureKind::enumeration_capacity}};
    return *value;
}

auto detail::CpuFrequencyAccess::validate_graph_bounded(
    std::span<const core::LinuxCpuId> present_cpus,
    std::span<const core::LinuxCpuId> worker_cpus,
    std::span<const CpuFrequencyPolicyView> policies,
    std::uint64_t maximum_policies, std::uint64_t maximum_memberships,
    std::uint64_t token_bytes, CpuFrequencyBoundedScratch& scratch) noexcept
    -> std::expected<CpuFrequencyBoundedGraph,
                     CpuFrequencyBoundedDiscoveryFailure> {
    const auto capacity_failure = [] {
        return std::unexpected{CpuFrequencyBoundedDiscoveryFailure{
            CpuFrequencyBoundedDiscoveryFailureKind::enumeration_capacity}};
    };
    const auto semantic_failure = [](CpuFrequencyBoundedFailure failure) {
        return std::unexpected{CpuFrequencyBoundedDiscoveryFailure{
            CpuFrequencyBoundedDiscoveryFailureKind::semantic, failure}};
    };
    if (present_cpus.empty() || policies.size() > maximum_policies ||
        present_cpus.size() > maximum_memberships ||
        scratch.parsed.capacity() < maximum_memberships ||
        scratch.seen.capacity() < maximum_memberships ||
        scratch.selected_policies.capacity() <
            std::min(worker_cpus.size(), present_cpus.size()))
        return capacity_failure();
    if (!std::ranges::is_sorted(present_cpus) ||
        std::ranges::adjacent_find(present_cpus) != present_cpus.end() ||
        !std::ranges::is_sorted(worker_cpus) ||
        std::ranges::adjacent_find(worker_cpus) != worker_cpus.end() ||
        std::ranges::any_of(worker_cpus, [&](core::LinuxCpuId cpu) {
            return !std::ranges::binary_search(present_cpus, cpu);
        }))
        return semantic_failure(CpuFrequencyBoundedFailure::membership);

    scratch.seen.clear();
    scratch.selected_policies.clear();
    std::uint64_t selected_memberships{};
    std::optional<std::uint32_t> previous_locator;
    for (const auto& policy : policies) {
        if ((previous_locator && policy.locator <= *previous_locator) ||
            policy.locator >= maximum_policies)
            return semantic_failure(CpuFrequencyBoundedFailure::metadata);
        previous_locator = policy.locator;
        const std::array tokens{policy.related_cpus, policy.scaling_driver,
                                policy.scaling_governor, policy.scaling_minimum,
                                policy.scaling_maximum};
        if (std::ranges::any_of(tokens, [&](std::string_view value) {
                return value.size() > token_bytes;
            }) ||
            (policy.hardware_current && policy.hardware_current->size() > token_bytes) ||
            (policy.scaling_current && policy.scaling_current->size() > token_bytes))
            return std::unexpected{CpuFrequencyBoundedDiscoveryFailure{
                CpuFrequencyBoundedDiscoveryFailureKind::token_capacity}};
        bool exhausted{};
        if (!bounded_cpu_list(policy.related_cpus, scratch.parsed, exhausted))
            return exhausted ? capacity_failure()
                             : semantic_failure(CpuFrequencyBoundedFailure::membership);
        bool selected{};
        for (const auto cpu : scratch.parsed) {
            if (!std::ranges::binary_search(present_cpus, cpu))
                return semantic_failure(CpuFrequencyBoundedFailure::membership);
            const auto insertion = std::ranges::lower_bound(scratch.seen, cpu);
            if (insertion != scratch.seen.end() && *insertion == cpu)
                return semantic_failure(CpuFrequencyBoundedFailure::membership);
            if (scratch.seen.size() == scratch.seen.capacity())
                return capacity_failure();
            scratch.seen.insert(insertion, cpu);
            selected = selected || std::ranges::binary_search(worker_cpus, cpu);
        }
        if (selected) {
            if (scratch.selected_policies.size() ==
                scratch.selected_policies.capacity())
                return capacity_failure();
            scratch.selected_policies.push_back(policy.locator);
            if (selected_memberships > maximum_memberships - scratch.parsed.size())
                return capacity_failure();
            selected_memberships += scratch.parsed.size();
        }
    }
    if (scratch.seen.size() > maximum_memberships ||
        scratch.selected_policies.size() >
            std::min(worker_cpus.size(), present_cpus.size()))
        return capacity_failure();
    std::uint64_t covered_workers{};
    for (const auto worker : worker_cpus) {
        std::size_t matches{};
        for (const auto& policy : policies) {
            bool exhausted{};
            if (!bounded_cpu_list(policy.related_cpus, scratch.parsed, exhausted))
                return exhausted ? capacity_failure()
                                 : semantic_failure(CpuFrequencyBoundedFailure::membership);
            matches += static_cast<std::size_t>(
                std::ranges::binary_search(scratch.parsed, worker));
        }
        if (matches > 1U)
            return semantic_failure(CpuFrequencyBoundedFailure::ambiguous);
        covered_workers += static_cast<std::uint64_t>(matches);
    }
    return CpuFrequencyBoundedGraph{
        scratch.selected_policies.size(), selected_memberships, covered_workers,
        scratch.selected_policies};
}

detail::BoundedCpuFrequencySources::BoundedCpuFrequencySources(
    std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
detail::BoundedCpuFrequencySources::BoundedCpuFrequencySources(
    BoundedCpuFrequencySources&&) noexcept = default;
detail::BoundedCpuFrequencySources::~BoundedCpuFrequencySources() noexcept = default;

detail::PreparedBoundedCpuFrequencyDiscoveryStorage::
    PreparedBoundedCpuFrequencyDiscoveryStorage(
        std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
detail::PreparedBoundedCpuFrequencyDiscoveryStorage::
    PreparedBoundedCpuFrequencyDiscoveryStorage(
        PreparedBoundedCpuFrequencyDiscoveryStorage&&) noexcept = default;
detail::PreparedBoundedCpuFrequencyDiscoveryStorage::
    ~PreparedBoundedCpuFrequencyDiscoveryStorage() noexcept = default;

auto detail::PreparedBoundedCpuFrequencyDiscoveryStorage::prepare(
    BoundedCpuFrequencyCapacity capacity,
    std::span<const core::LinuxCpuId> present_cpus,
    std::span<const core::LinuxCpuId> worker_cpus,
    BoundedSysfsOperations operations) noexcept
    -> std::expected<PreparedBoundedCpuFrequencyDiscoveryStorage,
                     BoundedCpuFrequencySourceFailureSequence> {
    const auto fail = [](BoundedCpuFrequencySourceFailureKind kind,
                         int native = 0) {
        return std::unexpected{frequency_source_failure(kind, native)};
    };
    if (capacity.root_entries == 0U || capacity.maximum_policies == 0U ||
        capacity.maximum_memberships == 0U || capacity.token_bytes == 0U ||
        capacity.maximum_cpu == std::numeric_limits<std::uint64_t>::max() ||
        capacity.root_entries != capacity.maximum_cpu + 2U ||
        capacity.maximum_policies != capacity.maximum_cpu + 1U ||
        present_cpus.empty() || operations.open_root == nullptr ||
        operations.open_at == nullptr || operations.stat_fd == nullptr ||
        operations.stat_at == nullptr || operations.next == nullptr ||
        operations.read == nullptr || operations.close == nullptr)
        return fail(BoundedCpuFrequencySourceFailureKind::enumeration_capacity);
    if (present_cpus.size() > capacity.maximum_policies ||
        std::ranges::any_of(present_cpus, [&](core::LinuxCpuId cpu) {
            return static_cast<std::uint64_t>(cpu.value()) > capacity.maximum_cpu;
        }))
        return fail(BoundedCpuFrequencySourceFailureKind::membership);

    std::size_t root_entries{}, policy_capacity{}, membership_capacity{};
    std::size_t token_capacity{}, token_storage{}, twice_policies{};
    std::size_t cleanup_capacity{}, thirty_one_policies{};
    std::size_t operation_capacity{}, retirement_capacity{};
    if (!checked_size(capacity.root_entries, root_entries) ||
        !checked_size(capacity.maximum_policies, policy_capacity) ||
        !checked_size(capacity.maximum_memberships, membership_capacity) ||
        !checked_size(capacity.token_bytes, token_capacity) ||
        !checked_add(token_capacity, 1U, token_storage) ||
        !checked_multiply(2U, policy_capacity, twice_policies) ||
        !checked_add(twice_policies, 1U, cleanup_capacity) ||
        !checked_multiply(31U, policy_capacity, thirty_one_policies) ||
        !checked_add(root_entries, 3U, operation_capacity) ||
        !checked_add(operation_capacity, thirty_one_policies,
                     operation_capacity) ||
        !checked_multiply(8U, policy_capacity, retirement_capacity) ||
        !checked_add(retirement_capacity, 1U, retirement_capacity))
        return fail(BoundedCpuFrequencySourceFailureKind::enumeration_capacity);
    try {
        return PreparedBoundedCpuFrequencyDiscoveryStorage{std::make_unique<State>(
            capacity, operations, present_cpus, worker_cpus, policy_capacity,
            membership_capacity, token_capacity, token_storage, cleanup_capacity,
            operation_capacity, retirement_capacity)};
    } catch (const std::bad_alloc&) {
        return fail(BoundedCpuFrequencySourceFailureKind::enumerate);
    } catch (...) {
        return fail(BoundedCpuFrequencySourceFailureKind::contained);
    }
}

auto detail::BoundedCpuFrequencySources::discover_prepared_engine(
    PreparedBoundedCpuFrequencyDiscoveryStorage& prepared,
    BoundedCpuFrequencyValidationGate gate) noexcept
    -> std::expected<std::optional<BoundedCpuFrequencySources>,
                     BoundedCpuFrequencySourceFailureSequence> {
    const auto fail = [](BoundedCpuFrequencySourceFailureKind kind,
                         int native = 0) {
        return std::unexpected{frequency_source_failure(kind, native)};
    };
    if (!prepared.state_)
        return fail(BoundedCpuFrequencySourceFailureKind::enumerate);
    auto* prepared_state = prepared.state_.get();
    auto& capacity = prepared_state->capacity;
    auto& operations = prepared_state->operations;
    auto& present_cpus = prepared_state->present_cpus;
    auto& worker_cpus = prepared_state->worker_cpus;
    auto& names = prepared_state->names;
    auto& text = prepared_state->text;
    auto& policies = prepared_state->policies;
    auto& views = prepared_state->views;
    auto& scratch = prepared_state->scratch;
    auto& cleanup_stock = prepared_state->cleanup_stock;
    auto& disposition_stock = prepared_state->disposition_stock;
    auto& cleanup_count = prepared_state->cleanup_count;
    auto& policy_count = prepared_state->policy_count;
    auto& view_count = prepared_state->view_count;
    auto& final_state = prepared_state->final_state;
    using PolicyStorage =
        PreparedBoundedCpuFrequencyDiscoveryStorage::State::PolicyStorage;
    const auto token_capacity = prepared_state->token_capacity;
    if (policy_count != 0U || view_count != 0U || cleanup_count != 0U ||
        !final_state || final_state->domain_count != 0U ||
        final_state->membership_count != 0U || final_state->root_descriptor >= 0 ||
        disposition_stock.operation_count() != 0U ||
        disposition_stock.retirement_count() != 0U)
        return fail(BoundedCpuFrequencySourceFailureKind::enumerate);
    const auto append_cleanup = [&](BoundedSysfsCloseOutcome outcome) noexcept {
        if (!outcome.succeeded() && cleanup_count < cleanup_stock.size())
            cleanup_stock[cleanup_count++] = frequency_close_failure(
                BoundedCpuFrequencySourceFailureKind::enumerate, outcome);
    };
    const auto sequence = [&](BoundedCpuFrequencySourceFailure primary) noexcept {
        return BoundedCpuFrequencySourceFailureSequence{primary, cleanup_count};
    };
    {
        const auto root_descriptor = operations.open_root(
            operations.context, cpufreq_root,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_DIRECTORY);
        if (root_descriptor < 0)
            return fail(BoundedCpuFrequencySourceFailureKind::enumerate, errno);
        FrequencyDescriptorRole root{operations, root_descriptor,
                                     &disposition_stock};
        struct stat root_initial{}, root_named{};
        errno = 0;
        if (operations.stat_fd(operations.context, root.get(), &root_initial) != 0 ||
            operations.stat_at(operations.context, AT_FDCWD, cpufreq_root,
                               &root_named) != 0 ||
            !bounded_sysfs_directory_identity(root_initial) ||
            !bounded_sysfs_same_identity(root_initial, root_named)) {
            const auto saved = errno == 0 ? ESTALE : errno;
            const auto closed = root.retire();
            append_cleanup(closed);
            return std::unexpected{sequence(frequency_source_failure(
                BoundedCpuFrequencySourceFailureKind::enumerate, saved))};
        }
        if (!disposition_stock.record_operation()) {
            append_cleanup(root.retire());
            return std::unexpected{sequence(frequency_source_failure(
                BoundedCpuFrequencySourceFailureKind::enumeration_capacity))};
        }
        BoundedSysfsEnumerationState enumeration{};
        std::uint64_t root_count{};
        for (;;) {
            const auto next = operations.next(operations.context, root.get(),
                                              &enumeration, names.data(), names.size());
            if (next < 0) {
                const auto saved = errno;
                const auto closed = root.retire();
                append_cleanup(closed);
                return std::unexpected{sequence(frequency_source_failure(
                    saved == ENAMETOOLONG
                        ? BoundedCpuFrequencySourceFailureKind::token_capacity
                        : BoundedCpuFrequencySourceFailureKind::enumerate,
                    saved == ENAMETOOLONG ? 0 : saved))};
            }
            if (next == 0)
                break;
            if (!disposition_stock.record_operation()) {
                append_cleanup(root.retire());
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumeration_capacity))};
            }
            if (++root_count > capacity.root_entries) {
                const auto closed = root.retire();
                append_cleanup(closed);
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumeration_capacity))};
            }
            const std::string_view name{names.data()};
            if (!name.starts_with("policy"))
                continue;
            std::uint64_t number{};
            const auto parsed = std::from_chars(name.data() + 6U,
                                                name.data() + name.size(), number);
            if (name.size() == 6U || parsed.ec != std::errc{} ||
                parsed.ptr != name.data() + name.size() ||
                number > capacity.maximum_cpu)
                continue;
            if (policy_count == policies.size()) {
                const auto closed = root.retire();
                append_cleanup(closed);
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumeration_capacity))};
            }
            const auto descriptor = operations.open_at(
                operations.context, root.get(), names.data(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_DIRECTORY);
            if (descriptor < 0)
                continue;
            FrequencyDescriptorRole policy_role{operations, descriptor,
                                                &disposition_stock};
            struct stat initial{}, named{}, final_held{}, final_named{};
            errno = 0;
            if (operations.stat_fd(operations.context, descriptor, &initial) != 0 ||
                operations.stat_at(operations.context, root.get(), names.data(),
                                   &named) != 0 ||
                !bounded_sysfs_directory_identity(initial) ||
                !bounded_sysfs_same_identity(initial, named)) {
                const auto saved = errno == 0 ? ESTALE : errno;
                const auto policy_close = policy_role.retire();
                const auto root_close = root.retire();
                append_cleanup(policy_close);
                append_cleanup(root_close);
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumerate, saved))};
            }
            if (!disposition_stock.record_operation()) {
                append_cleanup(policy_role.retire());
                append_cleanup(root.retire());
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumeration_capacity))};
            }
            auto& policy = policies[policy_count];
            policy.number = number;
            policy.name.assign(name);
            policy.identity = bounded_sysfs_identity(initial);
            const auto acquire = [&](const char* attribute, std::string& output,
                                     bool optional,
                                     BoundedSysfsIdentity* identity = nullptr,
                                     bool* hardware_unreadable = nullptr) noexcept
                -> std::expected<bool, BoundedCpuFrequencySourceFailure> {
                auto value = read_frequency_attribute(operations, descriptor, attribute,
                                                      text, token_capacity, optional,
                                                      identity, &disposition_stock,
                                                      cleanup_stock, &cleanup_count,
                                                      hardware_unreadable);
                if (!value)
                    return std::unexpected{value.error()};
                if (!*value)
                    return false;
                output.assign(**value);
                return true;
            };
            const auto membership = acquire("related_cpus", policy.membership, false);
            const auto driver = membership
                                    ? acquire("scaling_driver", policy.driver, false)
                                    : std::expected<bool, BoundedCpuFrequencySourceFailure>{
                                          std::unexpected{membership.error()}};
            const auto governor = driver
                                      ? acquire("scaling_governor", policy.governor, false)
                                      : std::expected<bool,
                                                      BoundedCpuFrequencySourceFailure>{
                                            std::unexpected{driver.error()}};
            const auto minimum = governor
                                     ? acquire("scaling_min_freq", policy.minimum, false)
                                     : std::expected<bool,
                                                     BoundedCpuFrequencySourceFailure>{
                                           std::unexpected{governor.error()}};
            const auto maximum = minimum
                                     ? acquire("scaling_max_freq", policy.maximum, false)
                                     : std::expected<bool,
                                                     BoundedCpuFrequencySourceFailure>{
                                           std::unexpected{minimum.error()}};
            const auto hardware = maximum
                                      ? acquire("cpuinfo_cur_freq", policy.hardware, true,
                                                &policy.hardware_identity,
                                                &policy.hardware_unreadable)
                                      : std::expected<bool,
                                                      BoundedCpuFrequencySourceFailure>{
                                            std::unexpected{maximum.error()}};
            const auto scaling = hardware
                                     ? acquire("scaling_cur_freq", policy.scaling, true,
                                               &policy.scaling_identity)
                                     : std::expected<bool,
                                                     BoundedCpuFrequencySourceFailure>{
                                           std::unexpected{hardware.error()}};
            if (!scaling) {
                const auto failure = scaling.error();
                append_cleanup(policy_role.retire());
                append_cleanup(root.retire());
                return std::unexpected{sequence(failure)};
            }
            policy.has_hardware = *hardware;
            policy.has_scaling = *scaling;
            errno = 0;
            if (operations.stat_fd(operations.context, descriptor, &final_held) != 0 ||
                operations.stat_at(operations.context, root.get(), names.data(),
                                   &final_named) != 0 ||
                !bounded_sysfs_same_identity(initial, final_held) ||
                !bounded_sysfs_same_identity(named, final_named)) {
                const auto saved = errno == 0 ? ESTALE : errno;
                const auto policy_close = policy_role.retire();
                const auto root_close = root.retire();
                append_cleanup(policy_close);
                append_cleanup(root_close);
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumerate, saved))};
            }
            if (!disposition_stock.record_operation()) {
                append_cleanup(policy_role.retire());
                append_cleanup(root.retire());
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumeration_capacity))};
            }
            if (const auto closed = policy_role.retire(); !closed.succeeded()) {
                append_cleanup(root.retire());
                return std::unexpected{sequence(frequency_close_failure(
                    BoundedCpuFrequencySourceFailureKind::enumerate, closed))};
            }
            ++policy_count;
        }
        struct stat root_final{}, root_final_named{};
        errno = 0;
        if (operations.stat_fd(operations.context, root.get(), &root_final) != 0 ||
            operations.stat_at(operations.context, AT_FDCWD, cpufreq_root,
                               &root_final_named) != 0 ||
            !bounded_sysfs_same_identity(root_initial, root_final) ||
            !bounded_sysfs_same_identity(root_named, root_final_named)) {
            const auto saved = errno == 0 ? ESTALE : errno;
            const auto closed = root.retire();
            append_cleanup(closed);
            return std::unexpected{sequence(frequency_source_failure(
                BoundedCpuFrequencySourceFailureKind::enumerate, saved))};
        }
        if (!disposition_stock.record_operation()) {
            append_cleanup(root.retire());
            return std::unexpected{sequence(frequency_source_failure(
                BoundedCpuFrequencySourceFailureKind::enumeration_capacity))};
        }
        if (const auto closed = root.retire(); !closed.succeeded())
            return std::unexpected{sequence(frequency_close_failure(
                BoundedCpuFrequencySourceFailureKind::enumerate, closed))};

        auto live_policies = std::span{policies}.first(policy_count);
        std::ranges::sort(live_policies, {}, &PolicyStorage::number);
        if (std::ranges::adjacent_find(live_policies, {},
                                       &PolicyStorage::number) !=
            live_policies.end())
            return fail(BoundedCpuFrequencySourceFailureKind::metadata);
        for (std::size_t index{}; index < live_policies.size(); ++index) {
            const auto& policy = policies[index];
            views[view_count++] = {
                static_cast<std::uint32_t>(index), policy.membership,
                policy.driver, policy.governor, policy.minimum, policy.maximum,
                policy.has_hardware
                    ? std::optional<std::string_view>{policy.hardware}
                    : std::nullopt,
                policy.has_scaling
                    ? std::optional<std::string_view>{policy.scaling}
                    : std::nullopt};
        }
        const auto live_views = std::span{views}.first(view_count);
        // This copied vector is a discovery membership set, not the indexed
        // worker/spec/readiness association retained by Runtime. Validate the
        // whole set before canonicalizing it for the strict pure graph kernel.
        if (!std::ranges::is_sorted(worker_cpus)) {
            if (!std::ranges::is_sorted(present_cpus) ||
                std::ranges::adjacent_find(present_cpus) != present_cpus.end())
                return fail(BoundedCpuFrequencySourceFailureKind::membership);
            for (std::size_t index{}; index < worker_cpus.size(); ++index) {
                if (!std::ranges::binary_search(present_cpus, worker_cpus[index]) ||
                    std::ranges::find(std::span{worker_cpus}.first(index), worker_cpus[index]) !=
                        std::span{worker_cpus}.first(index).end())
                    return fail(BoundedCpuFrequencySourceFailureKind::membership);
            }
            std::ranges::sort(worker_cpus);
        }
        auto graph = CpuFrequencyAccess::validate_graph_bounded(
            present_cpus, worker_cpus, live_views, capacity.maximum_policies,
            capacity.maximum_memberships, capacity.token_bytes, scratch);
        if (!graph) {
            if (graph.error().kind ==
                CpuFrequencyBoundedDiscoveryFailureKind::enumeration_capacity)
                return fail(BoundedCpuFrequencySourceFailureKind::enumeration_capacity);
            if (graph.error().kind ==
                CpuFrequencyBoundedDiscoveryFailureKind::token_capacity)
                return fail(BoundedCpuFrequencySourceFailureKind::token_capacity);
            const auto kind = graph.error().semantic == CpuFrequencyBoundedFailure::membership
                                  ? BoundedCpuFrequencySourceFailureKind::membership
                              : graph.error().semantic == CpuFrequencyBoundedFailure::metadata
                                  ? BoundedCpuFrequencySourceFailureKind::metadata
                              : graph.error().semantic == CpuFrequencyBoundedFailure::ambiguous
                                  ? BoundedCpuFrequencySourceFailureKind::ambiguous
                                  : BoundedCpuFrequencySourceFailureKind::value;
            return fail(kind);
        }
        if (graph->covered_worker_count != worker_cpus.size()) {
            final_state->gate_memberships.resize(0U);
            if (gate.accept != nullptr &&
                !gate.accept(gate.context,
                             BoundedCpuFrequencyValidatedGraphOutcome{
                                 BoundedCpuFrequencyValidatedDisposition::unsupported,
                                 std::move(final_state->gate_memberships)}))
                return fail(
                    BoundedCpuFrequencySourceFailureKind::preparation_rejected);
            prepared.state_.reset();
            return std::optional<BoundedCpuFrequencySources>{};
        }

        if (graph->domain_count > final_state->domains.size() ||
            graph->membership_count > final_state->membership_arena.size())
            return fail(
                BoundedCpuFrequencySourceFailureKind::enumeration_capacity);
        auto* state = final_state.get();

        std::size_t domain_index{};
        for (const auto locator : graph->selected_policies) {
            bool exhausted{};
            if (!bounded_cpu_list(views[locator].related_cpus, scratch.parsed,
                                  exhausted))
                return fail(exhausted
                                ? BoundedCpuFrequencySourceFailureKind::enumeration_capacity
                                : BoundedCpuFrequencySourceFailureKind::membership);
            const auto policy_view =
                live_views.subspan(locator, 1U);
            auto projected = CpuFrequencyAccess::project_bounded(
                scratch.parsed.front(), policy_view, scratch);
            if (!projected) {
                const auto kind = projected.error() == CpuFrequencyBoundedFailure::metadata
                                      ? BoundedCpuFrequencySourceFailureKind::metadata
                                  : projected.error() == CpuFrequencyBoundedFailure::value
                                      ? BoundedCpuFrequencySourceFailureKind::value
                                      : BoundedCpuFrequencySourceFailureKind::membership;
                return fail(kind);
            }
            if (!projected->value) {
                if (policies[locator].hardware_unreadable)
                    return fail(BoundedCpuFrequencySourceFailureKind::enumerate, EACCES);
                state->gate_memberships.resize(0U);
                if (gate.accept != nullptr &&
                    !gate.accept(gate.context,
                                 BoundedCpuFrequencyValidatedGraphOutcome{
                                     BoundedCpuFrequencyValidatedDisposition::unsupported,
                                     std::move(state->gate_memberships)}))
                    return fail(
                        BoundedCpuFrequencySourceFailureKind::preparation_rejected);
                prepared.state_.reset();
                return std::optional<BoundedCpuFrequencySources>{};
            }
            const auto& projection = *projected->value;
            auto& domain = state->domains[domain_index++];
            if (projection.related_cpus.size() >
                state->membership_arena.size() - state->membership_count)
                return fail(
                    BoundedCpuFrequencySourceFailureKind::enumeration_capacity);
            domain.membership_offset = state->membership_count;
            domain.membership_count = projection.related_cpus.size();
            std::ranges::copy(
                projection.related_cpus,
                state->membership_arena.begin() +
                    static_cast<std::ptrdiff_t>(state->membership_count));
            state->membership_count += projection.related_cpus.size();
            domain.kind = projection.kind;
            domain.driver.assign(projection.driver);
            domain.governor.assign(projection.governor);
            domain.minimum = projection.minimum_hz;
            domain.maximum = projection.maximum_hz;
            domain.policy_name = policies[locator].name;
            domain.current_attribute =
                projection.kind == CpuFrequencyCurrentKind::hardware_current
                    ? "cpuinfo_cur_freq"
                    : "scaling_cur_freq";
            domain.identity = policies[locator].identity;
            domain.attribute_identity =
                projection.kind == CpuFrequencyCurrentKind::hardware_current
                    ? policies[locator].hardware_identity
                    : policies[locator].scaling_identity;
            state->gate_memberships[domain_index - 1U] =
                domain.membership_count;
        }
        state->domain_count = domain_index;
        state->gate_memberships.resize(domain_index);
        if (gate.accept != nullptr &&
            !gate.accept(gate.context,
                         BoundedCpuFrequencyValidatedGraphOutcome{
                             BoundedCpuFrequencyValidatedDisposition::supported,
                             std::move(state->gate_memberships)}))
            return fail(BoundedCpuFrequencySourceFailureKind::preparation_rejected);
        const auto persistent_root_descriptor = operations.open_root(
            operations.context, cpufreq_root,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_DIRECTORY);
        if (persistent_root_descriptor < 0)
            return fail(BoundedCpuFrequencySourceFailureKind::enumerate, errno);
        FrequencyDescriptorRole persistent_root{operations,
                                                persistent_root_descriptor};
        const auto rollback_persistent = [&]() noexcept {
            auto live_domains =
                std::span{state->domains}.first(state->domain_count);
            for (auto domain = live_domains.rbegin();
                 domain != live_domains.rend(); ++domain) {
                if (!domain->active)
                    continue;
                domain->active = false;
                if (domain->attribute_descriptor >= 0) {
                    const auto attribute =
                        std::exchange(domain->attribute_descriptor, -1);
                    append_cleanup(
                        operations.close(operations.context, attribute));
                }
                if (domain->descriptor >= 0) {
                    const auto descriptor = std::exchange(domain->descriptor, -1);
                    append_cleanup(
                        operations.close(operations.context, descriptor));
                }
            }
            append_cleanup(persistent_root.retire());
        };
        struct stat persistent_root_identity{}, persistent_root_named{};
        errno = 0;
        if (operations.stat_fd(operations.context, persistent_root.get(),
                               &persistent_root_identity) != 0 ||
            operations.stat_at(operations.context, AT_FDCWD, cpufreq_root,
                               &persistent_root_named) != 0 ||
            bounded_sysfs_identity(persistent_root_identity) !=
                bounded_sysfs_identity(root_initial) ||
            bounded_sysfs_identity(persistent_root_named) !=
                bounded_sysfs_identity(root_named)) {
            const auto saved = errno == 0 ? ESTALE : errno;
            rollback_persistent();
            return std::unexpected{sequence(frequency_source_failure(
                BoundedCpuFrequencySourceFailureKind::enumerate, saved))};
        }
        for (auto& domain :
             std::span{state->domains}.first(state->domain_count)) {
            const auto descriptor = operations.open_at(
                operations.context, persistent_root.get(), domain.policy_name.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_DIRECTORY);
            if (descriptor < 0) {
                const auto saved = errno;
                rollback_persistent();
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumerate, saved))};
            }
            FrequencyDescriptorRole role{operations, descriptor};
            struct stat identity{}, named_identity{};
            errno = 0;
            if (operations.stat_fd(operations.context, descriptor, &identity) != 0 ||
                operations.stat_at(operations.context, persistent_root.get(),
                                   domain.policy_name.c_str(),
                                   &named_identity) != 0 ||
                !bounded_sysfs_same_identity(identity, named_identity) ||
                bounded_sysfs_identity(identity) != domain.identity) {
                const auto saved = errno == 0 ? ESTALE : errno;
                append_cleanup(role.retire());
                rollback_persistent();
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumerate, saved))};
            }
            domain.descriptor = role.release();
            domain.active = true;
            const auto attribute = operations.open_at(
                operations.context, domain.descriptor,
                domain.current_attribute.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (attribute < 0) {
                const auto saved = errno;
                rollback_persistent();
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumerate, saved))};
            }
            FrequencyDescriptorRole attribute_role{operations, attribute};
            struct stat attribute_identity{}, named_attribute_identity{};
            errno = 0;
            if (operations.stat_fd(operations.context, attribute,
                                   &attribute_identity) != 0 ||
                operations.stat_at(operations.context, domain.descriptor,
                                   domain.current_attribute.c_str(),
                                   &named_attribute_identity) != 0 ||
                !bounded_sysfs_same_identity(attribute_identity,
                                             named_attribute_identity) ||
                bounded_sysfs_identity(attribute_identity) !=
                    domain.attribute_identity) {
                const auto saved = errno == 0 ? ESTALE : errno;
                append_cleanup(attribute_role.retire());
                rollback_persistent();
                return std::unexpected{sequence(frequency_source_failure(
                    BoundedCpuFrequencySourceFailureKind::enumerate, saved))};
            }
            domain.attribute_descriptor = attribute_role.release();
        }
        state->root_descriptor = persistent_root.release();
        state->root_identity = bounded_sysfs_identity(persistent_root_identity);
        BoundedCpuFrequencySources source{std::move(final_state)};
        prepared.state_.reset();
        return std::optional<BoundedCpuFrequencySources>{std::move(source)};
    }
}

detail::FailedPreparedBoundedCpuFrequencyDiscovery::
    FailedPreparedBoundedCpuFrequencyDiscovery(
        PreparedBoundedCpuFrequencyDiscoveryStorage&& storage,
        BoundedCpuFrequencySourceFailureSequence sequence) noexcept
    : storage_{std::move(storage)}, sequence_{sequence} {}
detail::FailedPreparedBoundedCpuFrequencyDiscovery::
    FailedPreparedBoundedCpuFrequencyDiscovery(
        FailedPreparedBoundedCpuFrequencyDiscovery&& other) noexcept
    : storage_{std::move(other.storage_)}, sequence_{other.sequence_} {
    other.storage_.reset();
}
detail::FailedPreparedBoundedCpuFrequencyDiscovery::
    ~FailedPreparedBoundedCpuFrequencyDiscovery() noexcept = default;

auto detail::PreparedBoundedCpuFrequencyDiscoveryAccess::cleanup(
    const FailedPreparedBoundedCpuFrequencyDiscovery& failed) noexcept
    -> std::optional<std::span<const BoundedCpuFrequencySourceFailure>> {
    if (!failed.storage_ || !failed.storage_->state_ ||
        failed.sequence_.cleanup_count >
            failed.storage_->state_->cleanup_stock.size())
        return std::nullopt;
    return std::span<const BoundedCpuFrequencySourceFailure>{
        failed.storage_->state_->cleanup_stock.data(),
        failed.sequence_.cleanup_count};
}

auto detail::PreparedBoundedCpuFrequencyDiscoveryAccess::sequence(
    const FailedPreparedBoundedCpuFrequencyDiscovery& failed) noexcept
    -> std::optional<BoundedCpuFrequencySourceFailureSequence> {
    if (!failed.storage_ || !failed.storage_->state_)
        return std::nullopt;
    return failed.sequence_;
}

auto detail::BoundedCpuFrequencySources::discover_prepared(
    PreparedBoundedCpuFrequencyDiscoveryStorage&& prepared,
    BoundedCpuFrequencyValidationGate gate) noexcept
    -> std::expected<std::optional<BoundedCpuFrequencySources>,
                     FailedPreparedBoundedCpuFrequencyDiscovery> {
    auto result = discover_prepared_engine(prepared, gate);
    if (!result)
        return std::unexpected{FailedPreparedBoundedCpuFrequencyDiscovery{
            std::move(prepared), result.error()}};
    return std::move(*result);
}

auto detail::BoundedCpuFrequencySources::discover(
    BoundedCpuFrequencyCapacity capacity,
    std::span<const core::LinuxCpuId> present_cpus,
    std::span<const core::LinuxCpuId> worker_cpus,
    BoundedSysfsOperations operations,
    std::span<BoundedCpuFrequencySourceFailure> cleanup_stock,
    BoundedCpuFrequencyValidationGate gate) noexcept
    -> std::expected<std::optional<BoundedCpuFrequencySources>,
                     BoundedCpuFrequencySourceFailureSequence> {
    if (capacity.maximum_policies >
        (std::numeric_limits<std::size_t>::max() - 1U) / 2U)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::enumeration_capacity)};
    const auto cleanup_capacity =
        2U * static_cast<std::size_t>(capacity.maximum_policies) + 1U;
    if (cleanup_stock.size() < cleanup_capacity)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::enumeration_capacity)};
    auto prepared = PreparedBoundedCpuFrequencyDiscoveryStorage::prepare(
        capacity, present_cpus, worker_cpus, operations);
    if (!prepared)
        return std::unexpected{prepared.error()};
    auto result = discover_prepared(std::move(*prepared), gate);
    if (result)
        return std::move(*result);
    const auto sequence = PreparedBoundedCpuFrequencyDiscoveryAccess::sequence(
        result.error());
    const auto cleanup = PreparedBoundedCpuFrequencyDiscoveryAccess::cleanup(
        result.error());
    if (!sequence || !cleanup)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::contained)};
    std::copy(cleanup->begin(), cleanup->end(), cleanup_stock.begin());
    return std::unexpected{*sequence};
}

auto detail::BoundedCpuFrequencySources::domain_count() const noexcept
    -> std::size_t {
    return state_ ? state_->domain_count : 0U;
}
auto detail::BoundedCpuFrequencySources::related_cpus(std::size_t index) const noexcept
    -> std::span<const core::LinuxCpuId> {
    if (!state_ || index >= state_->domain_count)
        return {};
    const auto& domain = state_->domains[index];
    return std::span<const core::LinuxCpuId>{state_->membership_arena}.subspan(
        domain.membership_offset, domain.membership_count);
}
auto detail::BoundedCpuFrequencySources::kind(std::size_t index) const noexcept
    -> CpuFrequencyCurrentKind {
    return state_->domains[index].kind;
}
auto detail::BoundedCpuFrequencySources::driver(std::size_t index) const noexcept
    -> std::string_view {
    return state_->domains[index].driver;
}
auto detail::BoundedCpuFrequencySources::governor(std::size_t index) const noexcept
    -> std::string_view {
    return state_->domains[index].governor;
}
auto detail::BoundedCpuFrequencySources::minimum_hz(std::size_t index) const noexcept
    -> std::uint64_t {
    return state_->domains[index].minimum;
}
auto detail::BoundedCpuFrequencySources::maximum_hz(std::size_t index) const noexcept
    -> std::uint64_t {
    return state_->domains[index].maximum;
}
auto detail::BoundedCpuFrequencySources::read(
    std::size_t index, std::span<char> workspace,
    std::span<BoundedCpuFrequencySourceFailure> cleanup_stock) const noexcept
    -> std::expected<std::uint64_t, BoundedCpuFrequencySourceFailureSequence> {
    if (cleanup_stock.empty())
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::enumeration_capacity)};
    if (!state_ || index >= state_->domain_count ||
        workspace.size() <= state_->content_capacity)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::read, EINVAL)};
    auto& domain = state_->domains[index];
    if (!domain.active || domain.descriptor < 0)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::read, EINVAL)};
    struct stat root_identity{}, identity{}, named_policy{};
    errno = 0;
    if (state_->operations.stat_fd(state_->operations.context,
                                   state_->root_descriptor,
                                   &root_identity) != 0 ||
        bounded_sysfs_identity(root_identity) != state_->root_identity ||
        state_->operations.stat_fd(state_->operations.context,
                                   domain.descriptor, &identity) != 0 ||
        state_->operations.stat_at(state_->operations.context,
                                   state_->root_descriptor,
                                   domain.policy_name.c_str(),
                                   &named_policy) != 0 ||
        !bounded_sysfs_same_identity(identity, named_policy) ||
        bounded_sysfs_identity(identity) != domain.identity)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::read,
            errno == 0 ? ESTALE : errno)};
    struct stat attribute_identity{}, named_attribute_identity{};
    errno = 0;
    if (state_->operations.stat_fd(state_->operations.context,
                                   domain.attribute_descriptor,
                                   &attribute_identity) != 0 ||
        state_->operations.stat_at(state_->operations.context,
                                   domain.descriptor,
                                   domain.current_attribute.c_str(),
                                   &named_attribute_identity) != 0 ||
        !bounded_sysfs_same_identity(attribute_identity,
                                     named_attribute_identity) ||
        bounded_sysfs_identity(attribute_identity) != domain.attribute_identity)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::read,
            errno == 0 ? ESTALE : errno)};
    std::size_t used{};
    while (used < workspace.size()) {
        std::ptrdiff_t count{};
        do {
            count = state_->operations.read(
                state_->operations.context, domain.attribute_descriptor,
                workspace.data() + used, workspace.size() - used,
                static_cast<std::int64_t>(used));
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            return std::unexpected{frequency_source_failure(
                BoundedCpuFrequencySourceFailureKind::read, errno)};
        if (count == 0)
            break;
        used += static_cast<std::size_t>(count);
    }
    struct stat final_root{}, final_policy{}, final_named_policy{},
        final_attribute{}, final_named_attribute{};
    errno = 0;
    if (state_->operations.stat_fd(state_->operations.context,
                                   state_->root_descriptor,
                                   &final_root) != 0 ||
        bounded_sysfs_identity(final_root) != state_->root_identity ||
        state_->operations.stat_fd(state_->operations.context,
                                   domain.descriptor,
                                   &final_policy) != 0 ||
        state_->operations.stat_at(state_->operations.context,
                                   state_->root_descriptor,
                                   domain.policy_name.c_str(),
                                   &final_named_policy) != 0 ||
        !bounded_sysfs_same_identity(final_policy, final_named_policy) ||
        bounded_sysfs_identity(final_policy) != domain.identity ||
        state_->operations.stat_fd(state_->operations.context,
                                   domain.attribute_descriptor,
                                   &final_attribute) != 0 ||
        state_->operations.stat_at(state_->operations.context,
                                   domain.descriptor,
                                   domain.current_attribute.c_str(),
                                   &final_named_attribute) != 0 ||
        !bounded_sysfs_same_identity(final_attribute,
                                     final_named_attribute) ||
        bounded_sysfs_identity(final_attribute) != domain.attribute_identity)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::read,
            errno == 0 ? ESTALE : errno)};
    if (used > state_->content_capacity)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::read, EOVERFLOW)};
    const auto value = CpuFrequencyAccess::parse_khz_bounded(
        std::string_view{workspace.data(), used});
    if (!value)
        return std::unexpected{frequency_source_failure(
            BoundedCpuFrequencySourceFailureKind::value)};
    return *value;
}

auto detail::BoundedCpuFrequencySources::retire(
    std::span<BoundedSysfsDisposition> close_dispositions) noexcept -> std::size_t {
    if (!state_)
        return 0U;
    std::size_t count{};
    auto live_domains =
        std::span{state_->domains}.first(state_->domain_count);
    for (auto domain = live_domains.rbegin(); domain != live_domains.rend();
         ++domain) {
        if (!domain->active)
            continue;
        domain->active = false;
        if (domain->attribute_descriptor >= 0) {
            const auto attribute = std::exchange(domain->attribute_descriptor, -1);
            const auto result = state_->operations.close(state_->operations.context,
                                                         attribute);
            if (count < close_dispositions.size())
                close_dispositions[count] = {
                    result.kind, result.native_error, true};
            ++count;
        }
        const auto descriptor = std::exchange(domain->descriptor, -1);
        const auto result = state_->operations.close(state_->operations.context,
                                                     descriptor);
        if (count < close_dispositions.size())
            close_dispositions[count] = {
                result.kind, result.native_error, true};
        ++count;
    }
    if (state_->root_descriptor >= 0) {
        const auto root = std::exchange(state_->root_descriptor, -1);
        const auto result = state_->operations.close(state_->operations.context,
                                                     root);
        if (count < close_dispositions.size())
            close_dispositions[count] = {
                result.kind, result.native_error, true};
        ++count;
    }
    return count;
}

auto detail::CpuFrequencyAccess::discover(core::LinuxCpuId cpu,
                                          std::span<const CpuFrequencyFile> files, void* context,
                                          ReadOperation read)
    -> std::expected<std::optional<CpuFrequencyPolicySource>, core::Error> {
    std::vector<std::string> policies;
    constexpr std::string_view membership_suffix{"/related_cpus"};
    for (const auto& file : files) {
        if (!file.path.ends_with(membership_suffix)) {
            continue;
        }
        auto cpus = parse_cpu_list(file.contents);
        if (!cpus) {
            return std::unexpected{
                frequency_error("linux.cpu_frequency.invalid_value", "membership")};
        }
        if (cpus->contains(cpu)) {
            policies.push_back(file.path.substr(0, file.path.size() - membership_suffix.size()));
        }
    }
    std::ranges::sort(policies);
    policies.erase(std::unique(policies.begin(), policies.end()), policies.end());
    std::vector<CpuFrequencyPolicyView> views;
    views.reserve(policies.size());
    for (std::size_t index{}; index < policies.size(); ++index) {
        const auto& policy = policies[index];
        const auto* membership = file_contents(files, policy + "/related_cpus");
        const auto* driver = file_contents(files, policy + "/scaling_driver");
        const auto* governor = file_contents(files, policy + "/scaling_governor");
        const auto* minimum = file_contents(files, policy + "/scaling_min_freq");
        const auto* maximum = file_contents(files, policy + "/scaling_max_freq");
        if (!membership || !driver || !governor || !minimum || !maximum)
            return std::unexpected{
                frequency_error("linux.cpu_frequency.discovery_failed", "metadata")};
        const auto* hardware = file_contents(files, policy + "/cpuinfo_cur_freq");
        const auto* scaling = file_contents(files, policy + "/scaling_cur_freq");
        views.push_back({static_cast<std::uint32_t>(index), *membership, *driver, *governor,
                         *minimum, *maximum,
                         hardware ? std::optional<std::string_view>{*hardware} : std::nullopt,
                         scaling ? std::optional<std::string_view>{*scaling} : std::nullopt});
    }
    auto projected = project(cpu, views);
    if (!projected)
        return std::unexpected{std::move(projected.error())};
    if (!projected->value)
        return std::optional<CpuFrequencyPolicySource>{};
    auto value = std::move(*projected->value);
    const auto& prefix = policies[value.locator];
    const auto selected =
        prefix + (value.kind == CpuFrequencyCurrentKind::hardware_current ? "/cpuinfo_cur_freq"
                                                                          : "/scaling_cur_freq");
    return std::optional<CpuFrequencyPolicySource>{CpuFrequencyPolicySource{
        std::move(value.related_cpus), value.kind, std::move(value.driver),
        std::move(value.governor), value.minimum_hz, value.maximum_hz, selected, context, read}};
}

auto CpuFrequencyPolicySource::discover(core::LinuxCpuId cpu)
    -> std::expected<std::optional<CpuFrequencyPolicySource>, core::Error> {
    std::vector<detail::CpuFrequencyFile> files;
    try {
        const std::filesystem::path root{"/sys/devices/system/cpu/cpufreq"};
        for (const auto& policy : std::filesystem::directory_iterator(root)) {
            if (!policy.is_directory()) {
                continue;
            }
            for (const auto* name : {"related_cpus", "scaling_driver", "scaling_governor",
                                     "scaling_min_freq", "scaling_max_freq"}) {
                auto value = read_text_file(policy.path() / name);
                if (!value) {
                    return std::unexpected{
                        frequency_error("linux.cpu_frequency.discovery_failed", "enumerate")};
                }
                files.push_back({(policy.path() / name).string(), std::move(*value)});
            }
            for (const auto* name : {"cpuinfo_cur_freq", "scaling_cur_freq"}) {
                auto value = read_optional_text_file(policy.path() / name);
                if (!value) {
                    return std::unexpected{
                        frequency_error("linux.cpu_frequency.discovery_failed", "enumerate")};
                }
                if (*value) {
                    files.push_back({(policy.path() / name).string(), std::move(**value)});
                }
            }
        }
    } catch (...) {
        return std::unexpected{
            frequency_error("linux.cpu_frequency.discovery_failed", "enumerate")};
    }
    return detail::CpuFrequencyAccess::discover(cpu, files, nullptr, production_read);
}

auto CpuFrequencyPolicySource::read() const noexcept -> std::expected<std::uint64_t, core::Error> {
    auto text = read_(context_, current_path_);
    if (!text) {
        return std::unexpected{std::move(text.error())};
    }
    return detail::CpuFrequencyAccess::parse_khz(*text);
}

auto CpuFrequencyPolicySource::related_cpus() const noexcept -> std::span<const core::LinuxCpuId> {
    return related_cpus_;
}
auto CpuFrequencyPolicySource::kind() const noexcept -> CpuFrequencyCurrentKind { return kind_; }
auto CpuFrequencyPolicySource::scaling_driver() const noexcept -> const std::string& {
    return driver_;
}
auto CpuFrequencyPolicySource::scaling_governor() const noexcept -> const std::string& {
    return governor_;
}
auto CpuFrequencyPolicySource::scaling_minimum_hz() const noexcept -> std::uint64_t {
    return minimum_hz_;
}
auto CpuFrequencyPolicySource::scaling_maximum_hz() const noexcept -> std::uint64_t {
    return maximum_hz_;
}

} // namespace epycbench::platform_linux
