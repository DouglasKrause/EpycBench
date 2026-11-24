#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view prefix_scan_u64_task_id = "task.prefix-scan.inclusive.u64/v1";
inline constexpr std::string_view prefix_scan_u64_scalar_implementation_id = "scalar-u64-prefix-scan/v1";
inline constexpr std::string_view prefix_scan_u64_grouped4_implementation_id = "grouped4-u64-prefix-scan/v1";
inline constexpr std::string_view prefix_scan_u64_generator_profile = sum_u64_generator_profile;
inline constexpr std::string_view prefix_scan_u64_correctness_profile = "epycbench.task.prefix-scan-u64.exact-output-reference/v1";
inline constexpr std::uint64_t prefix_scan_u64_maximum_count = UINT64_C(1) << 20U;
inline constexpr std::byte prefix_scan_u64_padding_sentinel{0xa5};

struct PrefixScanU64Work {
    TaskRepetitionCount completed_scans;
    core::WorkCount scanned_elements;
    auto operator==(const PrefixScanU64Work&) const -> bool = default;
};
class PrefixScanU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t element_count, std::uint64_t input_seed = 0U)
        -> std::expected<PrefixScanU64Config, core::Error>;
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return elements_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    // Per-role logical extent: one readonly input and one equally sized output.
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * elements_.value());
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / elements_.value();
    }
    [[nodiscard]] auto work(TaskRepetitionCount) const -> std::expected<PrefixScanU64Work, core::Error>;
    auto operator<=>(const PrefixScanU64Config&) const = default;
  private:
    PrefixScanU64Config(ElementCount elements, std::uint64_t seed) noexcept : elements_{elements}, seed_{seed} {}
    ElementCount elements_;
    std::uint64_t seed_;
};
class PrefixScanU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const PrefixScanU64Config& { return config_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& { return output_; }
    // Sum of all inclusive output prefixes, modulo 2^64, for one scan.
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept -> std::uint64_t {
        return answer_ * count.value();
    }
  private:
    PrefixScanU64Reference(PrefixScanU64Config config, core::Sha256Digest input,
        core::Sha256Digest output, std::uint64_t answer) noexcept
        : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    PrefixScanU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_prefix_scan_u64_reference(const PrefixScanU64Config&, std::span<const std::byte>, std::span<std::byte>)
        -> std::expected<PrefixScanU64Reference, core::Error>;
};
using PrefixScanU64Verification = XorU64Verification;

// Input word i is mix64/v1(seed+i), serialized LE, with wrapping seed arithmetic.
// Each repetition writes output[i] = sum(input[0..i]) modulo 2^64 from scratch;
// output from a previous repetition is never an input to the next scan.
[[nodiscard]] auto initialize_prefix_scan_u64_input(const PrefixScanU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
// Untimed canonical-input validation, independent weighted-input checksum and
// complete expected output/digest construction in disjoint writable storage.
// No borrowed storage is retained. Padding contributes neither input nor work.
[[nodiscard]] auto prepare_prefix_scan_u64_reference(const PrefixScanU64Config&,
    std::span<const std::byte>, std::span<std::byte>) -> std::expected<PrefixScanU64Reference, core::Error>;
[[nodiscard]] auto reset_prefix_scan_u64_output(const PrefixScanU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_prefix_scan_u64_output(const PrefixScanU64Reference&, std::span<const std::byte>)
    -> std::expected<PrefixScanU64Verification, core::Error>;
} // namespace epycbench::kernel
