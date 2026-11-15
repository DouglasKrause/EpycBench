#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/quantity.hpp"
#include "epycbench/kernel/kernel_id.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string_view>
#include <variant>

namespace epycbench::kernel {

namespace detail {
struct KernelLeafAccess;
}

inline constexpr std::string_view pointer_cycle_seed_profile =
    "epycbench.pointer-cycle-seed.v1";

enum class PointerCycleSeedMode { explicit_value, derive_per_experiment };

class PointerCycleSeedIntent {
  public:
    [[nodiscard]] static auto explicit_value(std::uint64_t value) noexcept
        -> PointerCycleSeedIntent;
    [[nodiscard]] static auto derive_per_experiment() noexcept -> PointerCycleSeedIntent;
    [[nodiscard]] auto mode() const noexcept -> PointerCycleSeedMode;
    [[nodiscard]] auto value() const noexcept -> std::optional<std::uint64_t>;
    auto operator<=>(const PointerCycleSeedIntent&) const = default;

  private:
    PointerCycleSeedIntent(PointerCycleSeedMode mode,
                           std::optional<std::uint64_t> value) noexcept;

    PointerCycleSeedMode mode_;
    std::optional<std::uint64_t> value_;
};

class SequentialReadBandwidthConfig {
  public:
    [[nodiscard]] static auto create(core::ByteCount working_set_per_worker)
        -> std::expected<SequentialReadBandwidthConfig, core::Error>;
    [[nodiscard]] auto working_set_per_worker() const noexcept -> core::ByteCount;
    auto operator<=>(const SequentialReadBandwidthConfig&) const = default;

  private:
    friend struct detail::KernelLeafAccess;
    explicit SequentialReadBandwidthConfig(core::ByteCount working_set_per_worker) noexcept;
    core::ByteCount working_set_per_worker_;
};

class DependentLoadLatencyConfig {
  public:
    [[nodiscard]] static auto create(core::ByteCount working_set,
                                     PointerCycleSeedIntent pointer_cycle_seed)
        -> std::expected<DependentLoadLatencyConfig, core::Error>;
    [[nodiscard]] auto working_set() const noexcept -> core::ByteCount;
    [[nodiscard]] auto pointer_cycle_seed() const noexcept -> const PointerCycleSeedIntent&;
    auto operator<=>(const DependentLoadLatencyConfig&) const = default;

  private:
    friend struct detail::KernelLeafAccess;
    DependentLoadLatencyConfig(core::ByteCount working_set,
                               PointerCycleSeedIntent pointer_cycle_seed) noexcept;
    core::ByteCount working_set_;
    PointerCycleSeedIntent pointer_cycle_seed_;
};

using KernelConfiguration =
    std::variant<SequentialReadBandwidthConfig, DependentLoadLatencyConfig>;

[[nodiscard]] auto semantic_projection(KernelId id, const KernelConfiguration& configuration)
    -> std::expected<nlohmann::ordered_json, core::Error>;

} // namespace epycbench::kernel
