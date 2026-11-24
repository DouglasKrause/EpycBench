#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view gemm_u64_task_id = "task.matrix.gemm.u64/v1";
inline constexpr std::string_view gemm_u64_ijk_implementation_id = "scalar-u64-gemm-ijk/v1";
inline constexpr std::string_view gemm_u64_blocked_implementation_id = "blocked8-u64-gemm-ikj/v1";
inline constexpr std::string_view gemm_u64_generator_profile =
    "epycbench.generator.mix64-gemm-u64/v1";
inline constexpr std::string_view gemm_u64_correctness_profile =
    "epycbench.task.gemm-u64.exact-output-reference/v1";
inline constexpr std::uint64_t gemm_u64_maximum_dimension = 64U;
inline constexpr std::uint64_t gemm_u64_input_magic = UINT64_C(0x314d4d4547425045);
inline constexpr std::size_t gemm_u64_header_bytes = 32U;
inline constexpr std::byte gemm_u64_padding_sentinel{0xa5};

struct GemmU64Work {
    TaskRepetitionCount completed_tasks;
    core::WorkCount multiply_add_pairs;
    core::WorkCount output_elements;
    auto operator==(const GemmU64Work&) const -> bool = default;
};

class GemmU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t rows, std::uint64_t inner, std::uint64_t columns,
                                     std::uint64_t input_seed = 0U)
        -> std::expected<GemmU64Config, core::Error>;
    [[nodiscard]] auto rows() const noexcept -> std::uint64_t { return rows_; }
    [[nodiscard]] auto inner() const noexcept -> std::uint64_t { return inner_; }
    [[nodiscard]] auto columns() const noexcept -> std::uint64_t { return columns_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto input_bytes() const noexcept -> core::ByteCount;
    [[nodiscard]] auto output_bytes() const noexcept -> core::ByteCount;
    [[nodiscard]] auto native_input_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto native_output_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto output_elements() const noexcept -> ElementCount;
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t;
    [[nodiscard]] auto work(TaskRepetitionCount) const -> std::expected<GemmU64Work, core::Error>;
    auto operator<=>(const GemmU64Config&) const = default;

  private:
    GemmU64Config(std::uint64_t rows, std::uint64_t inner, std::uint64_t columns,
                  std::uint64_t seed) noexcept
        : rows_{rows}, inner_{inner}, columns_{columns}, seed_{seed} {}
    std::uint64_t rows_, inner_, columns_, seed_;
};

// A rank consumes its own ABI3 image of A rows followed by the full B.
class GemmU64RowPartition {
  public:
    [[nodiscard]] static auto for_rank(const GemmU64Config& whole, std::size_t rank)
        -> std::expected<GemmU64RowPartition, core::Error>;
    [[nodiscard]] auto local_shape() const noexcept -> const GemmU64Config& { return local_; }
    [[nodiscard]] auto first_row() const noexcept -> std::uint64_t { return first_; }
    [[nodiscard]] auto output_byte_offset() const noexcept -> std::size_t {
        return static_cast<std::size_t>(first_ * whole_.columns() * 8U);
    }
    [[nodiscard]] auto initialize_input(std::span<std::byte>) const
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto prepare_reference(std::span<const std::byte> input, std::span<std::byte> output) const
        -> std::expected<std::uint64_t, core::Error>;
    [[nodiscard]] auto single_task_checksum() const noexcept -> std::uint64_t;

  private:
    GemmU64RowPartition(GemmU64Config whole, GemmU64Config local, std::uint64_t first) noexcept
        : whole_{whole}, local_{local}, first_{first} {}
    GemmU64Config whole_, local_;
    std::uint64_t first_;
};

class GemmU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const GemmU64Config& {
        return configuration_;
    }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& {
        return output_;
    }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept
        -> std::uint64_t {
        return answer_ * count.value();
    }

  private:
    GemmU64Reference(GemmU64Config configuration, core::Sha256Digest input,
                     core::Sha256Digest output, std::uint64_t answer) noexcept
        : configuration_{configuration}, input_{input}, output_{output}, answer_{answer} {}
    GemmU64Config configuration_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_gemm_u64_reference(const GemmU64Config&, std::span<const std::byte>,
                                           std::span<std::byte>)
        -> std::expected<GemmU64Reference, core::Error>;
};
using GemmU64Verification = XorU64Verification;

// Canonical LE u64 words: magic, M, K, N, A[M*K], B[K*N]. Matrix payload
// ordinal i is mix64/v1(seed+i), with unsigned seed wrap. Input padding is not work.
[[nodiscard]] auto initialize_gemm_u64_input(const GemmU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
// Validates all generated input before mutation. Uses the existing host output
// as scratch for independently computed expected words; retains no borrowed spans
// or third matrix. Arithmetic is modulo 2^64, with products built from 32-bit limbs.
[[nodiscard]] auto prepare_gemm_u64_reference(const GemmU64Config&, std::span<const std::byte>,
                                              std::span<std::byte>)
    -> std::expected<GemmU64Reference, core::Error>;
// Offline replay reconstructs the reference from the authenticated shape and seed.
// The caller provides input_bytes() + output_bytes() scratch bytes. The result
// owns its digests and checksum and borrows neither of the scratch ranges.
[[nodiscard]] auto reconstruct_gemm_u64_reference(const GemmU64Config&, std::span<std::byte> scratch)
    -> std::expected<GemmU64Reference, core::Error>;
[[nodiscard]] auto reset_gemm_u64_output(const GemmU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_gemm_u64_output(const GemmU64Reference&, std::span<const std::byte>)
    -> std::expected<GemmU64Verification, core::Error>;

} // namespace epycbench::kernel
