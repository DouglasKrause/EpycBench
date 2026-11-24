#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view csr_u64_task_id = "task.spmv.csr.u64/v1";
inline constexpr std::string_view csr_u64_scalar_implementation_id = "scalar-u64-spmv-csr/v1";
inline constexpr std::string_view csr_u64_unroll4_implementation_id = "unroll4-u64-spmv-csr/v1";
inline constexpr std::string_view csr_u64_generator_profile =
    "epycbench.generator.mix64-stratified-csr/v1";
inline constexpr std::string_view csr_u64_correctness_profile =
    "epycbench.task.spmv-csr-u64.rows/v1";
inline constexpr std::uint64_t csr_u64_input_magic = UINT64_C(0x3130305253434245);
inline constexpr std::uint64_t csr_u64_maximum_rows = 4096U, csr_u64_maximum_columns = 65536U;
inline constexpr std::size_t csr_u64_header_words = 4U;
inline constexpr std::byte csr_u64_padding_sentinel{0xa5};

class CsrU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t rows, std::uint64_t columns,
                                     std::uint64_t seed = 0U)
        -> std::expected<CsrU64Config, core::Error>;
    [[nodiscard]] auto rows() const noexcept -> std::uint64_t { return rows_; }
    [[nodiscard]] auto columns() const noexcept -> std::uint64_t { return columns_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto nonzeros() const noexcept -> std::uint64_t { return nonzeros_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount {
        return *ElementCount::create(rows_);
    }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(
            8U * (csr_u64_header_words + rows_ + 1U + 2U * nonzeros_ + columns_));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto output_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * (csr_u64_header_words + rows_));
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / (rows_ > nonzeros_ ? rows_ : nonzeros_);
    }
    [[nodiscard]] auto work(TaskRepetitionCount) const -> std::expected<TaskWork, core::Error>;
    auto operator<=>(const CsrU64Config&) const = default;

  private:
    CsrU64Config(std::uint64_t rows, std::uint64_t columns, std::uint64_t seed,
                 std::uint64_t nonzeros) noexcept
        : rows_{rows}, columns_{columns}, seed_{seed}, nonzeros_{nonzeros} {}
    std::uint64_t rows_, columns_, seed_, nonzeros_;
};

class CsrU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const CsrU64Config& { return config_; }
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
    CsrU64Reference(CsrU64Config config, core::Sha256Digest input, core::Sha256Digest output,
                    std::uint64_t answer) noexcept
        : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    CsrU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_csr_u64_reference(const CsrU64Config&, std::span<const std::byte>,
                                          std::span<std::byte>)
        -> std::expected<CsrU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_csr_u64_input(const CsrU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_csr_u64_reference(const CsrU64Config&, std::span<const std::byte>,
                                             std::span<std::byte>)
    -> std::expected<CsrU64Reference, core::Error>;
[[nodiscard]] auto reset_csr_u64_output(const CsrU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_csr_u64_output(const CsrU64Reference&, std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;

} // namespace epycbench::kernel
