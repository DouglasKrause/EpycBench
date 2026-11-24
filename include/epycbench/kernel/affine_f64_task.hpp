#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view affine_f64_task_id = "task.map.affine32.f64/v1";
inline constexpr std::string_view affine_f64_scalar_implementation_id = "scalar-f64-affine32/v1";
inline constexpr std::string_view affine_f64_avx2_implementation_id = "avx2-f64-affine32/v1";
inline constexpr std::string_view affine_f64_generator_profile =
    "epycbench.generator.mix64-odd-dyadic-f64/v1";
inline constexpr std::string_view affine_f64_correctness_profile =
    "epycbench.task.affine32-f64.exact-output-reference/v1";
inline constexpr std::uint32_t affine_f64_steps = 32U;
inline constexpr std::byte affine_f64_padding_sentinel{0xa5};

[[nodiscard]] auto affine_f64_maximum_elements() noexcept -> std::uint64_t;

class AffineF64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t element_count, std::uint64_t input_seed = 0)
        -> std::expected<AffineF64Config, core::Error>;
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return elements_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount { return bytes_; }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t;
    auto operator<=>(const AffineF64Config&) const = default;
  private:
    AffineF64Config(ElementCount elements, std::uint64_t seed, core::ByteCount bytes) noexcept
        : elements_{elements}, seed_{seed}, bytes_{bytes} {}
    ElementCount elements_;
    std::uint64_t seed_;
    core::ByteCount bytes_;
};

class AffineF64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const AffineF64Config& { return configuration_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& { return output_; }
    // Observability checksum: modular sum of binary64 output bit patterns,
    // folded once per repetition. This is not a floating-point reduction.
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept -> std::uint64_t {
        return answer_ * count.value();
    }
  private:
    AffineF64Reference(AffineF64Config configuration, core::Sha256Digest input,
                       core::Sha256Digest output, std::uint64_t answer) noexcept
        : configuration_{configuration}, input_{input}, output_{output}, answer_{answer} {}
    AffineF64Config configuration_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_affine_f64_reference(const AffineF64Config&,
        std::span<const std::byte>, std::span<std::byte>)
        -> std::expected<AffineF64Reference, core::Error>;
};

// The existing carrier has only generic exact-output/padding/digest fields.
using AffineF64Verification = XorU64Verification;

[[nodiscard]] auto initialize_affine_f64_input(const AffineF64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
// m = 2*(mix64(seed+i)&0x7ffff)-524287, x=m/1024. Each output performs
// 32 dependent steps y=0.5*y+0.25, starting at x. Every intermediate is
// normal, nonzero and exactly representable as binary64. The independent
// reference encodes (2^41+m-512)/2^42 using integer arithmetic only.
// After step k the numerator is 2^(k+9)+m-512 over 2^(k+10).
// It stays odd and needs at most 42 bits for 1<=k<=32; thus cancellation,
// subnormals and rounding-mode differences do not enter this task's domain.
// Preparation validates input before writing expected bytes into the existing
// host output allocation as scratch. It retains no spans or third N-sized buffer.
[[nodiscard]] auto prepare_affine_f64_reference(const AffineF64Config&,
    std::span<const std::byte> input, std::span<std::byte> output)
    -> std::expected<AffineF64Reference, core::Error>;
[[nodiscard]] auto reset_affine_f64_output(const AffineF64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_affine_f64_output(const AffineF64Reference&, std::span<const std::byte>)
    -> std::expected<AffineF64Verification, core::Error>;

} // namespace epycbench::kernel
