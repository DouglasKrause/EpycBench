#include "monitoring/host_q4_historical_git_authority.hpp"

#include "monitoring/host_q1_git_fixed_results.hpp"
#include "monitoring/host_q4_complete_q3_predecessor_authority.hpp"
#include "monitoring/host_q1_validated_public_values.hpp"
#include "monitoring/host_q1_git_spawn.hpp"

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr std::array<std::string_view, 4> required_commits{
    "4721bcf2efe66dfcfd9532fde4853480937d7fed",
    "5990de3e0cfc80fbd74120125d9d2ebcfecbf72e",
    "fda00b486bd997210f35d513afc06bd29191490d",
    "485d5c314b6c32ab1b2a7d17f095b8325c387cd1"};
constexpr std::array<std::string_view, 4> paths{
    "config/monitoring-qualification/host-v2-profile.json",
    "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
    "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json",
    "config/monitoring-qualification/host-v2-q3-temperature-collection-plan.json"};
constexpr std::array invocations{
    MonitoringQ1GitInvocation::historical_tree_profile,
    MonitoringQ1GitInvocation::historical_tree_q1_plan,
    MonitoringQ1GitInvocation::historical_tree_q2_plan,
    MonitoringQ1GitInvocation::historical_tree_q3_plan};
constexpr std::array<std::size_t, 4> caps{1U << 20U, 1U << 16U, 1U << 16U,
                                          1U << 16U};

