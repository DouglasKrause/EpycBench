#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view merge_u64_task_id = "task.merge.sorted-runs.u64/v1";
inline constexpr std::string_view merge_u64_scalar_implementation_id = "scalar-u64-merge/v1";
inline constexpr std::string_view merge_u64_run_copy_implementation_id = "scalar-u64-merge-run-copy/v1";
inline constexpr std::string_view merge_u64_generator_profile = "epycbench.task.merge-u64.bucket-jitter-pairs/v1";
inline constexpr std::string_view merge_u64_correctness_profile = "epycbench.task.merge-u64.exact-output-reference/v1";
inline constexpr std::uint64_t merge_u64_maximum_count = UINT64_C(1) << 20U;
inline constexpr std::byte merge_u64_padding_sentinel{0xa5};

enum class MergeU64InputPattern { interleaved, separated };
[[nodiscard]] constexpr auto merge_u64_input_pattern_token(MergeU64InputPattern pattern) noexcept
    -> std::string_view {
    switch (pattern) {
    case MergeU64InputPattern::interleaved: return "interleaved";
    case MergeU64InputPattern::separated: return "separated";
    }
    return {};
}
struct MergeU64Work {
    TaskRepetitionCount completed_merges;
    core::WorkCount merged_elements;
    auto operator==(const MergeU64Work&) const -> bool = default;
};
class MergeU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t element_count, MergeU64InputPattern,
                                     std::uint64_t input_seed = 0U)
        -> std::expected<MergeU64Config, core::Error>;
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return elements_; }
    [[nodiscard]] auto input_pattern() const noexcept -> MergeU64InputPattern { return pattern_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto midpoint() const noexcept -> std::size_t {
        return static_cast<std::size_t>(elements_.value() / 2U);
    }
    // Independent read-only input and writable output, each exactly 8*N bytes.
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * elements_.value());
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / elements_.value();
    }
    [[nodiscard]] auto work(TaskRepetitionCount) const -> std::expected<MergeU64Work, core::Error>;
    auto operator<=>(const MergeU64Config&) const = default;
  private:
    MergeU64Config(ElementCount elements, MergeU64InputPattern pattern, std::uint64_t seed) noexcept
        : elements_{elements}, pattern_{pattern}, seed_{seed} {}
    ElementCount elements_;
    MergeU64InputPattern pattern_;
    std::uint64_t seed_;
};
class MergeU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const MergeU64Config& { return config_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& { return output_; }
    // Order-sensitive sum((i+1)*output[i]) modulo2^64, folded once per repetition.
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept -> std::uint64_t {
        return answer_ * count.value();
    }
  private:
    MergeU64Reference(MergeU64Config config, core::Sha256Digest input,
                      core::Sha256Digest output, std::uint64_t answer) noexcept
        : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    MergeU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_merge_u64_reference(const MergeU64Config&, std::span<const std::byte>, std::span<std::byte>)
        -> std::expected<MergeU64Reference, core::Error>;
};
using MergeU64Verification = XorU64Verification;

// Let B=ceil(N/2), w=UINT64_MAX/B. Sorted word i is
// floor(i/2)*w + mix64/v1(seed+floor(i/2))%w, with seed addition wrapping.
// Thus duplicate pairs occupy strictly increasing unsigned buckets. Separated
// input holds sorted positions [0,mid),[mid,N). Interleaved input holds odd
// sorted positions first (mid words), then even positions. No header is stored.
// Canonical sorted-run validation is untimed. Normal ABI entries assume sorted
// runs; their bounds/progress remain safe for arbitrary well-sized input bytes.
[[nodiscard]] auto initialize_merge_u64_input(const MergeU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
// Validates canonical input before mutation. Builds the independently generated
// expected sorted stream in existing disjoint output scratch, then hashes it.
// Retains no spans/third N-sized buffer; padding is never logical task work.
[[nodiscard]] auto prepare_merge_u64_reference(const MergeU64Config&,
    std::span<const std::byte>, std::span<std::byte>) -> std::expected<MergeU64Reference, core::Error>;
[[nodiscard]] auto reset_merge_u64_output(const MergeU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_merge_u64_output(const MergeU64Reference&, std::span<const std::byte>)
    -> std::expected<MergeU64Verification, core::Error>;
} // namespace epycbench::kernel
