#pragma once

#include "epycbench/kernel/kernel_id.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::kernel {

class KernelImplementationId {
  public:
    static const KernelImplementationId scalar_u64_sequential_v1;
    static const KernelImplementationId scalar_u64_random_cycle_v1;

    constexpr KernelImplementationId() noexcept : KernelImplementationId{0U} {}
    constexpr explicit KernelImplementationId(std::uint64_t legacy) noexcept {
        if (legacy == 0U) {
            family_ = KernelFamily::bandwidth;
            text_ = detail::KernelIdentityText{"scalar-u64-sequential/v1"};
        } else if (legacy == 1U) {
            family_ = KernelFamily::latency;
            text_ = detail::KernelIdentityText{"scalar-u64-random-cycle/v1"};
        }
    }
    [[nodiscard]] constexpr auto family() const noexcept -> KernelFamily { return family_; }
    [[nodiscard]] constexpr auto token() const noexcept -> std::string_view { return text_.view(); }
    [[nodiscard]] constexpr auto legacy_index() const noexcept -> std::optional<unsigned> {
        if (family_ == KernelFamily::bandwidth && token() == "scalar-u64-sequential/v1") return 0U;
        if (family_ == KernelFamily::latency && token() == "scalar-u64-random-cycle/v1") return 1U;
        return std::nullopt;
    }
    // Only historical encoders use these conversions. New IDs must be saved as
    // text; the sentinel cannot masquerade as either built-in implementation.
    constexpr explicit operator unsigned() const noexcept {
        return legacy_index().value_or(std::numeric_limits<unsigned>::max());
    }
    constexpr explicit operator std::uint64_t() const noexcept {
        const auto index = legacy_index();
        return index ? *index : std::numeric_limits<std::uint64_t>::max();
    }
    auto operator<=>(const KernelImplementationId&) const = default;

  private:
    constexpr KernelImplementationId(std::string_view token, KernelFamily family) noexcept
        : family_{family}, text_{token} {}
    KernelFamily family_{KernelFamily::invalid};
    detail::KernelIdentityText text_;
    friend auto parse_kernel_implementation_id(std::string_view, KernelFamily) noexcept
        -> std::optional<KernelImplementationId>;
};

inline constexpr KernelImplementationId KernelImplementationId::scalar_u64_sequential_v1{0U};
inline constexpr KernelImplementationId KernelImplementationId::scalar_u64_random_cycle_v1{1U};

[[nodiscard]] auto kernel_implementation_id_token(const KernelImplementationId& id) noexcept
    -> std::string_view;
[[nodiscard]] auto parse_kernel_implementation_id(std::string_view, KernelFamily) noexcept
    -> std::optional<KernelImplementationId>;
[[nodiscard]] auto parse_kernel_implementation_id(std::string_view) noexcept
    -> std::optional<KernelImplementationId>;
// Retain the original compiled-enum spellings in existing wire records.
[[nodiscard]] auto kernel_implementation_wire_token(const KernelImplementationId&) noexcept
    -> std::string_view;
[[nodiscard]] auto parse_kernel_implementation_wire_id(std::string_view, KernelFamily) noexcept
    -> std::optional<KernelImplementationId>;

struct ModuleIdentity {
    std::string version;
    std::string build_info;
    std::string file_sha256;
    auto operator==(const ModuleIdentity&) const -> bool = default;
};

class KernelDescriptor {
  public:
    KernelDescriptor(const KernelDescriptor&) = default;
    KernelDescriptor(KernelDescriptor&&) = default;
    auto operator=(const KernelDescriptor&) -> KernelDescriptor& = delete;
    auto operator=(KernelDescriptor&&) -> KernelDescriptor& = delete;
    ~KernelDescriptor() = default;

    [[nodiscard]] static auto create(KernelId id, KernelImplementationId implementation,
        std::optional<ModuleIdentity> module = {}) -> std::optional<KernelDescriptor> {
        if (id.family() == KernelFamily::invalid || id.family() != implementation.family())
            return std::nullopt;
        return KernelDescriptor{id, implementation, std::move(module)};
    }
    [[nodiscard]] auto kernel_id() const noexcept -> const KernelId& { return kernel_id_; }
    [[nodiscard]] auto implementation_id() const noexcept -> const KernelImplementationId& {
        return implementation_id_;
    }
    [[nodiscard]] auto module_identity() const noexcept -> const std::optional<ModuleIdentity>& {
        return module_;
    }

  private:
    KernelDescriptor(KernelId id, KernelImplementationId implementation,
        std::optional<ModuleIdentity> module)
        : kernel_id_{id}, implementation_id_{implementation}, module_{std::move(module)} {}

    const KernelId kernel_id_;
    const KernelImplementationId implementation_id_;
    const std::optional<ModuleIdentity> module_;
};

} // namespace epycbench::kernel
