#include "monitoring/current_revision_tracked_capture.hpp"

#include <algorithm>
#include <cctype>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr std::array<std::string_view, 18> q4_paths{
    "config/monitoring-qualification/host-v2-profile.json",
    "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
    "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json",
    "schemas/monitoring-q2-context-switch-collection-plan-v1.schema.json",
    "schemas/monitoring-host-q2-backend-result-v1.schema.json",
    "schemas/monitoring-host-q2-backend-error-v1.schema.json",
    "schemas/monitoring-host-q2-backend-authored-v1.schema.json",
    "config/monitoring-qualification/host-v2-q3-temperature-collection-plan.json",
    "schemas/monitoring-q3-temperature-collection-plan-v1.schema.json",
    "schemas/monitoring-host-q3-temperature-evidence-v1.schema.json",
    "schemas/monitoring-host-q3-backend-result-v1.schema.json",
    "config/monitoring-qualification/host-v2-q4-frequency-collection-plan.json",
    "schemas/monitoring-q4-frequency-collection-plan-v1.schema.json",
    "schemas/monitoring-host-q4-frequency-evidence-v1.schema.json",
    "schemas/monitoring-host-q4-backend-result-v1.schema.json",
    "schemas/monitoring-host-q4-backend-error-v1.schema.json",
    "schemas/monitoring-host-q4-backend-authored-v1.schema.json",
    "schemas/monitoring-host-q2-backend-result-v1-evidence-v2.schema.json"};
constexpr std::array<std::size_t, 18> q4_blob_caps{
    1U << 20U, 65536U,    65536U, 2U << 20U, 2U << 20U, 2U << 20U, 2U << 20U, 65536U, 2U << 20U,
    2U << 20U, 2U << 20U, 65536U, 65536U,    65536U,    65536U,    65536U,    65536U,
    2U << 20U};

auto failure(std::string classification, const bool contained = false)
    -> MonitoringQ1GitCommandFailure {
    return {
        {.category = core::ErrorCategory::monitoring,
         .code =
             core::StableErrorCode{contained ? "monitoring.q4_plan_authority.contained_failure"
                                             : "monitoring.q4_plan_authority.repository_rejected"},
         .message = "Q4 governed repository capture rejected",
         .operation = "authenticate_monitoring_q4_plan",
         .os_error = std::nullopt,
         .context = {{"classification", std::move(classification)}},
         .severity = core::ErrorSeverity::error,
         .continuation = core::ContinuationPolicy::abort_command},
        {}};
}
auto parse_head(std::string_view bytes) -> std::optional<std::string> {
    if (bytes.empty() || bytes.back() != '\n' || bytes.find('\n') != bytes.size() - 1)
        return std::nullopt;
    bytes.remove_suffix(1);
    if ((bytes.size() != 40 && bytes.size() != 64) ||
        !std::ranges::all_of(bytes, [](const unsigned char c) {
            return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
        }))
        return std::nullopt;
    return std::string{bytes};
}
auto parse_tree(std::string_view bytes, const std::string_view path, const std::size_t width)
    -> std::optional<std::string> {
    constexpr std::string_view prefix{"100644 blob "};
    if (bytes.empty() || bytes.back() != '\0' || bytes.find('\0') != bytes.size() - 1 ||
        !bytes.starts_with(prefix))
        return std::nullopt;
    bytes.remove_suffix(1);
    const auto tab = bytes.find('\t');
    if (tab == std::string_view::npos || bytes.substr(tab + 1) != path)
        return std::nullopt;
    const auto id = bytes.substr(prefix.size(), tab - prefix.size());
    if (id.size() != width || !std::ranges::all_of(id, [](const unsigned char c) {
            return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
        }))
        return std::nullopt;
    return std::string{id};
}
struct Production final {
    const MonitoringQ1GitExecutable& executable;
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root;
    const MonitoringQ1GitRepositoryMetadata& metadata;
};
} // namespace

