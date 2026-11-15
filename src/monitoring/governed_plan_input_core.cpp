#include "monitoring/governed_plan_input_core.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto bootstrap_failure() noexcept -> MonitoringQ1GitCommandFailure {
    MonitoringQ1GitCommandFailure result{};
    result.primary.category=core::ErrorCategory::monitoring;
    result.primary.severity=core::ErrorSeverity::error;
    result.primary.continuation=core::ContinuationPolicy::abort_command;
    return result;
}
auto identity(const struct stat& value) noexcept -> MonitoringGovernedPlanInputIdentity {
    return {static_cast<std::uint64_t>(value.st_dev),  static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_size), static_cast<std::uint32_t>(value.st_mode),
            static_cast<std::uint32_t>(value.st_uid),  static_cast<std::uint32_t>(value.st_nlink),
            static_cast<std::int64_t>(value.st_mtim.tv_sec),
            static_cast<std::int64_t>(value.st_mtim.tv_nsec),
            static_cast<std::int64_t>(value.st_ctim.tv_sec),
            static_cast<std::int64_t>(value.st_ctim.tv_nsec)};
}
auto failure(std::string_view stable_code, std::string_view message, std::string_view operation,
             std::string classification, int native = 0) -> MonitoringQ1GitCommandFailure {
    std::optional<core::OsError> os;
    if (native != 0)
        os = core::OsError::capture(std::error_code{native, std::generic_category()});
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{std::string{stable_code}},
             .message = std::string{message},
             .operation = std::string{operation},
             .os_error = std::move(os),
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command},
            {}};
}
constexpr std::array<std::string_view,11> diagnostic_classes{
    "operations","tracked_entry","root","config","plan","identity","read","growth",
    "mutation","contained_failure","cleanup_failure"};
struct DiagnosticStock final {
    std::array<MonitoringQ1GitCommandFailure,diagnostic_classes.size()> primary;
    std::array<core::Error,4> cleanup;
};
auto make_diagnostic_stock(std::string_view stable_code, std::string_view message,
                           std::string_view operation)
    -> DiagnosticStock {
    DiagnosticStock result;
    for(std::size_t i{};i<diagnostic_classes.size();++i){
        result.primary[i]=failure(stable_code, message, operation, std::string{diagnostic_classes[i]});
        result.primary[i].cleanup.reserve(4);
        result.primary[i].primary.context["native_errno"]=0;
    }
    for(auto& value:result.cleanup){value=failure(stable_code, message, operation, "cleanup_failure").primary;value.context["native_errno"]=0;}
    return result;
}
auto take(DiagnosticStock& stock,std::string_view classification) noexcept
    -> MonitoringQ1GitCommandFailure {
    const auto found=std::ranges::find(diagnostic_classes,classification);
    const auto index=found==diagnostic_classes.end()?9U:static_cast<std::size_t>(found-diagnostic_classes.begin());
    return std::move(stock.primary[index]);
}
struct CloseFailures final {
    std::array<int, 4> values{};
    std::size_t count{};
};
template <typename Operations>
auto close_all(std::array<int, 4>& fds, const Operations& operations) noexcept -> CloseFailures {
    CloseFailures failures;
    for (auto it = fds.rbegin(); it != fds.rend(); ++it) {
        const int fd = std::exchange(*it, -1);
        if (fd >= 0 && operations.close_fd(operations.context, fd) != 0)
            failures.values[failures.count++] = errno;
    }
    return failures;
}
template <typename Operations>
auto stat_checked(int fd, unsigned int euid, bool directory, unsigned int leaf_mode,
                  const Operations& operations)
    -> std::optional<MonitoringGovernedPlanInputIdentity> {
    struct stat value{};
    if (operations.stat_fd(operations.context, fd, &value) != 0 || value.st_uid != euid ||
        (directory
             ? !S_ISDIR(value.st_mode) || (value.st_mode & 0022) != 0
             : !S_ISREG(value.st_mode) || value.st_nlink != 1 ||
                   (static_cast<unsigned int>(value.st_mode) & 07777U) != leaf_mode ||
                   value.st_size < 0 || value.st_size > 65536))
        return std::nullopt;
    return identity(value);
}
} // namespace

MonitoringGovernedPlanInputCore::MonitoringGovernedPlanInputCore(
    std::array<int, 4> descriptors, std::array<MonitoringGovernedPlanInputIdentity, 4> identities,
    std::string bytes, core::Sha256Digest digest, std::string head,
    std::shared_ptr<const void> capture_binding, std::shared_ptr<const void> projection,
    Operations operations, Specification specification,
    MonitoringQ1GitCommandFailure retirement_failure,
    std::array<core::Error,4> retirement_cleanup) noexcept
    : descriptors_{descriptors}, identities_{identities}, bytes_{std::move(bytes)}, digest_{digest},
      head_{std::move(head)}, capture_binding_{std::move(capture_binding)},
      projection_{std::move(projection)}, operations_{operations}, specification_{specification},
      retirement_failure_{std::move(retirement_failure)},
      retirement_cleanup_{std::move(retirement_cleanup)} {}
