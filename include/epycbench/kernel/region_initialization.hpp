#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/quantity.hpp"
#include "epycbench/kernel/configuration.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace epycbench::kernel {

namespace detail {
struct RegionInitializationAccess;
}

inline constexpr std::string_view region_generator_profile =
    "epycbench.generator.mix64/v1";
inline constexpr std::string_view bandwidth_region_layout_profile =
    "epycbench.bandwidth-region-layout/v1";
inline constexpr std::string_view latency_region_layout_profile =
    "epycbench.latency-region-layout/v1";

enum class RegionContentFamily {
    sequential_read_bandwidth,
    dependent_load_latency,
};

class RegionInitializationSpec {
  public:
    [[nodiscard]] static auto bandwidth(SequentialReadBandwidthConfig configuration) noexcept
        -> RegionInitializationSpec;
    [[nodiscard]] static auto latency(DependentLoadLatencyConfig configuration,
                                      std::uint64_t resolved_permutation_seed) noexcept
        -> std::expected<RegionInitializationSpec, core::Error>;

    [[nodiscard]] auto family() const noexcept -> RegionContentFamily { return family_; }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return logical_bytes_;
    }
    [[nodiscard]] auto permutation_seed() const noexcept -> std::optional<std::uint64_t> {
        return permutation_seed_;
    }

  private:
    RegionInitializationSpec(RegionContentFamily family, core::ByteCount logical_bytes,
                             std::optional<std::uint64_t> permutation_seed) noexcept
        : family_{family}, logical_bytes_{logical_bytes}, permutation_seed_{permutation_seed} {}

    RegionContentFamily family_;
    core::ByteCount logical_bytes_;
    std::optional<std::uint64_t> permutation_seed_;

    friend struct detail::RegionInitializationAccess;
};

class RegionContentEvidence {
  public:
    [[nodiscard]] auto family() const noexcept -> RegionContentFamily { return family_; }
    [[nodiscard]] auto layout_profile() const noexcept -> std::string_view {
        return layout_profile_;
    }
    [[nodiscard]] auto generator_profile() const noexcept -> std::string_view {
        return region_generator_profile;
    }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return logical_bytes_;
    }
    [[nodiscard]] auto word_count() const noexcept -> std::optional<std::uint64_t> {
        return word_count_;
    }
    [[nodiscard]] auto node_count() const noexcept -> std::optional<std::uint64_t> {
        return node_count_;
    }
    [[nodiscard]] auto permutation_seed() const noexcept -> std::optional<std::uint64_t> {
        return permutation_seed_;
    }
    [[nodiscard]] auto structural_validation_completed() const noexcept -> bool {
        return structural_validation_completed_;
    }

  private:
    RegionContentEvidence(RegionContentFamily family, std::string_view layout_profile,
                          core::ByteCount logical_bytes,
                          std::optional<std::uint64_t> word_count,
                          std::optional<std::uint64_t> node_count,
                          std::optional<std::uint64_t> permutation_seed,
                          bool structural_validation_completed) noexcept
        : family_{family}, layout_profile_{layout_profile}, logical_bytes_{logical_bytes},
          word_count_{word_count}, node_count_{node_count},
          permutation_seed_{permutation_seed},
          structural_validation_completed_{structural_validation_completed} {}

    RegionContentFamily family_;
    std::string_view layout_profile_;
    core::ByteCount logical_bytes_;
    std::optional<std::uint64_t> word_count_;
    std::optional<std::uint64_t> node_count_;
    std::optional<std::uint64_t> permutation_seed_;
    bool structural_validation_completed_;

    friend struct detail::RegionInitializationAccess;
    friend class PreparedRegionInitializer;
};

class PreparedRegionInitializer {
  public:
    [[nodiscard]] static auto prepare(RegionInitializationSpec specification) noexcept
        -> std::expected<PreparedRegionInitializer, core::Error>;

    [[nodiscard]] auto initialize(std::span<std::byte> logical_region) const noexcept
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto evidence() const noexcept -> const RegionContentEvidence& {
        return evidence_;
    }

    PreparedRegionInitializer(PreparedRegionInitializer&&) noexcept = default;
    auto operator=(PreparedRegionInitializer&&) noexcept -> PreparedRegionInitializer& = default;
    PreparedRegionInitializer(const PreparedRegionInitializer&) = delete;
    auto operator=(const PreparedRegionInitializer&) -> PreparedRegionInitializer& = delete;
    ~PreparedRegionInitializer() noexcept = default;

  private:
    PreparedRegionInitializer(RegionContentEvidence evidence,
                              std::vector<std::uint64_t> successors,
                              core::Error span_size_failure,
                              core::Error alignment_failure,
                              core::Error invariant_failure) noexcept
        : evidence_{evidence}, successors_{std::move(successors)},
          span_size_failure_{std::move(span_size_failure)},
          alignment_failure_{std::move(alignment_failure)},
          invariant_failure_{std::move(invariant_failure)} {}

    RegionContentEvidence evidence_;
    std::vector<std::uint64_t> successors_;
    mutable core::Error span_size_failure_;
    mutable core::Error alignment_failure_;
    mutable core::Error invariant_failure_;

    friend struct detail::RegionInitializationAccess;
};

} // namespace epycbench::kernel
