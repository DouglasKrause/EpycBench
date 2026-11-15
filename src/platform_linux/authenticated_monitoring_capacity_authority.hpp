#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/core/quantity.hpp"
#include "epycbench/topology/fingerprint.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <sys/stat.h>
#include <vector>

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4MonitoringCapacityAdapter;
}
namespace epycbench::runtime::detail {
struct PairwiseMonitoringHostCapacityAdmissionAccess;
}

namespace epycbench::platform_linux::detail {

struct LinuxMonitoringCapacityAuthorityConstruction;
struct LinuxMonitoringCapacityAuthorityTestAccess;
class LinuxMonitoringCapacityLocalBindingSeal;

inline constexpr auto linux_monitoring_capacity_schema =
    "epycbench.linux-monitoring-capacity-authority/v1";
inline constexpr auto linux_monitoring_capacity_algorithm =
    "linux.kernel-max+authenticated-topology/bp-k-u/v1";

enum class LinuxMonitoringCapacityProvenanceKind : unsigned char {
    scenario = 1,
    host_qualification_plan = 2
};

class LinuxMonitoringCapacityProvenance final {
  public:
    [[nodiscard]] auto kind() const noexcept -> LinuxMonitoringCapacityProvenanceKind {
        return kind_;
    }
    [[nodiscard]] auto authority_digest() const noexcept -> const core::Sha256Digest& {
        return digest_;
    }
    auto operator<=>(const LinuxMonitoringCapacityProvenance&) const = default;

  private:
    LinuxMonitoringCapacityProvenance(LinuxMonitoringCapacityProvenanceKind kind,
                                      core::Sha256Digest digest) noexcept
        : kind_{kind}, digest_{digest} {}
    LinuxMonitoringCapacityProvenanceKind kind_;
    core::Sha256Digest digest_;
    friend struct LinuxMonitoringCapacityAuthorityConstruction;
    friend struct LinuxMonitoringCapacityAuthorityTestAccess;
};

class LinuxMonitoringCapacityHostFacts final {
  public:
    LinuxMonitoringCapacityHostFacts(const LinuxMonitoringCapacityHostFacts&) = delete;
    LinuxMonitoringCapacityHostFacts(LinuxMonitoringCapacityHostFacts&&) noexcept = default;
    auto operator=(const LinuxMonitoringCapacityHostFacts&)
        -> LinuxMonitoringCapacityHostFacts& = delete;
    auto operator=(LinuxMonitoringCapacityHostFacts&&)
        -> LinuxMonitoringCapacityHostFacts& = delete;

  private:
    LinuxMonitoringCapacityHostFacts(
        core::ByteCount base_page_bytes, std::vector<core::LinuxCpuId> present_cpus,
        topology::PhysicalTopologyFingerprint physical,
        topology::EffectiveTopologyFingerprint effective,
        LinuxMonitoringCapacityProvenance provenance) noexcept
        : base_page_bytes_{base_page_bytes}, present_cpus_{std::move(present_cpus)},
          physical_{std::move(physical)}, effective_{std::move(effective)},
          provenance_{std::move(provenance)} {}
    core::ByteCount base_page_bytes_;
    std::vector<core::LinuxCpuId> present_cpus_;
    topology::PhysicalTopologyFingerprint physical_;
    topology::EffectiveTopologyFingerprint effective_;
    LinuxMonitoringCapacityProvenance provenance_;
    friend class AuthenticatedLinuxMonitoringCapacityAuthority;
    friend struct LinuxMonitoringCapacityAuthorityConstruction;
    friend struct LinuxMonitoringCapacityAuthorityTestAccess;
};

enum class LinuxMonitoringCapacityFailureKind : unsigned char {
    invalid_host_facts,
    arithmetic_overflow,
    allocation,
    open,
    held_stat,
    named_stat,
    identity,
    read,
    read_contract,
    byte_limit,
    invalid_value,
    close,
    contained_exception,
    contained_unknown_failure,
    inactive
};

struct LinuxMonitoringCapacityCleanup final {
    LinuxMonitoringCapacityFailureKind kind{LinuxMonitoringCapacityFailureKind::close};
    int native_error{};
    bool uncertain{};
    auto operator<=>(const LinuxMonitoringCapacityCleanup&) const = default;
};

enum class LinuxMonitoringCapacityReason : std::uint16_t {
    none,
    prepare_binding_exception,
    prepare_binding_unknown,
    prepare_cpus_duplicate,
    prepare_cpus_empty,
    prepare_cpus_order,
    prepare_page_zero,
    prepare_preimage_exception,
    prepare_preimage_unknown,
    prepare_preimage_size,
    seal_cpu_range,
    seal_hash_allocation,
    seal_hash_exception,
    seal_hash_unknown,
    seal_preimage_allocation,
    seal_preimage_exception,
    seal_preimage_unknown,
};

struct LinuxMonitoringCapacityFailure final {
    LinuxMonitoringCapacityFailureKind primary{};
    int native_error{};
    bool primary_uncertain{};
    std::optional<LinuxMonitoringCapacityCleanup> cleanup{};
    LinuxMonitoringCapacityReason reason{LinuxMonitoringCapacityReason::none};
    auto operator<=>(const LinuxMonitoringCapacityFailure&) const = default;
};

class AuthenticatedLinuxMonitoringCapacityAuthority final {
  public:
    AuthenticatedLinuxMonitoringCapacityAuthority(
        const AuthenticatedLinuxMonitoringCapacityAuthority&) = delete;
    AuthenticatedLinuxMonitoringCapacityAuthority(
        AuthenticatedLinuxMonitoringCapacityAuthority&&) noexcept = default;
    auto operator=(const AuthenticatedLinuxMonitoringCapacityAuthority&)
        -> AuthenticatedLinuxMonitoringCapacityAuthority& = delete;
    auto operator=(AuthenticatedLinuxMonitoringCapacityAuthority&&)
        -> AuthenticatedLinuxMonitoringCapacityAuthority& = delete;

