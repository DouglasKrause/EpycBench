#include "monitoring/host_q3_historical_git_authority.hpp"

#include "monitoring/host_q1_git_spawn.hpp"

namespace epycbench::monitoring::qualification::detail {
namespace {
auto rejected(std::string classification) -> MonitoringQ3HistoricalGitFailure {
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{"monitoring.q3.repository_invalid"},
             .message = "Q3 historical repository authority rejected",
             .operation = "authenticate_monitoring_q3_repository_history",
             .os_error = std::nullopt,
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command}};
}
auto commit_id(std::string_view value) noexcept -> bool {
    return (value.size() == 40U || value.size() == 64U) &&
           std::ranges::all_of(value, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
auto transport(MonitoringQ1GitCommandFailure failure)
    -> MonitoringQ3HistoricalGitFailure {
    auto overflow = rejected("command_cleanup_overflow");
    if (failure.cleanup.size() > overflow.cleanup.size()) return overflow;
    MonitoringQ3HistoricalGitFailure result{std::move(failure.primary)};
    result.cleanup_count = failure.cleanup.size();
    for (std::size_t index{}; index < result.cleanup_count; ++index)
        result.cleanup[index].emplace(std::move(failure.cleanup[index]));
    return result;
}
constexpr std::array current_paths{
    "config/monitoring-qualification/host-v2-policy.json",
    "config/monitoring-qualification/host-v2-profile.json",
    "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
    "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json",
    "config/monitoring-qualification/host-v2-q3-temperature-collection-plan.json"};
} // namespace

auto MonitoringQ3HistoricalGitAuthority::current_matches(
    const MonitoringQ1GitFixedResults& capture, std::string_view commit,
    const std::array<std::string_view, 4>& expected) noexcept -> bool {
    if (capture.head_ != commit || capture.records.size() != current_paths.size()) return false;
    for (std::size_t index{}; index < current_paths.size(); ++index)
        if (capture.records[index].path_ != current_paths[index]) return false;
    for (std::size_t index{}; index < expected.size(); ++index)
        if (capture.records[index + 1U].bytes_ != expected[index]) return false;
    return true;
}

auto MonitoringQ3HistoricalGitAuthority::authenticate(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1GitRepositoryMetadata& metadata, std::string_view q0,
    std::string_view q1, std::string_view q2, std::string_view current,
    std::string_view profile, std::string_view q1_plan, std::string_view q2_plan,
    std::string_view q3_plan) noexcept -> std::expected<void, MonitoringQ3HistoricalGitFailure> {
    struct Context final {
        const MonitoringQ1GitExecutable& executable;
        const core::detail::OwnedGovernedRepositoryRootDescriptor& root;
        const MonitoringQ1GitRepositoryMetadata& metadata;
    } context{executable, root, metadata};
    return authenticate_with(
        {.context = &context,
         .capture = [](void* raw) {
             auto& value = *static_cast<Context*>(raw);
             return MonitoringQ1GitFixedResultAuthority::run_with_q3_plan(
                 value.executable, value.root, value.metadata);
         },
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
         .blob = [](void* raw, std::string_view object, const void* binding, std::size_t width,
                    std::size_t slot) {
             auto& value = *static_cast<Context*>(raw);
             MonitoringQ1GitObjectId token{std::string{object}, binding, width, slot};
             return MonitoringQ1GitSpawn::run_blob(value.executable, value.root, token, binding,
                                                   width, slot);
         }},
        q0, q1, q2, current, profile, q1_plan, q2_plan, q3_plan);
}

auto MonitoringQ3HistoricalGitAuthority::authenticate_with(
    Operations operations, std::string_view q0, std::string_view q1, std::string_view q2,
    std::string_view current, std::string_view profile, std::string_view q1_plan,
    std::string_view q2_plan, std::string_view q3_plan) noexcept
    -> std::expected<void, MonitoringQ3HistoricalGitFailure> {
    try {
        if (!operations.capture || !operations.object_type || !operations.ancestry ||
            !operations.tree || !operations.blob)
            return std::unexpected{rejected("operations")};
        if (!commit_id(q0) || !commit_id(q1) || !commit_id(q2) || !commit_id(current) ||
            q0.size() != q1.size() || q1.size() != q2.size() || q2.size() != current.size())
            return std::unexpected{rejected("commit_identity")};
        const std::array<std::string_view, 4> expected{profile, q1_plan, q2_plan, q3_plan};
        const std::array commits{q0, q1, q2, current};
        for (std::size_t index{}; index < commits.size(); ++index) {
            if (operations.fault)
                operations.fault(operations.context, static_cast<unsigned int>(index));
            auto type = operations.object_type(operations.context, commits[index]);
            if (!type) return std::unexpected{transport(std::move(type.error()))};
            if (type->standard_output != "commit\n" || !type->standard_error.empty())
                return std::unexpected{rejected("commit_object")};
        }
        if (operations.fault) operations.fault(operations.context, 4U);
        auto initial = operations.capture(operations.context);
        if (!initial) return std::unexpected{transport(std::move(initial.error()))};
        if (!current_matches(*initial, current, expected))
            return std::unexpected{rejected("current_capture")};

        const std::array older{q0, q1, q2};
        const std::array newer{q1, q2, current};
        for (std::size_t edge{}; edge < older.size(); ++edge) {
            if (operations.fault)
                operations.fault(operations.context, 5U + static_cast<unsigned int>(edge));
            auto ancestry = operations.ancestry(operations.context, older[edge], newer[edge]);
            if (!ancestry) return std::unexpected{transport(std::move(ancestry.error()))};
            if (!ancestry->standard_output.empty() || !ancestry->standard_error.empty())
                return std::unexpected{rejected("ancestry")};
        }

        constexpr std::array paths{
            "config/monitoring-qualification/host-v2-profile.json",
            "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
            "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json"};
        constexpr std::array invocations{
            MonitoringQ1GitInvocation::historical_tree_profile,
            MonitoringQ1GitInvocation::historical_tree_q1_plan,
            MonitoringQ1GitInvocation::historical_tree_q2_plan};
        const std::array expected_bytes{profile, q1_plan, q2_plan};
        int binding{};
        std::size_t slot{};
        for (std::size_t stage{}; stage < 2U; ++stage) {
            const auto stage_commit = stage == 0U ? q1 : q2;
            const std::size_t count = stage == 0U ? 2U : 3U;
            for (std::size_t item{}; item < count; ++item, ++slot) {
                if (operations.fault)
                    operations.fault(operations.context, 8U + static_cast<unsigned int>(slot));
                auto tree = operations.tree(operations.context, invocations[item], stage_commit);
                if (!tree) return std::unexpected{transport(std::move(tree.error()))};
                if (!tree->standard_error.empty())
                    return std::unexpected{rejected("historical_tree")};
                auto object = MonitoringQ1GitFixedResultAuthority::parse_tree(
                    tree->standard_output, paths[item], stage_commit);
                if (!object) return std::unexpected{rejected("historical_tree")};
                auto blob = operations.blob(operations.context, *object, &binding,
                                            stage_commit.size(), slot);
                if (!blob) return std::unexpected{transport(std::move(blob.error()))};
                if (!blob->standard_error.empty() || blob->standard_output != expected_bytes[item])
                    return std::unexpected{rejected("historical_blob")};
            }
        }
        if (operations.fault) operations.fault(operations.context, 13U);
        auto final = operations.capture(operations.context);
        if (!final) return std::unexpected{transport(std::move(final.error()))};
        if (!current_matches(*final, current, expected))
            return std::unexpected{rejected("final_capture")};
        return {};
    } catch (...) {
        return std::unexpected{rejected("contained")};
    }
}

} // namespace epycbench::monitoring::qualification::detail
