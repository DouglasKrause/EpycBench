#include "platform_linux/authenticated_monitoring_capacity_authority.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <new>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux::detail {
namespace {
constexpr auto path = "/sys/devices/system/cpu/kernel_max";
constexpr std::string_view scenario_provenance_domain =
    "epycbench.linux-monitoring-capacity-provenance/scenario/v1";
constexpr std::string_view host_plan_provenance_domain =
    "epycbench.linux-monitoring-capacity-provenance/host-plan/v1";
constexpr std::size_t maximum_provenance_domain_size =
    std::max(scenario_provenance_domain.size(), host_plan_provenance_domain.size());

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}
void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}
void append_text(std::vector<std::byte>& out, std::string_view text) {
    append_u32(out, static_cast<std::uint32_t>(text.size()));
    for (const char value : text)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
}
void append_digest(std::vector<std::byte>& out, const core::Sha256Digest& digest) {
    out.insert(out.end(), digest.bytes().begin(), digest.bytes().end());
}
auto provenance_digest(std::string_view domain, const core::Sha256Digest& first,
                       const core::Sha256Digest& second) noexcept -> core::Sha256Digest {
    std::array<std::byte, 4U + maximum_provenance_domain_size + 64U> bytes{};
    std::size_t cursor{};
    const auto size = static_cast<std::uint32_t>(domain.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes[cursor++] = static_cast<std::byte>((size >> shift) & 0xffU);
    for (const char value : domain)
        bytes[cursor++] = static_cast<std::byte>(static_cast<unsigned char>(value));
    for (const auto value : first.bytes()) bytes[cursor++] = value;
    for (const auto value : second.bytes()) bytes[cursor++] = value;
    return core::sha256(std::span<const std::byte>{bytes}.first(cursor));
}
auto fail(LinuxMonitoringCapacityFailureKind kind, int native = 0,
          LinuxMonitoringCapacityReason reason = LinuxMonitoringCapacityReason::none) noexcept {
    return LinuxMonitoringCapacityFailure{kind, native, false, std::nullopt, reason};
}
auto native_open(void*, const char* value, int flags) noexcept -> int {
    return ::open(value, flags);
}
auto native_stat_fd(void*, int descriptor, struct stat* value) noexcept -> int {
    return ::fstat(descriptor, value);
}
auto native_stat_named(void*, const char* value, struct stat* status) noexcept -> int {
    return ::fstatat(AT_FDCWD, value, status, AT_SYMLINK_NOFOLLOW);
}
auto native_read(void*, int descriptor, void* bytes, std::size_t count,
                 std::int64_t offset) noexcept -> ssize_t {
    return ::pread(descriptor, bytes, count, static_cast<off_t>(offset));
}
auto native_close(void*, int descriptor) noexcept -> int { return ::close(descriptor); }
auto same(const struct stat& left, const struct stat& right) noexcept -> bool {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_uid == right.st_uid && left.st_mode == right.st_mode &&
           left.st_nlink == right.st_nlink;
}
} // namespace

struct AuthenticatedLinuxMonitoringCapacityAuthority::LocalBinding final {
    std::optional<core::Sha256Digest> identity;
};

auto LinuxMonitoringCapacityAuthorityConstruction::scenario_provenance(
    const core::Sha256Digest& scenario, const core::Sha256Digest& resolved)
    noexcept -> LinuxMonitoringCapacityProvenance {
    return {LinuxMonitoringCapacityProvenanceKind::scenario,
            provenance_digest(
                scenario_provenance_domain,
                scenario, resolved)};
}
auto LinuxMonitoringCapacityAuthorityConstruction::host_plan_provenance(
    const core::Sha256Digest& environment, const core::Sha256Digest& plan)
    noexcept -> LinuxMonitoringCapacityProvenance {
    return {LinuxMonitoringCapacityProvenanceKind::host_qualification_plan,
            provenance_digest(
                host_plan_provenance_domain,
                environment, plan)};
}
auto LinuxMonitoringCapacityAuthorityConstruction::host_facts(
    core::ByteCount page, std::vector<core::LinuxCpuId> cpus,
    topology::PhysicalTopologyFingerprint physical,
    topology::EffectiveTopologyFingerprint effective,
    LinuxMonitoringCapacityProvenance provenance) noexcept
    -> LinuxMonitoringCapacityHostFacts {
    return {page, std::move(cpus), std::move(physical), std::move(effective),
            std::move(provenance)};
}