    [[nodiscard]] auto base_page_bytes() const noexcept -> core::ByteCount {
        return base_page_bytes_;
    }
    [[nodiscard]] auto kernel_maximum_cpu() const noexcept -> std::uint32_t {
        return kernel_maximum_cpu_;
    }
    [[nodiscard]] auto present_cpus() const noexcept -> std::span<const core::LinuxCpuId> {
        return present_cpus_;
    }
    [[nodiscard]] auto physical_fingerprint() const noexcept
        -> const topology::PhysicalTopologyFingerprint& { return physical_; }
    [[nodiscard]] auto effective_fingerprint() const noexcept
        -> const topology::EffectiveTopologyFingerprint& { return effective_; }
    [[nodiscard]] auto provenance() const noexcept
        -> const LinuxMonitoringCapacityProvenance& { return provenance_; }
    [[nodiscard]] auto identity() const noexcept -> const core::Sha256Digest& { return identity_; }
    [[nodiscard]] auto authentically_bound() const noexcept -> bool;
    [[nodiscard]] auto stable_equals(
        const AuthenticatedLinuxMonitoringCapacityAuthority&) const noexcept -> bool;

    struct Operations final {
        void* context{};
        int (*open)(void*, const char*, int) noexcept{};
        int (*stat_fd)(void*, int, struct stat*) noexcept{};
        int (*stat_named)(void*, const char*, struct stat*) noexcept{};
        ssize_t (*read)(void*, int, void*, std::size_t, std::int64_t) noexcept{};
        int (*close)(void*, int) noexcept{};
        void (*fault)(void*, unsigned){};
    };

  private:
    struct LocalBinding;
    AuthenticatedLinuxMonitoringCapacityAuthority(
        LinuxMonitoringCapacityHostFacts&&, std::uint32_t, core::Sha256Digest,
        std::shared_ptr<const LocalBinding>) noexcept;
    static auto authenticate_with(LinuxMonitoringCapacityHostFacts&&, Operations) noexcept
        -> std::expected<AuthenticatedLinuxMonitoringCapacityAuthority,
                         LinuxMonitoringCapacityFailure>;
    core::ByteCount base_page_bytes_;
    std::uint32_t kernel_maximum_cpu_{};
    std::vector<core::LinuxCpuId> present_cpus_;
    topology::PhysicalTopologyFingerprint physical_;
    topology::EffectiveTopologyFingerprint effective_;
    LinuxMonitoringCapacityProvenance provenance_;
    core::Sha256Digest identity_;
    std::shared_ptr<const LocalBinding> binding_;
    friend struct LinuxMonitoringCapacityAuthorityConstruction;
    friend struct LinuxMonitoringCapacityAuthorityTestAccess;
    friend class PreparedLinuxMonitoringCapacityAuthority;
    friend class LinuxMonitoringCapacityLocalBindingSeal;
};

class LinuxMonitoringCapacityLocalBindingSeal final {
  public:
    LinuxMonitoringCapacityLocalBindingSeal(
        const LinuxMonitoringCapacityLocalBindingSeal&) = delete;
    LinuxMonitoringCapacityLocalBindingSeal(
        LinuxMonitoringCapacityLocalBindingSeal&&) noexcept = default;
    auto operator=(const LinuxMonitoringCapacityLocalBindingSeal&)
        -> LinuxMonitoringCapacityLocalBindingSeal& = delete;
    auto operator=(LinuxMonitoringCapacityLocalBindingSeal&&)
        -> LinuxMonitoringCapacityLocalBindingSeal& = delete;