auto MonitoringCurrentRevisionTrackedCaptureCore::run_with(Operations operations)
    -> std::expected<MonitoringCurrentRevisionTrackedCapture, MonitoringQ1GitCommandFailure> {
    try {
        if (!operations.recheck || !operations.head || !operations.status || !operations.tree ||
            !operations.blob)
            return std::unexpected{failure("operations")};
        auto initial = operations.recheck(operations.context);
        if (!initial)
            return std::unexpected{std::move(initial.error())};
        if (!*initial)
            return std::unexpected{failure("git_identity")};
        auto head_output = operations.head(operations.context);
        if (!head_output)
            return std::unexpected{std::move(head_output.error())};
        auto head = parse_head(head_output->standard_output);
        if (!head || !head_output->standard_error.empty())
            return std::unexpected{failure("git_result")};
        auto status = operations.status(operations.context);
        if (!status)
            return std::unexpected{std::move(status.error())};
        if (!status->standard_error.empty())
            return std::unexpected{failure("git_result")};
        if (!status->standard_output.empty())
            return std::unexpected{failure("repository_dirty")};
        MonitoringCurrentRevisionTrackedCapture result{std::move(*head)};
        result.records_.reserve(q4_paths.size());
        for (std::size_t slot = 0; slot < q4_paths.size(); ++slot) {
            auto tree = operations.tree(operations.context, q4_paths[slot]);
            if (!tree)
                return std::unexpected{std::move(tree.error())};
            auto object = parse_tree(tree->standard_output, q4_paths[slot], result.head_.size());
            if (!object || !tree->standard_error.empty())
                return std::unexpected{
                    failure(tree->standard_error.empty() ? "tracked_entry" : "git_result")};
            MonitoringQ1GitObjectId token{std::move(*object), result.frame_.get(),
                                          result.head_.size(), slot, q4_blob_caps[slot]};
            auto blob = operations.blob(operations.context, token, result.frame_.get(),
                                        result.head_.size(), slot);
            if (!blob)
                return std::unexpected{std::move(blob.error())};
            if (!blob->standard_error.empty())
                return std::unexpected{failure("git_result")};
            result.records_.push_back(MonitoringCurrentRevisionTrackedRecord{
                std::string{q4_paths[slot]}, std::move(token), std::move(blob->standard_output)});
        }
        auto final = operations.recheck(operations.context);
        if (!final)
            return std::unexpected{std::move(final.error())};
        if (!*final)
            return std::unexpected{failure("git_identity")};
        return result;
    } catch (const std::exception&) {
        return std::unexpected{failure("contained_standard_exception", true)};
    } catch (...) {
        return std::unexpected{failure("contained_unknown_exception", true)};
    }
}

auto MonitoringQ4CurrentRevisionTrackedCapture::run(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1GitRepositoryMetadata& metadata)
    -> std::expected<MonitoringCurrentRevisionTrackedCapture, MonitoringQ1GitCommandFailure> {
    Production value{executable, root, metadata};
    return MonitoringCurrentRevisionTrackedCaptureCore::run_with(
        {.context = &value,
         .recheck =
             [](void* context) { return static_cast<Production*>(context)->metadata.recheck(); },
         .head =
             [](void* context) {
                 const auto& production = *static_cast<Production*>(context);
                 return MonitoringQ1GitSpawn::run(production.executable, production.root,
                                                  MonitoringQ1GitInvocation::head_commit);
             },
         .status =
             [](void* context) {
                 const auto& production = *static_cast<Production*>(context);
                 return MonitoringQ1GitSpawn::run(production.executable, production.root,
                                                  MonitoringQ1GitInvocation::clean_status);
             },
         .tree =
             [](void* context, const std::string_view path) {
                 const auto& production = *static_cast<Production*>(context);
                 return MonitoringQ1GitSpawn::run_tree_path(production.executable, production.root,
                                                            path);
             },
         .blob =
             [](void* context, const MonitoringQ1GitObjectId& object, const void* binding,
                const std::size_t width, const std::size_t slot) {
                 const auto& production = *static_cast<Production*>(context);
                 return MonitoringQ1GitSpawn::run_blob(production.executable, production.root,
                                                       object, binding, width, slot);
             }});
}
} // namespace epycbench::monitoring::qualification::detail
