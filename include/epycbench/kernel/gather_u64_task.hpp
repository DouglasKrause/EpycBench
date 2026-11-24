#pragma once

#include "epycbench/kernel/task.hpp"

#include <bit>
#include <optional>

namespace epycbench::kernel {
inline constexpr std::string_view gather_cache_sets_u64_task_id = "task.gather.page-set.sum.u64/v1";
inline constexpr std::string_view gather_cache_sets_u64_single_implementation_id = "scalar-u64-gather-set-single/v1";
inline constexpr std::string_view gather_cache_sets_u64_four_implementation_id = "scalar-u64-gather-set-four/v1";
inline constexpr std::string_view gather_cache_sets_u64_generator_profile = "epycbench.generator.page-set-gather-u64/v1";
inline constexpr std::string_view gather_cache_sets_u64_correctness_profile = "epycbench.task.page-set-gather-u64.modulo-reference/v1";
inline constexpr std::string_view gather_matched_pages_u64_single_implementation_id = "scalar-u64-gather-pages-single/v1";
inline constexpr std::string_view gather_matched_pages_u64_four_implementation_id = "scalar-u64-gather-pages-four/v1";
inline constexpr std::string_view gather_matched_pages_u64_generator_profile = "epycbench.generator.matched-line-pages-gather-u64/v1";
inline constexpr std::string_view gather_u64_task_id = "task.gather.sum.u64/v1";
inline constexpr std::string_view gather_u64_single_implementation_id =
    "scalar-u64-gather-sum-single/v1";
inline constexpr std::string_view gather_u64_four_implementation_id =
    "scalar-u64-gather-sum-four/v1";
inline constexpr std::string_view gather_u64_generator_profile =
    "epycbench.generator.seeded-indexed-gather-u64/v1";
inline constexpr std::string_view gather_u64_correctness_profile =
    "epycbench.task.gather-sum-u64.modulo-reference/v1";
inline constexpr std::uint64_t gather_u64_maximum_count = UINT64_C(1) << 20U;
inline constexpr std::uint64_t gather_u64_header_bytes = 32U;
inline constexpr std::uint64_t gather_u64_input_magic = UINT64_C(0x3152485447425045);
inline constexpr std::uint64_t gather_u64_cluster_values = 64U;
inline constexpr std::uint64_t gather_u64_index_seed_xor = UINT64_C(0xd1b54a32d192ed03);
inline constexpr std::uint64_t gather_u64_cluster_seed_xor = UINT64_C(0x8cb92baa3f3d8dd7);

enum class GatherU64IndexPattern : std::uint8_t { clustered, spread, set_fixed, set_cycle, pages_compact, pages_spread };
inline constexpr std::uint64_t gather_set_page_bytes = 4096U;
inline constexpr std::uint64_t gather_set_line_bytes = 64U;
inline constexpr std::uint64_t gather_matched_page_lines = 256U;
inline constexpr std::uint64_t gather_matched_page_values = (256U * gather_set_page_bytes - gather_u64_header_bytes) / sizeof(std::uint64_t);
[[nodiscard]] constexpr auto valid_gather_matched_page_shape(std::uint64_t values, std::uint64_t indices) noexcept -> bool {
    return values == gather_matched_page_values && indices > 0U && indices <= gather_u64_maximum_count &&
           indices % gather_matched_page_lines == 0U;
}
[[nodiscard]] constexpr auto valid_gather_cache_set_table(std::uint64_t values) noexcept -> bool {
    if (values == 0U || values > gather_u64_maximum_count) return false;
    const auto bytes = gather_u64_header_bytes + sizeof(std::uint64_t) * values;
    return bytes % gather_set_page_bytes == 0U && std::has_single_bit(bytes / gather_set_page_bytes);
}
[[nodiscard]] auto gather_u64_index_pattern_token(GatherU64IndexPattern) noexcept
    -> std::string_view;
[[nodiscard]] auto parse_gather_u64_index_pattern(std::string_view) noexcept -> std::optional<GatherU64IndexPattern>;
struct GatherU64Work {
    TaskRepetitionCount completed_tasks;
    core::WorkCount indexed_lookups;
    auto operator==(const GatherU64Work&) const -> bool = default;
};
class GatherU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t value_count, std::uint64_t index_count,
                                     GatherU64IndexPattern, std::uint64_t input_seed = 0U)
        -> std::expected<GatherU64Config, core::Error>;
    [[nodiscard]] auto value_count() const noexcept -> ElementCount { return values_; }
    [[nodiscard]] auto index_count() const noexcept -> ElementCount { return indices_; }
    // Common task work is indexed lookups, not the size of the value table.
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return indices_; }
    [[nodiscard]] auto index_pattern() const noexcept -> GatherU64IndexPattern { return pattern_; }
    [[nodiscard]] auto matched_page_pattern() const noexcept -> bool {
        return pattern_ == GatherU64IndexPattern::pages_compact || pattern_ == GatherU64IndexPattern::pages_spread;
    }
    [[nodiscard]] auto cache_set_pattern() const noexcept -> bool {
        return matched_page_pattern() || pattern_ == GatherU64IndexPattern::set_fixed || pattern_ == GatherU64IndexPattern::set_cycle;
    }
    [[nodiscard]] auto generator_profile() const noexcept -> std::string_view {
        if (matched_page_pattern()) return gather_matched_pages_u64_generator_profile;
        return cache_set_pattern() ? gather_cache_sets_u64_generator_profile : gather_u64_generator_profile;
    }
    [[nodiscard]] auto correctness_profile() const noexcept -> std::string_view {
        return cache_set_pattern() ? gather_cache_sets_u64_correctness_profile : gather_u64_correctness_profile;
    }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(gather_u64_header_bytes +
                                                8U * (values_.value() + indices_.value()));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / indices_.value();
    }
    [[nodiscard]] auto work(TaskRepetitionCount) const -> std::expected<GatherU64Work, core::Error>;
    auto operator<=>(const GatherU64Config&) const = default;

  private:
    GatherU64Config(ElementCount values, ElementCount indices, GatherU64IndexPattern pattern,
                    std::uint64_t seed)
        : values_{values}, indices_{indices}, pattern_{pattern}, seed_{seed} {}
    ElementCount values_, indices_;
    GatherU64IndexPattern pattern_;
    std::uint64_t seed_;
};
[[nodiscard]] auto gather_u64_implementation_matches(const GatherU64Config&, std::string_view) noexcept -> bool;
class GatherU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const GatherU64Config& { return config_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& {
        return digest_;
    }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept
        -> std::uint64_t {
        return answer_ * count.value();
    }

