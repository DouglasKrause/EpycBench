#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view stencil_u64_task_id = "task.grid.stencil5.u64/v1";
inline constexpr std::string_view stencil_u64_row_implementation_id = "scalar-u64-stencil5-row/v1";
inline constexpr std::string_view stencil_u64_column_implementation_id = "scalar-u64-stencil5-column/v1";
inline constexpr std::string_view stencil_u64_generator_profile = "epycbench.generator.mix64-stencil-grid/v1";
inline constexpr std::string_view stencil_u64_correctness_profile = "epycbench.task.stencil5-u64.zero-exterior/v1";
inline constexpr std::uint64_t stencil_u64_input_magic = UINT64_C(0x3130545035425045);
inline constexpr std::uint64_t stencil_u64_maximum_dimension = 256U;
inline constexpr std::size_t stencil_u64_header_words = 3U;
inline constexpr std::byte stencil_u64_padding_sentinel{0xa5};

class StencilU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t rows, std::uint64_t columns, std::uint64_t seed = 0U)
        -> std::expected<StencilU64Config, core::Error>;
    [[nodiscard]] auto rows() const noexcept -> std::uint64_t { return rows_; }
    [[nodiscard]] auto columns() const noexcept -> std::uint64_t { return columns_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return *ElementCount::create(rows_ * columns_); }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * (stencil_u64_header_words + rows_ * columns_));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t { return UINT64_MAX / (rows_ * columns_); }
    [[nodiscard]] auto work(TaskRepetitionCount count) const -> std::expected<TaskWork, core::Error> {
        return TaskWork::create(element_count(), count);
    }
    auto operator<=>(const StencilU64Config&) const = default;
  private:
    StencilU64Config(std::uint64_t rows, std::uint64_t columns, std::uint64_t seed) noexcept
        : rows_{rows}, columns_{columns}, seed_{seed} {}
    std::uint64_t rows_, columns_, seed_;
};

class StencilU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const StencilU64Config& { return config_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& { return output_; }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept -> std::uint64_t {
        return answer_ * count.value();
    }
  private:
    StencilU64Reference(StencilU64Config config, core::Sha256Digest input, core::Sha256Digest output,
                          std::uint64_t answer) noexcept : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    StencilU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_stencil_u64_reference(const StencilU64Config&, std::span<const std::byte>,
        std::span<std::byte>) -> std::expected<StencilU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_stencil_u64_input(const StencilU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_stencil_u64_reference(const StencilU64Config&, std::span<const std::byte>,
    std::span<std::byte>) -> std::expected<StencilU64Reference, core::Error>;
[[nodiscard]] auto reset_stencil_u64_output(const StencilU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_stencil_u64_output(const StencilU64Reference&, std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;

} // namespace epycbench::kernel
