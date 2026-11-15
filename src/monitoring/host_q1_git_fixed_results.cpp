#include "monitoring/host_q1_git_fixed_results.hpp"
#include <algorithm>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto hex_id(std::string_view value) noexcept -> bool {
    return (value.size() == 40 || value.size() == 64) &&
           std::ranges::all_of(value, [](unsigned char c) {
               return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
           });
}
auto rejected(std::string_view classification, bool contained = false)
    -> MonitoringQ1GitCommandFailure {
    return {
        {.category = core::ErrorCategory::monitoring,
         .code =
             core::StableErrorCode{contained ? "monitoring.q1_plan_authority.contained_failure"
                                             : "monitoring.q1_plan_authority.repository_rejected"},
         .message = "Q1 Git command result rejected",
         .operation = "authenticate_monitoring_q1_plan",
         .os_error = std::nullopt,
         .context = {{"classification", std::string{classification}}},
         .severity = core::ErrorSeverity::error,
         .continuation = core::ContinuationPolicy::abort_command},
        {}};
}
struct ProductionContext final {
    const MonitoringQ1GitExecutable& executable;
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root;
    const MonitoringQ1GitRepositoryMetadata& metadata;
};
auto production_recheck(void* context) -> std::expected<bool, MonitoringQ1GitCommandFailure> {
    return static_cast<ProductionContext*>(context)->metadata.recheck();
}
auto production_run(void* context, MonitoringQ1GitInvocation invocation)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    const auto& value = *static_cast<ProductionContext*>(context);
    return MonitoringQ1GitSpawn::run(value.executable, value.root, invocation);
}
} // namespace
auto MonitoringQ1GitFixedResultAuthority::parse_head(std::string_view bytes)
    -> std::optional<std::string> {
    if (bytes.empty() || bytes.back() != '\n' || bytes.find('\n') != bytes.size() - 1U)
        return std::nullopt;
    bytes.remove_suffix(1);
    return hex_id(bytes) ? std::optional<std::string>{bytes} : std::nullopt;
}
auto MonitoringQ1GitFixedResultAuthority::parse_tree(std::string_view bytes, std::string_view path,
                                                     std::string_view head)
    -> std::optional<std::string> {
    constexpr std::string_view prefix{"100644 blob "};
    if (bytes.empty() || bytes.back() != '\0' || bytes.find('\0') != bytes.size() - 1U ||
        !bytes.starts_with(prefix))
        return std::nullopt;
    bytes.remove_suffix(1);
    const auto tab = bytes.find('\t');
    if (tab == std::string_view::npos || bytes.substr(tab + 1U) != path)
        return std::nullopt;
    const auto object = bytes.substr(prefix.size(), tab - prefix.size());
    if (!hex_id(object) || object.size() != head.size())
        return std::nullopt;
    return std::string{object};
}
auto MonitoringQ1GitFixedResultAuthority::run(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1GitRepositoryMetadata& metadata)
    -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure> {
    ProductionContext context{executable, root, metadata};
    return run_with({.context = &context,
                     .recheck = production_recheck,
                     .run = production_run,
                     .run_blob =
                         [](void* value, const MonitoringQ1GitObjectId& object_id,
                            const void* binding, std::size_t width, std::size_t slot) {
                             const auto& production = *static_cast<ProductionContext*>(value);
                             return MonitoringQ1GitSpawn::run_blob(production.executable,
                                                                   production.root, object_id,
                                                                   binding, width, slot);
                         }},
                    3U);
}
auto MonitoringQ1GitFixedResultAuthority::run_with(Operations operations)
    -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure> {
    return run_with(operations, 3U);
}
auto MonitoringQ1GitFixedResultAuthority::run_with_q2_plan(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1GitRepositoryMetadata& metadata)
    -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure> {
    ProductionContext context{executable, root, metadata};
    return run_with({.context = &context,
                     .recheck = production_recheck,
                     .run = production_run,
                     .run_blob =
                         [](void* value, const MonitoringQ1GitObjectId& object_id,
                            const void* binding, std::size_t width, std::size_t slot) {
                             const auto& production = *static_cast<ProductionContext*>(value);
                             return MonitoringQ1GitSpawn::run_blob(production.executable,
                                                                   production.root, object_id,
                                                                   binding, width, slot);
                         }},
                    4U);
}
auto MonitoringQ1GitFixedResultAuthority::run_with_q3_plan(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1GitRepositoryMetadata& metadata)
    -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure> {
    ProductionContext context{executable, root, metadata};
    return run_with({.context = &context,
                     .recheck = production_recheck,
                     .run = production_run,
                     .run_blob =
                         [](void* value, const MonitoringQ1GitObjectId& object_id,
                            const void* binding, std::size_t width, std::size_t slot) {
                             const auto& production = *static_cast<ProductionContext*>(value);
                             return MonitoringQ1GitSpawn::run_blob(production.executable,
                                                                   production.root, object_id,
                                                                   binding, width, slot);
                         }},
                    5U);
}
auto MonitoringQ1GitFixedResultAuthority::run_with(Operations operations, std::size_t count)
    -> std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure> {
    try {
        auto initial = operations.recheck(operations.context);
        if (!initial)
            return std::unexpected{std::move(initial.error())};
        if (!*initial)
            return std::unexpected{rejected("git_identity")};
        auto head_output =
            operations.run(operations.context, MonitoringQ1GitInvocation::head_commit);
        if (!head_output)
            return std::unexpected{std::move(head_output.error())};
        auto head = parse_head(head_output->standard_output);
        if (!head || !head_output->standard_error.empty())
            return std::unexpected{rejected("git_result")};
        auto status = operations.run(operations.context, MonitoringQ1GitInvocation::clean_status);
        if (!status)
            return std::unexpected{std::move(status.error())};
        if (!status->standard_error.empty())
            return std::unexpected{rejected("git_result")};
        if (!status->standard_output.empty())
            return std::unexpected{rejected("repository_dirty")};
        constexpr std::array<std::string_view, 5> paths{
            "config/monitoring-qualification/host-v2-policy.json",
            "config/monitoring-qualification/host-v2-profile.json",
            "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
            "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json",
            "config/monitoring-qualification/host-v2-q3-temperature-collection-plan.json"};
        constexpr std::array invocations{
            MonitoringQ1GitInvocation::tree_policy, MonitoringQ1GitInvocation::tree_profile,
            MonitoringQ1GitInvocation::tree_plan, MonitoringQ1GitInvocation::tree_q2_plan,
            MonitoringQ1GitInvocation::tree_q3_plan};
        MonitoringQ1GitFixedResults result{std::move(*head)};
        if (count < 3U || count > paths.size())
            return std::unexpected{rejected("tracked_entry")};
        result.records.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto output = operations.run(operations.context, invocations[index]);
            if (!output)
                return std::unexpected{std::move(output.error())};
            auto object = parse_tree(output->standard_output, paths[index], result.head_);
            if (!object || !output->standard_error.empty())
                return std::unexpected{
                    rejected(output->standard_error.empty() ? "tracked_entry" : "git_result")};
            MonitoringQ1GitObjectId object_id{std::move(*object), result.frame_.get(),
                                              result.head_.size(), index};
            auto blob = operations.run_blob(operations.context, object_id, result.frame_.get(),
                                            result.head_.size(), index);
            if (!blob)
                return std::unexpected{std::move(blob.error())};
            if (!blob->standard_error.empty())
                return std::unexpected{rejected("git_result")};
            result.records.push_back(MonitoringQ1GitFixedRecord{
                std::string{paths[index]}, std::move(object_id), std::move(blob->standard_output)});
        }
        auto final = operations.recheck(operations.context);
        if (!final)
            return std::unexpected{std::move(final.error())};
        if (!*final)
            return std::unexpected{rejected("git_identity")};
        return result;
    } catch (const std::exception&) {
        return std::unexpected{rejected("contained_standard_exception", true)};
    } catch (...) {
        return std::unexpected{rejected("contained_unknown_exception", true)};
    }
}
} // namespace epycbench::monitoring::qualification::detail