  private:
    GatherU64Reference(GatherU64Config config, core::Sha256Digest digest, std::uint64_t answer)
        : config_{config}, digest_{digest}, answer_{answer} {}
    GatherU64Config config_;
    core::Sha256Digest digest_;
    std::uint64_t answer_;
    friend auto prepare_gather_u64_reference(const GatherU64Config&, std::span<const std::byte>)
        -> std::expected<GatherU64Reference, core::Error>;
};
// LE uint64 layout: magic,V,G,pattern(0 clustered/1 spread), V values,
// G indices. Value i=mix64(seed+i). Index stream j=mix64((seed^index_xor)+j).
// Spread takes stream%V; clustered takes base+stream%min(V,64), where
// base=mix64(seed^cluster_xor)%(V-min(V,64)+1). All seed arithmetic wraps.
// One task checks G index bounds, gathers their values and sums modulo 2^64.
// The same canonical bytes are used in every repetition and both strategies.
// Patterns are separate input identities, not an implicit within-Run change.
// Clustered is a logical contiguous window, not a cache-hit/locality claim.
// The one-worker input is readonly; no output mapping or FP17 policy is needed.
[[nodiscard]] auto initialize_gather_u64_input(const GatherU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
// Untimed full canonical prefix check and digest. The expected answer comes
// from generated indices/values, independently of the timed table-read loop.
// No borrowed spans are retained; allocation padding is neither input nor work.
[[nodiscard]] auto prepare_gather_u64_reference(const GatherU64Config&, std::span<const std::byte>)
    -> std::expected<GatherU64Reference, core::Error>;
} // namespace epycbench::kernel