MonitoringGovernedPlanInputCore::MonitoringGovernedPlanInputCore(
    MonitoringGovernedPlanInputCore&& other) noexcept
    : descriptors_{std::exchange(other.descriptors_, {-1, -1, -1, -1})},
      identities_{other.identities_}, bytes_{std::move(other.bytes_)}, digest_{other.digest_},
      head_{std::move(other.head_)}, capture_binding_{std::move(other.capture_binding_)},
      projection_{std::move(other.projection_)}, operations_{other.operations_},
      specification_{other.specification_}, retirement_failure_{std::move(other.retirement_failure_)},
      retirement_cleanup_{std::move(other.retirement_cleanup_)},
      active_{std::exchange(other.active_, false)} {}
MonitoringGovernedPlanInputCore::~MonitoringGovernedPlanInputCore() noexcept {
    if (active_) static_cast<void>(retire());
}
auto MonitoringGovernedPlanInputCore::retain(
    int root, const MonitoringQ1GitFixedResults& git, Operations operations,
    Specification specification)
    -> std::expected<MonitoringGovernedPlanInputCore, MonitoringQ1GitCommandFailure> {
    std::optional<DiagnosticStock> diagnostic_storage;
    try { diagnostic_storage.emplace(make_diagnostic_stock(
        specification.stable_code, specification.message, specification.operation)); }
    catch (...) { return std::unexpected{bootstrap_failure()}; }
    auto& diagnostics=*diagnostic_storage;
    std::array<int, 4> fds{-1, -1, -1, -1};
    const auto reject = [&](std::string classification, int native = 0) {
        auto result=take(diagnostics,classification);
        if(native!=0)try{result.primary.os_error=core::OsError::capture(
          std::error_code{native,std::generic_category()});}catch(...){result=take(diagnostics,"contained_failure");}
        const auto close_errors = close_all(fds, operations);
        for (std::size_t index{}; index < close_errors.count; ++index) {
            diagnostics.cleanup[index].context["native_errno"]=close_errors.values[index];
            result.cleanup.push_back(std::move(diagnostics.cleanup[index]));
        }
        return std::expected<MonitoringGovernedPlanInputCore, MonitoringQ1GitCommandFailure>{
            std::unexpected{std::move(result)}};
    };
    if (!operations.open_at || !operations.stat_fd || !operations.stat_at || !operations.read_at ||
        !operations.close_fd || !operations.effective_user || !operations.fault)
        return reject("operations");
    try {
    operations.fault(operations.context, 0);
    if (git.records.size() != specification.record_count ||
        git.records[specification.record_index].path_ != specification.tracked_path)
        return reject("tracked_entry");
    if (specification.require_exact_record_order)
        for (std::size_t index{}; index < specification.record_count; ++index)
            if (git.records[index].path_ != specification.exact_record_paths[index])
                return reject("tracked_entry");
    fds[0] = operations.open_at(operations.context, root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fds[0] < 0)
        return reject("root", errno);
    fds[1] = operations.open_at(operations.context, fds[0], "config", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fds[1] < 0)
        return reject("config", errno);
    fds[2] = operations.open_at(operations.context, fds[1], "monitoring-qualification",
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fds[2] < 0)
        return reject("config", errno);
    operations.fault(operations.context, 1);
    fds[3] = operations.open_at(operations.context, fds[2], specification.leaf.data(),
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fds[3] < 0)
        return reject("plan", errno);
    std::array<MonitoringGovernedPlanInputIdentity, 4> identities{};
    const auto euid = operations.effective_user(operations.context);
    for (std::size_t i{}; i < 4U; ++i) {
        auto value = stat_checked(fds[i], euid, i != 3U, specification.leaf_mode, operations);
        if (!value)
            return reject("identity");
        identities[i] = *value;
    }
    std::string bytes(static_cast<std::size_t>(identities[3].size), '\0');
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto got = operations.read_at(operations.context, fds[3], bytes.data() + offset, bytes.size() - offset,
                                 static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return reject("read", got < 0 ? errno : EIO);
        offset += static_cast<std::size_t>(got);
    }
    char extra{};
    ssize_t trailing{};
    do {
        trailing = operations.read_at(operations.context, fds[3], &extra, 1, static_cast<off_t>(offset));
    } while (trailing < 0 && errno == EINTR);
    if (trailing != 0)
        return reject("growth", trailing < 0 ? errno : EFBIG);
    if (bytes != git.records[specification.record_index].bytes_)
        return reject("tracked_entry");
    const std::array final_names{".","config","monitoring-qualification",
                                 specification.leaf.data()};
    for(std::size_t i{};i<fds.size();++i){struct stat held{},named{};
      const int parent=i==0?fds[0]:fds[i<3?i-1:2];
      if(operations.stat_fd(operations.context,fds[i],&held)!=0||
         operations.stat_at(operations.context,parent,final_names[i],&named,AT_SYMLINK_NOFOLLOW)!=0)
        return reject("mutation",errno);
      if(identity(held)!=identities[i]||identity(named)!=identities[i])return reject("mutation");}
    auto projection = specification.decode(bytes);
    if (!projection) {auto rejected=reject("plan");if(auto*outer=std::get_if<core::Error>(&projection.error()))rejected.error().primary=std::move(*outer);else rejected.error().governed_authored=std::move(std::get<MonitoringGovernedPlanAuthoredDiagnostic>(projection.error()));return rejected;}
    auto retirement=take(diagnostics,"cleanup_failure");
    auto retirement_cleanup=std::move(diagnostics.cleanup);
    return MonitoringGovernedPlanInputCore{
        fds, identities, std::move(bytes),
        core::sha256(git.records[specification.record_index].bytes_), git.head_, git.frame_,
        std::move(*projection), operations, specification, std::move(retirement),
        std::move(retirement_cleanup)};
    } catch (...) {
        auto contained=take(diagnostics,"contained_failure");
        const auto close_errors=close_all(fds,operations);
        for(std::size_t index{};index<close_errors.count;++index) {
            diagnostics.cleanup[index].context["native_errno"]=close_errors.values[index];
            contained.cleanup.push_back(std::move(diagnostics.cleanup[index]));
        }
        return std::unexpected{std::move(contained)};
    }
}
auto MonitoringGovernedPlanInputCore::recheck() const
    -> std::expected<bool, MonitoringQ1GitCommandFailure> {
    if (!active_) return std::unexpected{bootstrap_failure()};
    try {
    if (operations_.recheck_failure) {
        auto injected = operations_.recheck_failure(operations_.context);
        if (injected) return std::unexpected{std::move(*injected)};
    }
    const std::array names{".", "config", "monitoring-qualification",
                           specification_.leaf.data()};
    for (std::size_t i{}; i < descriptors_.size(); ++i) {
        auto held = stat_checked(descriptors_[i], operations_.effective_user(operations_.context),
                                 i != 3U, specification_.leaf_mode, operations_);
        struct stat named{};
        const int parent = i == 0U ? descriptors_[0] : descriptors_[i < 3U ? i - 1U : 2U];
        if (!held || operations_.stat_at(operations_.context, parent, names[i], &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            *held != identities_[i] || identity(named) != identities_[i])
            return false;
    }
    std::string repeated(bytes_.size(), '\0');
    std::size_t offset{};
    while (offset < repeated.size()) {
        const auto got = operations_.read_at(operations_.context, descriptors_[3], repeated.data() + offset,
                                 repeated.size() - offset, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return std::unexpected{failure(specification_.stable_code, specification_.message,
                                           specification_.operation, "authority_changed",
                                           got < 0 ? errno : EIO)};
        offset += static_cast<std::size_t>(got);
    }
    char extra{};
    ssize_t trailing{};
    do { trailing = operations_.read_at(operations_.context, descriptors_[3], &extra, 1, static_cast<off_t>(offset)); }
    while (trailing < 0 && errno == EINTR);
    if (trailing != 0 || repeated != bytes_)
        return false;
    const std::array final_names{".","config","monitoring-qualification",
                                 specification_.leaf.data()};
    for(std::size_t i{};i<descriptors_.size();++i){struct stat held{},named{};
      const int parent=i==0?descriptors_[0]:descriptors_[i<3?i-1:2];
      if(operations_.stat_fd(operations_.context,descriptors_[i],&held)!=0||
         operations_.stat_at(operations_.context,parent,final_names[i],&named,AT_SYMLINK_NOFOLLOW)!=0)
        return std::unexpected{failure(specification_.stable_code, specification_.message,
                                       specification_.operation, "authority_changed", errno)};
      if(identity(held)!=identities_[i]||identity(named)!=identities_[i])return false;}
    return true;
    } catch (...) { return std::unexpected{bootstrap_failure()}; }
}
auto MonitoringGovernedPlanInputCore::retire()
    -> std::expected<void, MonitoringQ1GitCommandFailure> {
    if (!active_) return std::unexpected{bootstrap_failure()};
    auto result=std::move(retirement_failure_);
    active_=false;
    const auto close_errors = close_all(descriptors_, operations_);
    if (close_errors.count != 0U) {
        result.primary.context["native_errno"]=close_errors.values[0];
        for (std::size_t index = 1; index < close_errors.count; ++index)
        { retirement_cleanup_[index].context["native_errno"]=close_errors.values[index];
          result.cleanup.push_back(std::move(retirement_cleanup_[index])); }
        return std::unexpected{std::move(result)};
    }
    return {};
}

} // namespace epycbench::monitoring::qualification::detail
