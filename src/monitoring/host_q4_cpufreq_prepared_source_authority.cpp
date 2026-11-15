#include "monitoring/host_q4_cpufreq_prepared_source_authority.hpp"

#include "monitoring/frequency_source_access.hpp"
#include "platform_linux/cpu_frequency_access.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Reason = MonitoringQ4CapacityReason;
constexpr std::array<std::string_view, 7> names{
    "related_cpus",     "scaling_driver",   "scaling_governor", "scaling_min_freq",
    "scaling_max_freq", "cpuinfo_cur_freq", "scaling_cur_freq"};

auto same(const MonitoringQ4CpuFreqIdentity& left, const struct stat& right) noexcept -> bool {
    return left.device == static_cast<std::uint64_t>(right.st_dev) &&
           left.inode == static_cast<std::uint64_t>(right.st_ino) &&
           left.owner == static_cast<std::uint64_t>(right.st_uid) &&
           left.mode == static_cast<std::uint64_t>(right.st_mode) &&
           left.links == static_cast<std::uint64_t>(right.st_nlink);
}
auto same(const struct stat& left, const struct stat& right) noexcept -> bool {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_uid == right.st_uid && left.st_mode == right.st_mode &&
           left.st_nlink == right.st_nlink;
}
auto bounded_trim(std::string_view value) noexcept -> std::string_view {
    const auto whitespace = [](char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
    while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
    return value;
}
auto oracle_khz(std::string_view input) noexcept -> std::optional<std::uint64_t> {
    const auto value = bounded_trim(input);
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed == 0 || parsed > std::numeric_limits<std::uint64_t>::max() / 1000U)
        return std::nullopt;
    return parsed * 1000U;
}
auto callback_error(std::string_view code, std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Linux CPU frequency operation failed",
            .operation = "cpu_frequency",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
void observe_close(auto& operations,
                   MonitoringQ4PreparedCleanupRole role, std::uint64_t policy,
                   std::uint64_t attribute, int result) noexcept {
    if (operations.observe_cleanup)
        operations.observe_cleanup(operations.cleanup_context,
                                    4U + static_cast<unsigned>(role), policy, attribute,
                                    result == 0 ? 0 : errno, result != 0);
}
} // namespace

struct MonitoringQ4CpuFreqPreparedSourceAuthority::State final {
    MonitoringQ4CpuFreqInventoryAuthority::Operations operations{};
    int policy{-1};
    MonitoringQ4CpuFreqIdentity identity{};
    std::uint32_t private_locator{};
    std::uint64_t policy_ordinal{};
    std::uint64_t attribute_ordinal{};
    std::array<char, 20> attribute{};
    std::vector<char> bytes;
    std::optional<core::Error> typed_read_error;
    std::optional<core::Error> typed_invalid_error;
    std::optional<MonitoringQ4PreparedSourceFailure> typed_failure;
    bool typed_failed{};
    bool force_disagreement{};
    bool reject_production{};
    bool reject_oracle{};
    std::uint64_t production_parser_calls{};
    std::uint64_t oracle_parser_calls{};
    bool terminal{};
    bool cleanup_uncertain{};
    ~State() noexcept {
        if (policy >= 0 && operations.close) {
            const int descriptor = std::exchange(policy, -1);
            const int result = operations.close(operations.context, descriptor);
            observe_close(operations, MonitoringQ4PreparedCleanupRole::retained_policy,
                          policy_ordinal, 0, result);
            if (result != 0)
                cleanup_uncertain = true;
        }
    }
};

struct MonitoringQ4CpuFreqPreparedSourceAuthority::Selection final {
    MonitoringQ4CpuFreqInventoryAuthority::Operations operations{};
    core::LinuxCpuId cpu{0};
    platform_linux::detail::CpuFrequencyBoundedScratch scratch;
    std::vector<core::LinuxCpuId> cpus;
    std::string driver;
    std::string governor;
    int selected{-1};
    MonitoringQ4CpuFreqIdentity identity{};
    monitoring::FrequencySourceKind kind{};
    std::uint64_t minimum{};
    std::uint64_t maximum{};
    std::uint32_t private_locator{};
    std::uint64_t policy_ordinal{};
    unsigned matches{};
    MonitoringQ4PreparedSourceFailureKind callback_failure{};
    int native{};
    int cleanup_native{};
    bool cleanup_uncertain{};
};

