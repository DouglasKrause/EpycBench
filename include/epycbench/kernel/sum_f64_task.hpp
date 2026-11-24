#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view sum_f64_task_id = "task.sum.f64.exact/v1";
inline constexpr std::string_view sum_f64_scalar_implementation_id = "scalar-f64-sum-single/v1";
inline constexpr std::string_view sum_f64_avx_implementation_id = "avx-f64-sum/v1";
inline constexpr std::string_view sum_f64_generator_profile = "epycbench.generator.mix64-dyadic-f64/v1";
inline constexpr std::string_view sum_f64_correctness_profile =
    "epycbench.task.sum-f64.exact-scaled-integer-reference/v1";
inline constexpr std::uint64_t sum_f64_maximum_elements = UINT64_C(1) << 20U;

class SumF64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t element_count, std::uint64_t input_seed = 0U)
        -> std::expected<SumF64Config, core::Error>;
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return elements_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount { return bytes_; }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t;
    auto operator<=>(const SumF64Config&) const = default;
  private:
    SumF64Config(ElementCount elements, std::uint64_t seed, core::ByteCount bytes) noexcept
        : elements_{elements}, seed_{seed}, bytes_{bytes} {}
    ElementCount elements_;
    std::uint64_t seed_;
    core::ByteCount bytes_;
};

class SumF64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const SumF64Config& { return configuration_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto scaled_sum() const noexcept -> std::int64_t { return sum_; }
    // Canonical binary64 bits of scaled_sum()/2^15, with positive zero.
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept -> std::uint64_t {
        return answer_ * count.value();
    }
  private:
    SumF64Reference(SumF64Config configuration, core::Sha256Digest input,
                       std::int64_t sum, std::uint64_t answer) noexcept
        : configuration_{configuration}, input_{input}, sum_{sum}, answer_{answer} {}
    SumF64Config configuration_;
    core::Sha256Digest input_;
    std::int64_t sum_;
    std::uint64_t answer_;
    friend auto prepare_sum_f64_reference(const SumF64Config&, std::span<const std::byte>)
        -> std::expected<SumF64Reference, core::Error>;
};

// Each input is one LE binary64 value m/2^15, where m is the low 16 bits of
// mix64(seed + index) interpreted by subtracting 32768. The bound N<=2^20
// limits the absolute integer sum to 2^35. Every binary64 partial sum is
// exact for any reduction order. The reference sums signed integer numerators.
[[nodiscard]] auto initialize_sum_f64_input(const SumF64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_sum_f64_reference(const SumF64Config&, std::span<const std::byte>)
    -> std::expected<SumF64Reference, core::Error>;
} // namespace epycbench::kernel
