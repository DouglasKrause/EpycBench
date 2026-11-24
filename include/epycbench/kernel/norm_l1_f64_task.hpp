#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view norm_l1_f64_task_id = "task.norm.l1.f64.exact/v1";
inline constexpr std::string_view norm_l1_f64_scalar_implementation_id = "scalar-f64-norm-l1-single/v1";
inline constexpr std::string_view norm_l1_f64_avx_implementation_id = "avx-f64-norm-l1/v1";
inline constexpr std::string_view norm_l1_f64_generator_profile = "epycbench.generator.mix64-dyadic-f64/v1";
inline constexpr std::string_view norm_l1_f64_correctness_profile =
    "epycbench.task.norm-l1-f64.exact-scaled-integer-reference/v1";
inline constexpr std::uint64_t norm_l1_f64_maximum_elements = UINT64_C(1) << 20U;

class NormL1F64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t element_count, std::uint64_t input_seed = 0U)
        -> std::expected<NormL1F64Config, core::Error>;
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return elements_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount { return bytes_; }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t;
    auto operator<=>(const NormL1F64Config&) const = default;
  private:
    NormL1F64Config(ElementCount elements, std::uint64_t seed, core::ByteCount bytes) noexcept
        : elements_{elements}, seed_{seed}, bytes_{bytes} {}
    ElementCount elements_;
    std::uint64_t seed_;
    core::ByteCount bytes_;
};

class NormL1F64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const NormL1F64Config& { return configuration_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto scaled_sum() const noexcept -> std::uint64_t { return sum_; }
    // Canonical binary64 bits of scaled_sum()/2^15, with positive zero.
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept -> std::uint64_t {
        return answer_ * count.value();
    }
  private:
    NormL1F64Reference(NormL1F64Config configuration, core::Sha256Digest input,
                       std::uint64_t sum, std::uint64_t answer) noexcept
        : configuration_{configuration}, input_{input}, sum_{sum}, answer_{answer} {}
    NormL1F64Config configuration_;
    core::Sha256Digest input_;
    std::uint64_t sum_;
    std::uint64_t answer_;
    friend auto prepare_norm_l1_f64_reference(const NormL1F64Config&, std::span<const std::byte>)
        -> std::expected<NormL1F64Reference, core::Error>;
};

// Each input is one LE binary64 value m/2^15, where m is the low 16 bits of
// mix64(seed + index) interpreted by subtracting 32768. The bound N<=2^20
// limits the absolute integer sum to 2^35. Every binary64 partial sum is
// exact for any reduction order. The reference sums integer magnitudes.
[[nodiscard]] auto initialize_norm_l1_f64_input(const NormL1F64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_norm_l1_f64_reference(const NormL1F64Config&, std::span<const std::byte>)
    -> std::expected<NormL1F64Reference, core::Error>;
} // namespace epycbench::kernel