MonitoringQ4CpuFreqPreparedSourceAuthority::MonitoringQ4CpuFreqPreparedSourceAuthority(
    std::unique_ptr<State> state, monitoring::FrequencySource source) noexcept
    : state_{std::move(state)}, source_{std::move(source)} {}

MonitoringQ4CpuFreqPreparedSourceAuthority::MonitoringQ4CpuFreqPreparedSourceAuthority(
    MonitoringQ4CpuFreqPreparedSourceAuthority&&) noexcept = default;

MonitoringQ4CpuFreqPreparedSourceAuthority::~MonitoringQ4CpuFreqPreparedSourceAuthority() noexcept {
    if (state_ && state_->policy >= 0) {
        const int descriptor = std::exchange(state_->policy, -1);
        const int result = state_->operations.close(state_->operations.context, descriptor);
        observe_close(state_->operations, MonitoringQ4PreparedCleanupRole::retained_policy,
                      state_->policy_ordinal, 0, result);
        if (result != 0)
            state_->cleanup_uncertain = true;
    }
}

MonitoringQ4CpuFreqPreparedSourceAuthority::Workspace::Workspace(
    core::LinuxCpuId cpu, std::unique_ptr<Selection> selection, std::unique_ptr<State> state,
    MonitoringQ4CpuFreqSemanticWorkspace semantic,
    MonitoringQ4CpuFreqInventoryAuthority::Workspace inventory) noexcept
    : cpu_{cpu}, selection_{std::move(selection)}, state_{std::move(state)},
      semantic_{std::move(semantic)}, inventory_{std::move(inventory)} {}
MonitoringQ4CpuFreqPreparedSourceAuthority::Workspace::Workspace(Workspace&&) noexcept = default;
MonitoringQ4CpuFreqPreparedSourceAuthority::Workspace::~Workspace() noexcept = default;