  private:
    explicit LinuxMonitoringCapacityLocalBindingSeal(
        std::shared_ptr<const AuthenticatedLinuxMonitoringCapacityAuthority::LocalBinding>
            binding) noexcept
        : binding_{std::move(binding)} {}
    std::shared_ptr<const AuthenticatedLinuxMonitoringCapacityAuthority::LocalBinding> binding_;
    friend struct LinuxMonitoringCapacityAuthorityConstruction;
};

class PreparedLinuxMonitoringCapacityAuthority final {
  public:
    PreparedLinuxMonitoringCapacityAuthority(
        const PreparedLinuxMonitoringCapacityAuthority&) = delete;
    PreparedLinuxMonitoringCapacityAuthority(
        PreparedLinuxMonitoringCapacityAuthority&&) noexcept = default;
    auto operator=(const PreparedLinuxMonitoringCapacityAuthority&)
        -> PreparedLinuxMonitoringCapacityAuthority& = delete;
    auto operator=(PreparedLinuxMonitoringCapacityAuthority&&)
        -> PreparedLinuxMonitoringCapacityAuthority& = delete;

  private:
    PreparedLinuxMonitoringCapacityAuthority(
        LinuxMonitoringCapacityHostFacts facts, std::vector<std::byte> preimage,
        std::shared_ptr<AuthenticatedLinuxMonitoringCapacityAuthority::LocalBinding> binding)
        noexcept
        : facts_{std::move(facts)}, preimage_{std::move(preimage)},
          binding_{std::move(binding)} {}
    LinuxMonitoringCapacityHostFacts facts_;
    std::vector<std::byte> preimage_;
    std::shared_ptr<AuthenticatedLinuxMonitoringCapacityAuthority::LocalBinding> binding_;
    friend struct LinuxMonitoringCapacityAuthorityConstruction;
    friend struct LinuxMonitoringCapacityAuthorityTestAccess;
    friend class AuthenticatedLinuxMonitoringCapacityAuthority;
};

struct LinuxMonitoringCapacityAuthorityConstruction final {
  private:
    struct PreparationOperations final {
        void* context{};
        void (*fault)(void*, unsigned){};
    };
    [[nodiscard]] static auto scenario_provenance(
        const core::Sha256Digest&, const core::Sha256Digest&) noexcept
        -> LinuxMonitoringCapacityProvenance;
    [[nodiscard]] static auto host_plan_provenance(
        const core::Sha256Digest&, const core::Sha256Digest&) noexcept
        -> LinuxMonitoringCapacityProvenance;
    [[nodiscard]] static auto host_facts(
        core::ByteCount, std::vector<core::LinuxCpuId>,
        topology::PhysicalTopologyFingerprint, topology::EffectiveTopologyFingerprint,
        LinuxMonitoringCapacityProvenance) noexcept -> LinuxMonitoringCapacityHostFacts;
    [[nodiscard]] static auto authenticate(LinuxMonitoringCapacityHostFacts&&) noexcept
        -> std::expected<AuthenticatedLinuxMonitoringCapacityAuthority,
                         LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto authenticate_with(
        LinuxMonitoringCapacityHostFacts&& facts,
        AuthenticatedLinuxMonitoringCapacityAuthority::Operations operations) noexcept {
        return AuthenticatedLinuxMonitoringCapacityAuthority::authenticate_with(
            std::move(facts), operations);
    }
    [[nodiscard]] static auto prepare(LinuxMonitoringCapacityHostFacts&&) noexcept
        -> std::expected<PreparedLinuxMonitoringCapacityAuthority,
                         LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto prepare_with(LinuxMonitoringCapacityHostFacts&&,
                                           PreparationOperations) noexcept
        -> std::expected<PreparedLinuxMonitoringCapacityAuthority,
                         LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto seal(PreparedLinuxMonitoringCapacityAuthority&&,
                                   std::uint32_t kernel_maximum_cpu) noexcept
        -> std::expected<AuthenticatedLinuxMonitoringCapacityAuthority,
                         LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto binding_seal(
        const AuthenticatedLinuxMonitoringCapacityAuthority& authority) noexcept
        -> LinuxMonitoringCapacityLocalBindingSeal {
        return LinuxMonitoringCapacityLocalBindingSeal{authority.binding_};
    }
    [[nodiscard]] static auto binding_matches(
        const LinuxMonitoringCapacityLocalBindingSeal& seal,
        const AuthenticatedLinuxMonitoringCapacityAuthority& authority) noexcept -> bool {
        return seal.binding_ && seal.binding_ == authority.binding_;
    }
    friend class ::epycbench::monitoring::qualification::detail::MonitoringQ4MonitoringCapacityAdapter;
    friend struct ::epycbench::runtime::detail::PairwiseMonitoringHostCapacityAdmissionAccess;
    friend class AuthenticatedLinuxMonitoringCapacityAuthority;
    friend struct LinuxMonitoringCapacityAuthorityTestAccess;
};

} // namespace epycbench::platform_linux::detail
