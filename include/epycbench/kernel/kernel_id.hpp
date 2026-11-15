#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace epycbench::kernel {

enum class KernelFamily : std::uint8_t { bandwidth, latency, invalid };
inline constexpr std::size_t maximum_kernel_identity_bytes = 127;

namespace detail {
// IDs own their text so queued requests and saved evidence never borrow a
// module's storage. Their bounded representation needs no allocation.
struct KernelIdentityText {
    std::array<char, maximum_kernel_identity_bytes + 1> bytes{};
    std::uint8_t size{};

    constexpr KernelIdentityText() noexcept = default;
    constexpr explicit KernelIdentityText(std::string_view value) noexcept
        : size{static_cast<std::uint8_t>(value.size())} {
        for (std::size_t i{}; i < value.size(); ++i) bytes[i] = value[i];
    }
    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view {
        return {bytes.data(), size};
    }
    auto operator<=>(const KernelIdentityText&) const = default;
};
[[nodiscard]] auto valid_kernel_identity_token(std::string_view) noexcept -> bool;
} // namespace detail

class KernelId {
  public:
    static const KernelId sequential_read_bandwidth_v1;
    static const KernelId dependent_load_latency_v1;

    constexpr KernelId() noexcept : KernelId{0U} {}
    // Retained numeric records use 0 and 1. Other numbers create an invalid ID.
    constexpr explicit KernelId(std::uint64_t legacy) noexcept {
        if (legacy == 0U) {
            family_ = KernelFamily::bandwidth;
            text_ = detail::KernelIdentityText{"memory.bandwidth.read/v1"};
        } else if (legacy == 1U) {
            family_ = KernelFamily::latency;
            text_ = detail::KernelIdentityText{"memory.latency.dependent-load/v1"};
        }
    }
    [[nodiscard]] constexpr auto family() const noexcept -> KernelFamily { return family_; }
    [[nodiscard]] constexpr auto token() const noexcept -> std::string_view { return text_.view(); }
    auto operator<=>(const KernelId&) const = default;

  private:
    constexpr KernelId(std::string_view token, KernelFamily family) noexcept
        : family_{family}, text_{token} {}
    KernelFamily family_{KernelFamily::invalid};
    detail::KernelIdentityText text_;
    friend auto parse_kernel_id(std::string_view) noexcept -> std::optional<KernelId>;
};

inline constexpr KernelId KernelId::sequential_read_bandwidth_v1{0U};
inline constexpr KernelId KernelId::dependent_load_latency_v1{1U};

[[nodiscard]] auto kernel_id_token(const KernelId& id) noexcept -> std::string_view;
[[nodiscard]] auto parse_kernel_id(std::string_view token) noexcept -> std::optional<KernelId>;

} // namespace epycbench::kernel