auto LinuxMonitoringCapacityAuthorityConstruction::authenticate(
    LinuxMonitoringCapacityHostFacts&& facts) noexcept
    -> std::expected<AuthenticatedLinuxMonitoringCapacityAuthority,
                     LinuxMonitoringCapacityFailure> {
    return AuthenticatedLinuxMonitoringCapacityAuthority::authenticate_with(
        std::move(facts), {nullptr, native_open, native_stat_fd, native_stat_named,
                           native_read, native_close, nullptr});
}

auto LinuxMonitoringCapacityAuthorityConstruction::prepare(
    LinuxMonitoringCapacityHostFacts&& facts) noexcept
    -> std::expected<PreparedLinuxMonitoringCapacityAuthority,
                     LinuxMonitoringCapacityFailure> {
    return prepare_with(std::move(facts), {});
}

auto LinuxMonitoringCapacityAuthorityConstruction::prepare_with(
    LinuxMonitoringCapacityHostFacts&& facts, PreparationOperations operations) noexcept
    -> std::expected<PreparedLinuxMonitoringCapacityAuthority,
                     LinuxMonitoringCapacityFailure> {
    using Reason = LinuxMonitoringCapacityReason;
    bool binding_scope = false;
    try {
        if (facts.base_page_bytes_.value() == 0U)
            return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::invalid_host_facts, 0, Reason::prepare_page_zero)};
        if (facts.present_cpus_.empty())
            return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::invalid_host_facts, 0, Reason::prepare_cpus_empty)};
        if (!std::ranges::is_sorted(facts.present_cpus_))
            return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::invalid_host_facts, 0, Reason::prepare_cpus_order)};
        if (std::ranges::adjacent_find(facts.present_cpus_) != facts.present_cpus_.end())
            return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::invalid_host_facts, 0, Reason::prepare_cpus_duplicate)};
        constexpr std::size_t fixed = 4U + std::string_view{linux_monitoring_capacity_schema}.size() +
                                      4U + std::string_view{linux_monitoring_capacity_algorithm}.size() +
                                      8U + 4U + 8U + 32U + 32U + 1U + 32U;
        if (facts.present_cpus_.size() > (std::numeric_limits<std::size_t>::max() - fixed) / 4U)
            return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::arithmetic_overflow, 0, Reason::prepare_preimage_size)};
        if (operations.fault)
            operations.fault(operations.context, 0U);
        std::vector<std::byte> preimage;
        preimage.reserve(fixed + facts.present_cpus_.size() * 4U);
        binding_scope = true;
        if (operations.fault)
            operations.fault(operations.context, 1U);
        auto binding = std::make_shared<AuthenticatedLinuxMonitoringCapacityAuthority::LocalBinding>();
        return PreparedLinuxMonitoringCapacityAuthority{
            std::move(facts), std::move(preimage), std::move(binding)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::allocation)};
    } catch (const std::exception&) {
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::contained_exception, 0, binding_scope ? Reason::prepare_binding_exception : Reason::prepare_preimage_exception)};
    } catch (...) {
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::contained_unknown_failure, 0, binding_scope ? Reason::prepare_binding_unknown : Reason::prepare_preimage_unknown)};
    }
}