auto MonitoringQ4CpuFreqPreparedSourceAuthority::select_policy(
    void* raw, int policy, std::uint64_t policy_ordinal,
    const MonitoringQ4CpuFreqPolicyInventory& stored,
    std::span<const char> contents) noexcept -> int {
    auto& selection = *static_cast<Selection*>(raw);
    const auto bytes = [&](std::size_t index) noexcept -> std::optional<std::string_view> {
        const auto& value = stored.attributes[index];
        if (!value.present || value.hardware_unreadable)
            return std::nullopt;
        if (value.offset > contents.size() || value.size > contents.size() - value.offset)
            return std::nullopt;
        return std::string_view{contents.data() + value.offset,
                                static_cast<std::size_t>(value.size)};
    };
    const auto related = bytes(0), driver = bytes(1), governor = bytes(2), minimum = bytes(3),
               maximum = bytes(4);
    if (!related || !driver || !governor || !minimum || !maximum) {
        selection.callback_failure = MonitoringQ4PreparedSourceFailureKind::production_semantics;
        errno = EINVAL;
        return -1;
    }
    const platform_linux::detail::CpuFrequencyPolicyView view{
        stored.policy_number, *related, *driver, *governor, *minimum, *maximum, bytes(5), bytes(6)};
    auto projected = platform_linux::detail::CpuFrequencyAccess::project_bounded(
        selection.cpu, std::span{&view, 1U}, selection.scratch);
    if (!projected) {
        selection.callback_failure = MonitoringQ4PreparedSourceFailureKind::production_semantics;
        errno = EINVAL;
        return -1;
    }
    if (!projected->value && projected->unsupported ==
                                 platform_linux::detail::CpuFrequencyUnsupportedReason::
                                     no_resolvable_policy)
        return 0;
    if (!projected->value && stored.attributes[5].hardware_unreadable) {
        selection.callback_failure = MonitoringQ4PreparedSourceFailureKind::open_attribute;
        selection.native = EACCES;
        errno = EACCES;
        return -1;
    }
    ++selection.matches;
    if (!projected->value)
        return 0;
    if (selection.selected >= 0)
        return 0;
    const auto& value = *projected->value;
    selection.cpus.assign(value.related_cpus.begin(), value.related_cpus.end());
    selection.driver.assign(value.driver);
    selection.governor.assign(value.governor);
    selection.kind = value.kind == platform_linux::CpuFrequencyCurrentKind::hardware_current
                         ? monitoring::FrequencySourceKind::hardware_current
                         : monitoring::FrequencySourceKind::scaling_current;
    selection.minimum = value.minimum_hz;
    selection.maximum = value.maximum_hz;
    selection.private_locator = value.locator;
    selection.policy_ordinal = policy_ordinal;
    selection.identity = stored.identity;
    if (!selection.operations.duplicate) {
        selection.callback_failure = MonitoringQ4PreparedSourceFailureKind::contained;
        errno = ENOSYS;
        return -1;
    }
    selection.selected = selection.operations.duplicate(selection.operations.context, policy);
    if (selection.selected < 0) {
        selection.callback_failure = MonitoringQ4PreparedSourceFailureKind::open_attribute;
        selection.native = errno;
        return -1;
    }
    return 0;
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::prepare_cpu(const MonitoringQ4SourceReady& ready,
                                                              core::LinuxCpuId cpu) noexcept
    -> std::expected<Preparation, MonitoringQ4PreparedSourceFailure> {
    return prepare_with(ready, cpu, MonitoringQ4CpuFreqInventoryAuthority::native_operations());
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::prepare_with(
    const MonitoringQ4SourceReady& ready, core::LinuxCpuId cpu,
    MonitoringQ4CpuFreqInventoryAuthority::Operations operations) noexcept
    -> std::expected<Preparation, MonitoringQ4PreparedSourceFailure> {
    auto workspace = build_workspace(ready, cpu, operations);
    if (!workspace) return std::unexpected{std::move(workspace.error())};
    return prepare_with_workspace(ready, std::move(*workspace));
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::build_workspace(
    const MonitoringQ4SourceReady& ready, core::LinuxCpuId cpu,
    MonitoringQ4CpuFreqInventoryAuthority::Operations operations) noexcept
    -> std::expected<Workspace, MonitoringQ4PreparedSourceFailure> {
    if (!std::ranges::binary_search(ready.inputs().admitted_cpus(), cpu))
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::cpu_not_admitted}};
    auto allocation_reason = Reason::source_selection_allocation;
    try {
        unsigned allocation_stage = 100U;
        const auto checkpoint = [&](Reason reason) {
            allocation_reason = reason;
            if (operations.fault) operations.fault(operations.context, allocation_stage);
            ++allocation_stage;
        };
        checkpoint(Reason::source_selection_allocation);
        auto selection = std::make_unique<Selection>();
        checkpoint(Reason::source_state_allocation);
        auto state = std::make_unique<State>();
        const auto n = static_cast<std::size_t>(ready.inputs().structural()
                                                    .maximum_membership_entries().value);
        const auto b = static_cast<std::size_t>(ready.inputs().structural()
                                                    .per_file_probe_bytes().value);
        selection->operations = operations;
        selection->cpu = cpu;
        allocation_reason = Reason::source_parsed_allocation;
        selection->scratch.parsed.reserve(n);
        checkpoint(Reason::source_selected_allocation);
        selection->scratch.selected.reserve(n);
        checkpoint(Reason::source_cpus_allocation);
        selection->cpus.reserve(n);
        checkpoint(Reason::source_driver_allocation);
        selection->driver.reserve(b);
        checkpoint(Reason::source_governor_allocation);
        selection->governor.reserve(b);
        state->operations = operations;
        checkpoint(Reason::source_bytes_allocation);
        state->bytes.resize(b);
        checkpoint(Reason::source_read_error_allocation);
        state->typed_read_error.emplace(
            callback_error("linux.cpu_frequency.read_failed", "read"));
        checkpoint(Reason::source_invalid_error_allocation);
        state->typed_invalid_error.emplace(
            callback_error("linux.cpu_frequency.invalid_value", "value"));
        checkpoint(Reason::source_failure_slot_allocation);
        state->typed_failure.emplace(MonitoringQ4PreparedSourceFailureKind::inactive);

        checkpoint(Reason::source_semantic_call_allocation);
        auto semantic_workspace = MonitoringQ4CpuFreqSemanticWorkspace::prepare(ready);
        if (!semantic_workspace)
            return std::unexpected{MonitoringQ4PreparedSourceFailure{
                MonitoringQ4PreparedSourceFailureKind::semantic_agreement, 0, false,
                std::nullopt, 0, semantic_workspace.error()}};
        auto inventory = MonitoringQ4CpuFreqInventoryAuthority::prepare_workspace(
            ready.inputs(), operations);
        if (!inventory)
            return std::unexpected{MonitoringQ4PreparedSourceFailure{
                MonitoringQ4PreparedSourceFailureKind::inventory, 0, false,
                std::move(inventory.error())}};
        return Workspace{cpu, std::move(selection), std::move(state),
                         std::move(*semantic_workspace), std::move(*inventory)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::allocation, 0, false, {}, 0, {}, allocation_reason}};
    } catch (...) {
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::contained}};
    }
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::prepare_with_workspace(
    const MonitoringQ4SourceReady& ready, Workspace&& workspace) noexcept
    -> std::expected<Preparation, MonitoringQ4PreparedSourceFailure> {
    auto& selection = *workspace.selection_;
    auto state = std::move(workspace.state_);
    auto operations = selection.operations;
    const auto close_selected = [&](int& descriptor, MonitoringQ4PreparedCleanupRole role,
                                    std::uint64_t policy) noexcept {
        const int result = operations.close(operations.context, std::exchange(descriptor, -1));
        observe_close(operations, role, policy, 0, result);
        return result;
    };
    try {
        auto inventory = MonitoringQ4CpuFreqInventoryAuthority::retain_prepared(
            ready.inputs(), operations, std::move(*workspace.inventory_));
        if (!inventory)
            return std::unexpected{MonitoringQ4PreparedSourceFailure{
                MonitoringQ4PreparedSourceFailureKind::inventory, 0, false,
                std::move(inventory.error())}};
        MonitoringQ4CpuFreqInventoryAuthority::bind_ready(*inventory, ready);
        auto captured = inventory->capture_with(&selection, select_policy);
        if (!captured) {
            bool cleanup_uncertain{};
            if (selection.selected >= 0 && close_selected(
                    selection.selected, MonitoringQ4PreparedCleanupRole::selected_policy,
                    selection.policy_ordinal) != 0)
                cleanup_uncertain = true, selection.cleanup_native = errno;
            if (captured.error().primary ==
                MonitoringQ4CpuFreqInventoryFailureKind::policy_callback)
                {
                const bool inventory_uncertain = captured.error().cleanup_overflowed ||
                    std::ranges::any_of(captured.error().cleanup,
                                        [](const auto& cleanup) { return cleanup.uncertain; });
                MonitoringQ4PreparedSourceFailure failure{
                    selection.callback_failure,
                    selection.callback_failure ==
                            MonitoringQ4PreparedSourceFailureKind::ambiguous_policy
                        ? 0
                        : (selection.native ? selection.native
                                            : captured.error().native_error),
                    cleanup_uncertain || selection.cleanup_uncertain || inventory_uncertain,
                    std::move(captured.error()), selection.cleanup_native};
                if (cleanup_uncertain)
                    failure.append_cleanup({MonitoringQ4PreparedCleanupRole::selected_policy,
                                            selection.policy_ordinal, 0,
                                            failure.cleanup_native_error, true});
                return std::unexpected{std::move(failure)};
                }
            return std::unexpected{MonitoringQ4PreparedSourceFailure{
                MonitoringQ4PreparedSourceFailureKind::inventory, 0, cleanup_uncertain,
                std::move(captured.error())}};
        }
        if (operations.fault) operations.fault(operations.context, 200U);
        auto semantic = MonitoringQ4CpuFreqSemanticAgreement::evaluate(
            std::move(*workspace.semantic_), *captured);
        if (!semantic) {
            int close_native{};
            if (selection.selected >= 0 && close_selected(
                    selection.selected, MonitoringQ4PreparedCleanupRole::selected_policy,
                    selection.policy_ordinal) != 0)
                close_native = errno;
            const auto kind = semantic.error().kind ==
                                      MonitoringQ4CpuFreqSemanticFailureKind::overlap
                                  ? MonitoringQ4PreparedSourceFailureKind::semantic_overlap
                              : semantic.error().kind ==
                                        MonitoringQ4CpuFreqSemanticFailureKind::conflict
                                  ? MonitoringQ4PreparedSourceFailureKind::semantic_conflict
                                  : MonitoringQ4PreparedSourceFailureKind::semantic_agreement;
            MonitoringQ4PreparedSourceFailure failure{
                kind, 0, close_native != 0, std::nullopt, close_native, semantic.error()};
            if (close_native != 0)
                failure.append_cleanup({MonitoringQ4PreparedCleanupRole::selected_policy,
                                        selection.policy_ordinal, 0, close_native, true});
            return std::unexpected{std::move(failure)};
        }
        const auto outcome = std::ranges::find(semantic->cpus(), workspace.cpu_,
                                               &MonitoringQ4CpuFreqCpuOutcome::cpu);
        if (outcome == semantic->cpus().end())
            return std::unexpected{MonitoringQ4PreparedSourceFailure{
                MonitoringQ4PreparedSourceFailureKind::semantic_agreement}};
        if (!outcome->domain_ordinal && outcome->unsupported ==
                                                MonitoringQ4CpuFreqUnsupportedReason::
                                                    no_resolvable_policy)
            return Preparation{std::nullopt, MonitoringQ4PreparedSourceUnsupported{
                                                 MonitoringQ4CpuFreqUnsupportedReason::
                                                     no_resolvable_policy}};
        if (!outcome->domain_ordinal && outcome->unsupported ==
                                                MonitoringQ4CpuFreqUnsupportedReason::
                                                    no_current_attribute)
            return Preparation{std::nullopt, MonitoringQ4PreparedSourceUnsupported{
                                                 MonitoringQ4CpuFreqUnsupportedReason::
                                                     no_current_attribute}};
        if (!outcome->domain_ordinal || *outcome->domain_ordinal == 0 ||
            *outcome->domain_ordinal > semantic->domains().size() || selection.selected < 0)
            return std::unexpected{MonitoringQ4PreparedSourceFailure{
                MonitoringQ4PreparedSourceFailureKind::semantic_agreement}};
        const auto& accepted_domain =
            semantic->domains()[static_cast<std::size_t>(*outcome->domain_ordinal - 1U)];
        selection.cpus.assign(accepted_domain.cpus.begin(), accepted_domain.cpus.end());
        selection.driver.assign(accepted_domain.driver);
        selection.governor.assign(accepted_domain.governor);
        selection.kind = accepted_domain.source_kind;
        selection.minimum = accepted_domain.minimum_hz;
        selection.maximum = accepted_domain.maximum_hz;
        selection.private_locator = accepted_domain.private_policy_locator;
        state->policy = std::exchange(selection.selected, -1);
        state->identity = selection.identity;
        state->private_locator = selection.private_locator;
        state->policy_ordinal = selection.policy_ordinal;
        const auto selected_name = selection.kind == monitoring::FrequencySourceKind::hardware_current
                                       ? names[5]
                                       : names[6];
        std::copy(selected_name.begin(), selected_name.end(), state->attribute.begin());
        state->attribute[selected_name.size()] = '\0';
        state->attribute_ordinal = selection.kind ==
                                           monitoring::FrequencySourceKind::hardware_current
                                       ? 6U
                                       : 7U;
        if (operations.fault) operations.fault(operations.context, 201U);
        auto domain = monitoring::FrequencyDomainId::create(std::move(selection.cpus));
        if (!domain) {
            const int descriptor = std::exchange(state->policy, -1);
            const int close_result = operations.close(operations.context, descriptor);
            observe_close(operations, MonitoringQ4PreparedCleanupRole::retained_policy,
                          state->policy_ordinal, 0, close_result);
            const bool uncertain = close_result != 0;
            const int close_native = uncertain ? errno : 0;
            MonitoringQ4PreparedSourceFailure failure{
                MonitoringQ4PreparedSourceFailureKind::production_semantics, 0, uncertain};
            if (uncertain)
                failure.append_cleanup({MonitoringQ4PreparedCleanupRole::retained_policy,
                                        state->policy_ordinal, 0, close_native, true});
            return std::unexpected{std::move(failure)};
        }
        auto metadata = monitoring::FrequencyPolicyMetadata::create(
            std::move(*domain), selection.kind,
            monitoring::FrequencyDriverId::create(std::move(selection.driver)),
            monitoring::FrequencyGovernorId::create(std::move(selection.governor)),
            monitoring::FrequencyHz::from_hz(selection.minimum),
            monitoring::FrequencyHz::from_hz(selection.maximum));
        auto source = monitoring::detail::FrequencySourceAccess::create(
            std::move(metadata), state.get(), typed_callback);
        return Preparation{MonitoringQ4CpuFreqPreparedSourceAuthority{
                               std::move(state), std::move(source)},
                           std::nullopt};
    } catch (const std::bad_alloc&) {
        MonitoringQ4PreparedSourceFailure failure{
            MonitoringQ4PreparedSourceFailureKind::allocation, 0, false, {}, 0, {}, Reason::source_prepare_allocation};
        if (selection.selected >= 0 && close_selected(
                selection.selected, MonitoringQ4PreparedCleanupRole::selected_policy,
                selection.policy_ordinal) != 0)
            failure.append_cleanup({MonitoringQ4PreparedCleanupRole::selected_policy,
                                    selection.policy_ordinal, 0, errno, true});
        if (state && state->policy >= 0 && close_selected(
                state->policy, MonitoringQ4PreparedCleanupRole::retained_policy,
                state->policy_ordinal) != 0)
            failure.append_cleanup({MonitoringQ4PreparedCleanupRole::retained_policy,
                                    state->policy_ordinal, 0, errno, true});
        return std::unexpected{std::move(failure)};
    } catch (...) {
        MonitoringQ4PreparedSourceFailure failure{
            MonitoringQ4PreparedSourceFailureKind::contained};
        if (selection.selected >= 0 && close_selected(
                selection.selected, MonitoringQ4PreparedCleanupRole::selected_policy,
                selection.policy_ordinal) != 0)
            failure.append_cleanup({MonitoringQ4PreparedCleanupRole::selected_policy,
                                    selection.policy_ordinal, 0, errno, true});
        if (state && state->policy >= 0 && close_selected(
                state->policy, MonitoringQ4PreparedCleanupRole::retained_policy,
                state->policy_ordinal) != 0)
            failure.append_cleanup({MonitoringQ4PreparedCleanupRole::retained_policy,
                                    state->policy_ordinal, 0, errno, true});
        return std::unexpected{std::move(failure)};
    }
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::acquire(bool agreement) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4PreparedSourceFailure> {
    if (!state_ || state_->terminal || state_->policy < 0)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::inactive}};
    return acquire_state(*state_, agreement);
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::acquire_state(State& state,
                                                                bool agreement) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4PreparedSourceFailure> {
    struct stat policy_before{};
    if (state.operations.stat_fd(state.operations.context, state.policy, &policy_before) != 0)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::held_stat, errno}};
    if (!same(state.identity, policy_before))
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::identity}};
    int descriptor = state.operations.open_at(
        state.operations.context, state.policy, state.attribute.data(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::open_attribute, errno}};
    auto finish = [&](MonitoringQ4PreparedSourceFailure failure)
        -> std::expected<std::uint64_t, MonitoringQ4PreparedSourceFailure> {
        const int owned = std::exchange(descriptor, -1);
        const int result = state.operations.close(state.operations.context, owned);
        observe_close(state.operations, MonitoringQ4PreparedCleanupRole::fresh_attribute,
                      state.policy_ordinal, state.attribute_ordinal, result);
        if (result != 0)
            failure.append_cleanup({MonitoringQ4PreparedCleanupRole::fresh_attribute,
                                    state.policy_ordinal, state.attribute_ordinal, errno, true});
        return std::unexpected{failure};
    };
    struct stat held{}, named{};
    if (state.operations.stat_fd(state.operations.context, descriptor, &held) != 0)
        return finish({MonitoringQ4PreparedSourceFailureKind::held_stat, errno});
    if (state.operations.stat_at(state.operations.context, state.policy, state.attribute.data(),
                                 &named) != 0)
        return finish({MonitoringQ4PreparedSourceFailureKind::named_stat, errno});
    if (!S_ISREG(held.st_mode) || held.st_nlink != 1 || !same(held, named))
        return finish({MonitoringQ4PreparedSourceFailureKind::identity});
    std::size_t used{};
    const auto limit = state.bytes.size() - 1U;
    while (used < limit) {
        const auto count = state.operations.read(state.operations.context, descriptor,
                                                 state.bytes.data() + used, limit - used,
                                                 static_cast<std::int64_t>(used));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0)
            return finish({MonitoringQ4PreparedSourceFailureKind::read, errno});
        if (count == 0) break;
        if (static_cast<std::size_t>(count) > limit - used)
            return finish({MonitoringQ4PreparedSourceFailureKind::read_contract});
        used += static_cast<std::size_t>(count);
    }
    char probe{};
    for (;;) {
        const auto count = state.operations.read(state.operations.context, descriptor, &probe, 1,
                                                 static_cast<std::int64_t>(used));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0)
            return finish({MonitoringQ4PreparedSourceFailureKind::read, errno});
        if (count > 1)
            return finish({MonitoringQ4PreparedSourceFailureKind::read_contract});
        if (count != 0)
            return finish({MonitoringQ4PreparedSourceFailureKind::byte_limit, 0, false, {}, 0, {}, Reason::source_current_bytes});
        break;
    }
    struct stat after{}, named_after{}, policy_after{};
    if (state.operations.stat_fd(state.operations.context, descriptor, &after) != 0)
        return finish({MonitoringQ4PreparedSourceFailureKind::held_stat, errno});
    if (state.operations.stat_at(state.operations.context, state.policy, state.attribute.data(),
                                 &named_after) != 0)
        return finish({MonitoringQ4PreparedSourceFailureKind::named_stat, errno});
    if (!same(held, after) || !same(held, named_after))
        return finish({MonitoringQ4PreparedSourceFailureKind::identity});
    const int owned_attribute = std::exchange(descriptor, -1);
    const int close_result = state.operations.close(state.operations.context, owned_attribute);
    observe_close(state.operations, MonitoringQ4PreparedCleanupRole::fresh_attribute,
                  state.policy_ordinal, state.attribute_ordinal, close_result);
    if (close_result != 0) {
        const int native = errno;
        MonitoringQ4PreparedSourceFailure failure{
            MonitoringQ4PreparedSourceFailureKind::close, native, true};
        failure.append_cleanup({MonitoringQ4PreparedCleanupRole::fresh_attribute,
                                state.policy_ordinal, state.attribute_ordinal, native, true});
        return std::unexpected{failure};
    }
    if (state.operations.stat_fd(state.operations.context, state.policy, &policy_after) != 0)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::held_stat, errno}};
    if (!same(state.identity, policy_after))
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::identity}};
    const std::string_view text{state.bytes.data(), used};
    std::optional<std::uint64_t> oracle;
    if (agreement) {
        ++state.oracle_parser_calls;
        oracle = oracle_khz(text);
        if (state.reject_oracle) oracle.reset();
    }
    ++state.production_parser_calls;
    std::optional<std::uint64_t> production;
    try {
        auto parsed = platform_linux::detail::CpuFrequencyAccess::parse_khz(text);
        if (parsed) production = *parsed;
        if (state.reject_production) production.reset();
    } catch (...) {
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::contained}};
    }
    if (agreement && !oracle)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::oracle_parser}};
    if (!production)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::production_parser}};
    if (!agreement)
        return *production;
    if (*oracle != *production)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::parser_disagreement}};
    if (state.force_disagreement)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::parser_disagreement}};
    return *production;
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::raw_agreement() noexcept
    -> std::expected<std::uint64_t, MonitoringQ4PreparedSourceFailure> {
    return acquire(true);
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::typed_callback(void* raw) noexcept
    -> std::expected<std::uint64_t, core::Error> {
    auto value = acquire_state(*static_cast<State*>(raw), false);
    if (!value) {
        auto& state = *static_cast<State*>(raw);
        *state.typed_failure = std::move(value.error());
        state.typed_failed = true;
        const bool invalid = state.typed_failure->kind ==
                                 MonitoringQ4PreparedSourceFailureKind::production_parser ||
                             state.typed_failure->kind ==
                                 MonitoringQ4PreparedSourceFailureKind::oracle_parser ||
                             state.typed_failure->kind ==
                                 MonitoringQ4PreparedSourceFailureKind::parser_disagreement;
        auto& error = invalid ? state.typed_invalid_error : state.typed_read_error;
        return std::unexpected{std::move(*error)};
    }
    return *value;
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::typed_read() noexcept
    -> std::expected<std::uint64_t, MonitoringQ4PreparedSourceFailure> {
    if (!state_ || state_->terminal || state_->policy < 0)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::inactive}};
    if (state_->typed_failed)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::inactive}};
    auto value = monitoring::detail::FrequencySourceAccess::read_value(source_);
    if (!value)
        return std::unexpected{std::move(*state_->typed_failure)};
    return *value;
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::metadata() const noexcept
    -> const monitoring::FrequencyPolicyMetadata& { return source_.metadata(); }

