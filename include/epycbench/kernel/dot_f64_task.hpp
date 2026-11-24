#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view dot_f64_task_id = "task.dot.f64.exact/v1";
inline constexpr std::string_view dot_f64_scalar_implementation_id = "scalar-f64-dot-single/v1";
inline constexpr std::string_view dot_f64_avx_implementation_id = "avx-f64-dot/v1";
inline constexpr std::string_view dot_f64_generator_profile =
    "epycbench.generator.mix64-dyadic-pairs-f64/v1";
inline constexpr std::string_view dot_f64_correctness_profile =
    "epycbench.task.dot-f64.exact-scaled-integer-reference/v1";
inline constexpr std::uint64_t dot_f64_maximum_elements = UINT64_C(1) << 20U;
inline constexpr std::uint64_t dot_f64_x_seed = 0U;
inline constexpr std::uint64_t dot_f64_y_seed = UINT64_C(0xd1b54a32d192ed03);

class DotF64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t element_count)
        -> std::expected<DotF64Config, core::Error>;
    // An element is one interleaved (x,y) pair, not one binary64 word.
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return elements_; }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount { return bytes_; }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t;
    auto operator<=>(const DotF64Config&) const = default;
  private:
    DotF64Config(ElementCount elements, core::ByteCount bytes) noexcept
        : elements_{elements}, bytes_{bytes} {}
    ElementCount elements_;
    core::ByteCount bytes_;
};

class DotF64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const DotF64Config& { return configuration_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto scaled_sum() const noexcept -> std::int64_t { return sum_; }
    // Canonical binary64 bits of scaled_sum()/2^30, with positive zero.
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept -> std::uint64_t {
        return answer_ * count.value();
    }
  private:
    DotF64Reference(DotF64Config configuration, core::Sha256Digest input,
                    std::int64_t sum, std::uint64_t answer) noexcept
        : configuration_{configuration}, input_{input}, sum_{sum}, answer_{answer} {}
    DotF64Config configuration_;
    core::Sha256Digest input_;
    std::int64_t sum_;
    std::uint64_t answer_;
    friend auto prepare_dot_f64_reference(const DotF64Config&, std::span<const std::byte>)
        -> std::expected<DotF64Reference, core::Error>;
    friend auto replay_dot_f64_reference(const DotF64Config&, core::ByteCount)
        -> std::expected<DotF64Reference, core::Error>;
};

// Logical layout: LE binary64 x0,y0,x1,y1,... . For stream seed s, the
// signed numerator is (mix64(s+i)&65535)-32768 and the value is numerator/2^15.
// The independent streams have the immutable seeds above. N<=2^20 bounds every
// product and arbitrary-order partial sum's scaled magnitude by 2^50; binary64
// multiplication and addition are exact. A task completes N products/additions.
// The host reference uses signed integer products and encodes the result bits;
// no host floating arithmetic or per-element temporary allocation participates.
[[nodiscard]] auto initialize_dot_f64_input(const DotF64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_dot_f64_reference(const DotF64Config&, std::span<const std::byte>)
    -> std::expected<DotF64Reference, core::Error>;
// Regenerate the logical pairs and answer for saved replay within a caller byte
// ceiling. A replay reference carries no evidence about mapped input contents.
[[nodiscard]] auto replay_dot_f64_reference(const DotF64Config&, core::ByteCount maximum_generated_bytes)
    -> std::expected<DotF64Reference, core::Error>;
} // namespace epycbench::kernel