auto LinuxMonitoringCapacityAuthorityConstruction::seal(
    PreparedLinuxMonitoringCapacityAuthority&& prepared, std::uint32_t maximum) noexcept
    -> std::expected<AuthenticatedLinuxMonitoringCapacityAuthority,
                     LinuxMonitoringCapacityFailure> {
    using Reason = LinuxMonitoringCapacityReason;
    bool hash_scope = false;
    try {
        if (std::ranges::any_of(prepared.facts_.present_cpus_,
                                [maximum](core::LinuxCpuId cpu) {
                                    return cpu.value() > maximum;
                                }))
            return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::invalid_host_facts, 0, Reason::seal_cpu_range)};
        auto& preimage = prepared.preimage_;
        append_text(preimage, linux_monitoring_capacity_schema);
        append_text(preimage, linux_monitoring_capacity_algorithm);
        append_u64(preimage, prepared.facts_.base_page_bytes_.value());
        append_u32(preimage, maximum);
        append_u64(preimage, prepared.facts_.present_cpus_.size());
        for (const auto cpu : prepared.facts_.present_cpus_)
            append_u32(preimage, cpu.value());
        append_digest(preimage, prepared.facts_.physical_.digest());
        append_digest(preimage, prepared.facts_.effective_.digest());
        preimage.push_back(static_cast<std::byte>(prepared.facts_.provenance_.kind()));
        append_digest(preimage, prepared.facts_.provenance_.authority_digest());
        hash_scope = true;
        const auto identity = core::sha256(preimage);
        prepared.binding_->identity = identity;
        return AuthenticatedLinuxMonitoringCapacityAuthority{
            std::move(prepared.facts_), maximum, identity, std::move(prepared.binding_)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::allocation, 0, hash_scope ? Reason::seal_hash_allocation : Reason::seal_preimage_allocation)};
    } catch (const std::exception&) {
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::contained_exception, 0, hash_scope ? Reason::seal_hash_exception : Reason::seal_preimage_exception)};
    } catch (...) {
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::contained_unknown_failure, 0, hash_scope ? Reason::seal_hash_unknown : Reason::seal_preimage_unknown)};
    }
}

AuthenticatedLinuxMonitoringCapacityAuthority::AuthenticatedLinuxMonitoringCapacityAuthority(
    LinuxMonitoringCapacityHostFacts&& facts, std::uint32_t maximum,
    core::Sha256Digest identity, std::shared_ptr<const LocalBinding> binding) noexcept
    : base_page_bytes_{facts.base_page_bytes_}, kernel_maximum_cpu_{maximum},
      present_cpus_{std::move(facts.present_cpus_)}, physical_{std::move(facts.physical_)},
      effective_{std::move(facts.effective_)}, provenance_{std::move(facts.provenance_)},
      identity_{identity}, binding_{std::move(binding)} {}

auto AuthenticatedLinuxMonitoringCapacityAuthority::authentically_bound() const noexcept -> bool {
    return binding_ && binding_->identity && *binding_->identity == identity_;
}
auto AuthenticatedLinuxMonitoringCapacityAuthority::stable_equals(
    const AuthenticatedLinuxMonitoringCapacityAuthority& other) const noexcept -> bool {
    return authentically_bound() && other.authentically_bound() && identity_ == other.identity_ &&
           base_page_bytes_ == other.base_page_bytes_ &&
           kernel_maximum_cpu_ == other.kernel_maximum_cpu_ &&
           present_cpus_ == other.present_cpus_ && physical_ == other.physical_ &&
           effective_ == other.effective_ && provenance_ == other.provenance_;
}