void MonitoringQ4CpuFreqPreparedSourceAuthority::force_oracle_disagreement() noexcept {
    if (state_) state_->force_disagreement = true;
}

void MonitoringQ4CpuFreqPreparedSourceAuthority::force_parser_rejection(
    bool production, bool oracle) noexcept {
    if (state_) {
        state_->reject_production = production;
        state_->reject_oracle = oracle;
    }
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::parser_calls() const noexcept
    -> std::array<std::uint64_t, 2> {
    if (!state_) return {};
    return {state_->production_parser_calls, state_->oracle_parser_calls};
}

auto MonitoringQ4CpuFreqPreparedSourceAuthority::retire() noexcept
    -> std::expected<void, MonitoringQ4PreparedSourceFailure> {
    if (!state_ || state_->terminal || state_->policy < 0)
        return std::unexpected{MonitoringQ4PreparedSourceFailure{
            MonitoringQ4PreparedSourceFailureKind::inactive}};
    MonitoringQ4PreparedSourceFailureKind primary{};
    int native{};
    struct stat held{};
    if (state_->operations.stat_fd(state_->operations.context, state_->policy, &held) != 0) {
        primary = MonitoringQ4PreparedSourceFailureKind::held_stat;
        native = errno;
    } else if (!same(state_->identity, held)) {
        primary = MonitoringQ4PreparedSourceFailureKind::identity;
    }
    state_->terminal = true;
    const int descriptor = std::exchange(state_->policy, -1);
    const int close_result = state_->operations.close(state_->operations.context, descriptor);
    observe_close(state_->operations, MonitoringQ4PreparedCleanupRole::retained_policy,
                  state_->policy_ordinal, 0, close_result);
    if (close_result != 0) {
        const int close_native = errno;
        MonitoringQ4PreparedSourceFailure failure{
            primary != MonitoringQ4PreparedSourceFailureKind{}
                ? primary
                : MonitoringQ4PreparedSourceFailureKind::close,
            primary != MonitoringQ4PreparedSourceFailureKind{} ? native : close_native, true};
        failure.append_cleanup({MonitoringQ4PreparedCleanupRole::retained_policy,
                                state_->policy_ordinal, 0, close_native, true});
        return std::unexpected{failure};
    }
    if (primary != MonitoringQ4PreparedSourceFailureKind{})
        return std::unexpected{MonitoringQ4PreparedSourceFailure{primary, native}};
    return {};
}

} // namespace epycbench::monitoring::qualification::detail