auto rejected(std::string classification) -> MonitoringQ4HistoricalGitFailure {
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{"monitoring.q4.repository_history_invalid"},
             .message = "Q4 historical repository authority rejected",
             .operation = "authenticate_monitoring_q4_repository_history",
             .os_error = std::nullopt,
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command}};
}
auto valid_commit(std::string_view value) noexcept -> bool {
    return (value.size() == 40U || value.size() == 64U) &&
           std::ranges::all_of(value, [](char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}
auto transported(MonitoringQ1GitCommandFailure failure) -> MonitoringQ4HistoricalGitFailure {
    if (failure.cleanup.size() > 16U)
        return rejected("command_cleanup_overflow");
    MonitoringQ4HistoricalGitFailure result{std::move(failure.primary)};
    result.cleanup_count = failure.cleanup.size();
    for (std::size_t index{}; index < result.cleanup_count; ++index)
        result.cleanup[index].emplace(std::move(failure.cleanup[index]));
    return result;
}
} // namespace

auto MonitoringQ4HistoricalGitAuthority::authenticate(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1GitRepositoryMetadata&,
    const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4CompleteQ3PredecessorAuthority* snapshot,
    const std::array<std::string_view, 4>* retained_commits, std::string_view retained_producer)
    -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure> {
    struct Context final {
        const MonitoringQ1GitExecutable& executable;
        const core::detail::OwnedGovernedRepositoryRootDescriptor& root;
    } context{executable, root};
    Operations operations{
        .context = &context,
         .object_type = [](void* raw, std::string_view object) {
             auto& value = *static_cast<Context*>(raw);
             return MonitoringQ1GitSpawn::run_exact_object_type(value.executable, value.root,
                                                                 object);
         },
         .ancestry = [](void* raw, std::string_view older, std::string_view newer) {
             auto& value = *static_cast<Context*>(raw);
             return MonitoringQ1GitSpawn::run_historical_ancestry(value.executable, value.root,
                                                                  older, newer);
         },
         .tree = [](void* raw, MonitoringQ1GitInvocation invocation, std::string_view commit) {
             auto& value = *static_cast<Context*>(raw);
             return MonitoringQ1GitSpawn::run_historical_tree(value.executable, value.root,
                                                              invocation, commit);
         },
         .blob = [](void* raw, std::string_view object, const void* binding,
                    std::size_t width, std::size_t slot, std::size_t cap) {
             auto& value = *static_cast<Context*>(raw);
             MonitoringQ1GitObjectId token{std::string{object}, binding, width, slot, cap};
             return MonitoringQ1GitSpawn::run_blob(value.executable, value.root, token,
                                                   binding, width, slot);
         },
         .q4_tree = [](void* raw, MonitoringQ4HistoricalSelector selector, std::string_view commit) {
             auto& value = *static_cast<Context*>(raw);
             return MonitoringQ1GitSpawn::run_q4_historical_tree(value.executable, value.root,
                                                               selector, commit);
         }};
    auto fallback = bootstrap_failure();
    if (retained_commits) {
        auto history = authenticate_commits_impl(operations, candidate, *retained_commits,
                                                 std::move(fallback), true);
        if (!history) return history;
        return authenticate_producer_with(operations, candidate, std::move(*history),
                                           retained_producer, bootstrap_failure());
    }
    if (snapshot && snapshot->completed_snapshot_)
        return authenticate_completed_with(operations, candidate, *snapshot, std::move(fallback));
    if (snapshot)
        return authenticate_campaign_with(operations, candidate, *snapshot, std::move(fallback));
    return authenticate_delegated(
        operations, candidate, std::move(fallback),
        {nullptr, [](void*) { return bootstrap_failure(); }});
}

auto MonitoringQ4HistoricalGitAuthority::bootstrap_failure()
    -> MonitoringQ4HistoricalGitFailure {
    return rejected("bootstrap_contained");
}

auto MonitoringQ4HistoricalGitAuthority::authenticate_delegated(
    Operations operations, const MonitoringQ4GovernedInputCandidate& candidate,
    MonitoringQ4HistoricalGitFailure fallback, BootstrapOperations bootstrap) noexcept
    -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure> {
    if (!bootstrap.construct)
        return std::unexpected{std::move(fallback)};
    try {
        auto stock = bootstrap.construct(bootstrap.context);
        return authenticate_with(operations, candidate, std::move(stock));
    } catch (...) {
        return std::unexpected{std::move(fallback)};
    }
}

auto MonitoringQ4HistoricalGitAuthority::authenticate_with(
    Operations operations, const MonitoringQ4GovernedInputCandidate& candidate,
    MonitoringQ4HistoricalGitFailure bootstrap) noexcept
    -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure> {
    return authenticate_commits_with(operations, candidate, required_commits,
                                     std::move(bootstrap));
}

auto MonitoringQ4HistoricalGitAuthority::authenticate_commits_with(
    Operations operations, const MonitoringQ4GovernedInputCandidate& candidate,
    const std::array<std::string_view, 4>& supplied_commits,
    MonitoringQ4HistoricalGitFailure bootstrap) noexcept
    -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure> {
    return authenticate_commits_impl(operations, candidate, supplied_commits, std::move(bootstrap), false);
}

auto MonitoringQ4HistoricalGitAuthority::authenticate_campaign_with(
    Operations operations, const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot,
    MonitoringQ4HistoricalGitFailure bootstrap) noexcept
    -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure> {
    if (!candidate.valid_ || !candidate.binding_ || !snapshot.active_.all() || snapshot.history_ || snapshot.value_ || snapshot.binding_attempted_ ||
        snapshot.candidate_ != &candidate || !snapshot.campaign_values_ ||
        (snapshot.campaign_values_->qualification_id() != "MON-007.host-v2.campaign-20261005-01" && snapshot.campaign_values_->qualification_id() != "MON-007.host-v2.campaign-20261018-01" && snapshot.campaign_values_->qualification_id() != "MON-007.host-v2.campaign-20261020-01" && snapshot.campaign_values_->qualification_id() != "MON-007.host-v2.campaign-20261024-01" && snapshot.campaign_values_->qualification_id() != "MON-007.host-v2.campaign-20261102-01") ||
        snapshot.campaign_values_->profile_sha256() != candidate.profile().sha256.to_hex())
        return std::unexpected{std::move(bootstrap)};
    const auto& ids = snapshot.provisional_commits_;
    return authenticate_commits_impl(operations, candidate,
        {ids[0], ids[1], ids[2], ids[3]}, std::move(bootstrap), true);
}

auto MonitoringQ4HistoricalGitAuthority::authenticate_completed_with(
    Operations operations, const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot,
    MonitoringQ4HistoricalGitFailure bootstrap) noexcept
    -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure> {
    if (!snapshot.completed_snapshot_ || !snapshot.completed_active_.all())
        return std::unexpected{std::move(bootstrap)};
    try {
        auto fallback = bootstrap_failure();
        auto history = authenticate_campaign_with(operations, candidate, snapshot, std::move(bootstrap));
        if (!history) return history;
        return authenticate_producer_with(operations, candidate, std::move(*history),
                                         snapshot.producer_commit_, std::move(fallback));
    } catch (...) { return std::unexpected{bootstrap_failure()}; }
}

auto MonitoringQ4HistoricalGitAuthority::authenticate_producer_with(
    Operations operations, const MonitoringQ4GovernedInputCandidate& candidate,
    MonitoringQ4HistoricalGitValue history, std::string_view producer,
    MonitoringQ4HistoricalGitFailure bootstrap) noexcept
    -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure> {
    try {
        if (!candidate.valid_ || !candidate.binding_ || history.binding_ != candidate.binding_ ||
            history.current_commit() != candidate.captured_head() || !valid_commit(producer) ||
            producer.size() != history.current_commit().size() || !operations.object_type ||
            !operations.ancestry || !operations.tree || !operations.q4_tree || !operations.blob)
            return std::unexpected{rejected("completed_producer_binding")};
        auto type = operations.object_type(operations.context, producer);
        if (!type) return std::unexpected{transported(std::move(type.error()))};
        if (type->standard_output != "commit\n" || !type->standard_error.empty())
            return std::unexpected{rejected("completed_producer_object")};
        const std::array edges{
            std::pair{history.q3_commit(), producer}, std::pair{producer, history.current_commit()}};
        for (const auto& edge : edges) {
            auto checked = operations.ancestry(operations.context, edge.first, edge.second);
            if (!checked) return std::unexpected{transported(std::move(checked.error()))};
            if (!checked->standard_output.empty() || !checked->standard_error.empty())
                return std::unexpected{rejected("completed_producer_ancestry")};
        }
        constexpr std::array<std::string_view, 6> q4_paths{
            "config/monitoring-qualification/host-v2-q4-frequency-collection-plan.json",
            "schemas/monitoring-q4-frequency-collection-plan-v1.schema.json",
            "schemas/monitoring-host-q4-frequency-evidence-v1.schema.json",
            "schemas/monitoring-host-q4-backend-result-v1.schema.json",
            "schemas/monitoring-host-q4-backend-error-v1.schema.json",
            "schemas/monitoring-host-q4-backend-authored-v1.schema.json"};
        const std::array<std::string_view, 10> expected{
            candidate.profile().bytes, candidate.q1_plan().bytes,
            candidate.q2_plan().bytes, candidate.q3_plan().bytes, candidate.q4_plan().bytes,
            candidate.q4_plan_schema().bytes, candidate.q4_evidence_schema().bytes,
            candidate.q4_result_schema().bytes, candidate.q4_error_schema().bytes,
            candidate.q4_authored_schema().bytes};
        for (std::size_t i{}; i < expected.size(); ++i) {
            auto tree = i < 4U ? operations.tree(operations.context, invocations[i], producer) :
                operations.q4_tree(operations.context, static_cast<MonitoringQ4HistoricalSelector>(i - 4U), producer);
            if (!tree) return std::unexpected{transported(std::move(tree.error()))};
            if (!tree->standard_error.empty()) return std::unexpected{rejected("completed_producer_tree")};
            const auto path = i < 4U ? paths[i] : q4_paths[i - 4U];
            auto object = MonitoringQ1GitFixedResultAuthority::parse_tree(tree->standard_output, path, producer);
            if (!object) return std::unexpected{rejected("completed_producer_tree")};
            const auto cap = i < 4U ? caps[i] : 65536U;
            auto blob = operations.blob(operations.context, *object, candidate.binding_.get(),
                                       producer.size(), 9U + i, cap);
            if (!blob) return std::unexpected{transported(std::move(blob.error()))};
            if (!blob->standard_error.empty() || blob->standard_output != expected[i])
                return std::unexpected{rejected("completed_producer_blob")};
        }
        history.completed_q4_producer_ = producer;
        return history;
    } catch (...) { return std::unexpected{std::move(bootstrap)}; }
}

auto MonitoringQ4HistoricalGitAuthority::authenticate_commits_impl(
    Operations operations, const MonitoringQ4GovernedInputCandidate& candidate,
    const std::array<std::string_view, 4>& supplied_commits,
    MonitoringQ4HistoricalGitFailure bootstrap, bool campaign) noexcept
    -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure> {
    std::array<std::optional<MonitoringQ4HistoricalGitFailure>, 10> stocks;
    enum Stock : std::size_t {
        contained,
        invalid_operations,
        invalid_fixed_commit,
        invalid_binding,
        invalid_commit,
        invalid_object,
        invalid_ancestry,
        invalid_tree,
        invalid_blob,
        cleanup_overflow
    };
    try {
        if (operations.fault)
            operations.fault(operations.context, 199U);
        stocks[contained].emplace(rejected("contained"));
        constexpr std::array<std::string_view, 9> classifications{
            "operations", "fixed_commit", "candidate_binding", "commit_identity", "commit_object",
            "ancestry", "historical_tree", "historical_blob", "command_cleanup_overflow"};
        for (std::size_t index{}; index < classifications.size(); ++index) {
            if (operations.fault)
                operations.fault(operations.context, static_cast<unsigned int>(200U + index));
            stocks[index + 1U].emplace(rejected(std::string{classifications[index]}));
        }
        const auto fail = [&](Stock which) {
            return std::expected<MonitoringQ4HistoricalGitValue,
                                 MonitoringQ4HistoricalGitFailure>{
                std::unexpected{std::move(*stocks[which])}};
        };
        if (!operations.object_type || !operations.ancestry || !operations.tree ||
            !operations.blob)
            return fail(invalid_operations);
        if (!campaign && supplied_commits != required_commits)
            return fail(invalid_fixed_commit);
        if (!candidate.valid_ || !candidate.binding_)
            return fail(invalid_binding);
        const std::array ids{supplied_commits[0], supplied_commits[1], supplied_commits[2],
                             supplied_commits[3], std::string_view{candidate.head_}};
        for (const auto id : ids)
            if (!valid_commit(id) || id.size() != ids[0].size())
                return fail(invalid_commit);
        std::array<std::string, 5> owned;
        for (std::size_t index{}; index < owned.size(); ++index)
            owned[index] = ids[index];
        if (operations.fault)
            operations.fault(operations.context, 209U);
        const auto command = [&](MonitoringQ1GitCommandFailure failure) {
            if (failure.cleanup.size() > 16U)
                return fail(cleanup_overflow);
            return std::expected<MonitoringQ4HistoricalGitValue,
                                 MonitoringQ4HistoricalGitFailure>{
                std::unexpected{transported(std::move(failure))}};
        };
        for (std::size_t index{}; index < ids.size(); ++index) {
            if (operations.fault)
                operations.fault(operations.context, static_cast<unsigned int>(index));
            auto type = operations.object_type(operations.context, ids[index]);
            if (!type)
                return command(std::move(type.error()));
            if (type->standard_output != "commit\n" || !type->standard_error.empty())
                return fail(invalid_object);
        }
        for (std::size_t edge{}; edge < 4U; ++edge) {
            if (operations.fault)
                operations.fault(operations.context, static_cast<unsigned int>(5U + edge));
            auto ancestry = operations.ancestry(operations.context, ids[edge], ids[edge + 1U]);
            if (!ancestry)
                return command(std::move(ancestry.error()));
            if (!ancestry->standard_output.empty() || !ancestry->standard_error.empty())
                return fail(invalid_ancestry);
        }

        const std::array<std::string_view, 4> bytes{
            candidate.profile().bytes, candidate.q1_plan().bytes,
            candidate.q2_plan().bytes, candidate.q3_plan().bytes};
        std::size_t ordinal{};
        for (std::size_t stage = 1U; stage <= 3U; ++stage) {
            for (std::size_t item{}; item <= stage; ++item, ++ordinal) {
                if (operations.fault)
                    operations.fault(operations.context,
                                     static_cast<unsigned int>(9U + ordinal));
                auto tree = operations.tree(operations.context, invocations[item], ids[stage]);
                if (!tree)
                    return command(std::move(tree.error()));
                if (!tree->standard_error.empty())
                    return fail(invalid_tree);
                auto object = MonitoringQ1GitFixedResultAuthority::parse_tree(
                    tree->standard_output, paths[item], ids[stage]);
                if (!object)
                    return fail(invalid_tree);
                auto blob = operations.blob(operations.context, *object,
                                            candidate.binding_.get(), ids[0].size(), ordinal,
                                            caps[item]);
                if (!blob)
                    return command(std::move(blob.error()));
                if (!blob->standard_error.empty() || blob->standard_output != bytes[item])
                    return fail(invalid_blob);
            }
        }
        return MonitoringQ4HistoricalGitValue{std::move(owned), candidate.binding_};
    } catch (...) {
        if (stocks[contained])
            return std::unexpected{std::move(*stocks[contained])};
        return std::unexpected{std::move(bootstrap)};
    }
}

} // namespace epycbench::monitoring::qualification::detail