auto AuthenticatedLinuxMonitoringCapacityAuthority::authenticate_with(
    LinuxMonitoringCapacityHostFacts&& facts, Operations operations) noexcept
    -> std::expected<AuthenticatedLinuxMonitoringCapacityAuthority,
                     LinuxMonitoringCapacityFailure> {
    if (operations.open == nullptr || operations.stat_fd == nullptr ||
        operations.stat_named == nullptr || operations.read == nullptr ||
        operations.close == nullptr)
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::invalid_host_facts)};
    try {
        auto prepared = LinuxMonitoringCapacityAuthorityConstruction::prepare(std::move(facts));
        if (!prepared)
            return std::unexpected{prepared.error()};
        if (operations.fault)
            operations.fault(operations.context, 0U);

        const int descriptor = operations.open(
            operations.context, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (descriptor < 0)
            return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::open, errno)};
        bool owned = true;
        const auto reject = [&](LinuxMonitoringCapacityFailure value) noexcept
            -> std::expected<AuthenticatedLinuxMonitoringCapacityAuthority,
                             LinuxMonitoringCapacityFailure> {
            if (owned) {
                owned = false;
                if (operations.close(operations.context, descriptor) != 0)
                    value.cleanup = LinuxMonitoringCapacityCleanup{
                        LinuxMonitoringCapacityFailureKind::close, errno, true};
            }
            return std::unexpected{value};
        };
        try {
            struct stat held{}, named{};
            if (operations.stat_fd(operations.context, descriptor, &held) != 0)
                return reject(fail(LinuxMonitoringCapacityFailureKind::held_stat, errno));
            if (operations.stat_named(operations.context, path, &named) != 0)
                return reject(fail(LinuxMonitoringCapacityFailureKind::named_stat, errno));
            if (!S_ISREG(held.st_mode) || held.st_nlink != 1 || !same(held, named))
                return reject(fail(LinuxMonitoringCapacityFailureKind::identity));
            std::array<char, 11> bytes{};
            std::size_t used{};
            while (used < bytes.size()) {
                const auto count = operations.read(operations.context, descriptor,
                                                   bytes.data() + used, bytes.size() - used,
                                                   static_cast<std::int64_t>(used));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count < 0)
                    return reject(fail(LinuxMonitoringCapacityFailureKind::read, errno));
                if (count == 0)
                    break;
                if (static_cast<std::size_t>(count) > bytes.size() - used)
                    return reject(fail(LinuxMonitoringCapacityFailureKind::read_contract));
                used += static_cast<std::size_t>(count);
            }
            char probe{};
            for (;;) {
                const auto count = operations.read(operations.context, descriptor, &probe, 1U,
                                                   static_cast<std::int64_t>(used));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count < 0)
                    return reject(fail(LinuxMonitoringCapacityFailureKind::read, errno));
                if (count > 1)
                    return reject(fail(LinuxMonitoringCapacityFailureKind::read_contract));
                if (count != 0)
                    return reject(fail(LinuxMonitoringCapacityFailureKind::byte_limit));
                break;
            }
            struct stat after{}, named_after{};
            if (operations.stat_fd(operations.context, descriptor, &after) != 0)
                return reject(fail(LinuxMonitoringCapacityFailureKind::held_stat, errno));
            if (operations.stat_named(operations.context, path, &named_after) != 0)
                return reject(fail(LinuxMonitoringCapacityFailureKind::named_stat, errno));
            if (!same(held, after) || !same(held, named_after))
                return reject(fail(LinuxMonitoringCapacityFailureKind::identity));
            if (used < 2U || bytes[used - 1U] != '\n')
                return reject(fail(LinuxMonitoringCapacityFailureKind::invalid_value));
            const std::string_view digits{bytes.data(), used - 1U};
            if ((digits.size() > 1U && digits.front() == '0') || digits.size() > 10U)
                return reject(fail(LinuxMonitoringCapacityFailureKind::invalid_value));
            std::uint64_t parsed{};
            const auto converted = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
            if (converted.ec != std::errc{} || converted.ptr != digits.data() + digits.size() ||
                parsed > std::numeric_limits<std::uint32_t>::max())
                return reject(fail(LinuxMonitoringCapacityFailureKind::invalid_value));
            const auto maximum = static_cast<std::uint32_t>(parsed);
            if (std::ranges::any_of(prepared->facts_.present_cpus_,
                                    [maximum](core::LinuxCpuId cpu) {
                                        return cpu.value() > maximum;
                                    }))
                return reject(fail(LinuxMonitoringCapacityFailureKind::invalid_host_facts));
            if (operations.fault)
                operations.fault(operations.context, 1U);
            owned = false;
            if (operations.close(operations.context, descriptor) != 0) {
                auto value = fail(LinuxMonitoringCapacityFailureKind::close, errno);
                value.primary_uncertain = true;
                return std::unexpected{value};
            }
            return LinuxMonitoringCapacityAuthorityConstruction::seal(std::move(*prepared), maximum);
        } catch (const std::bad_alloc&) {
            return reject(fail(LinuxMonitoringCapacityFailureKind::allocation));
        } catch (const std::exception&) {
            return reject(fail(LinuxMonitoringCapacityFailureKind::contained_exception));
        } catch (...) {
            return reject(fail(LinuxMonitoringCapacityFailureKind::contained_unknown_failure));
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::allocation)};
    } catch (const std::exception&) {
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::contained_exception)};
    } catch (...) {
        return std::unexpected{fail(LinuxMonitoringCapacityFailureKind::contained_unknown_failure)};
    }
}

} // namespace epycbench::platform_linux::detail
