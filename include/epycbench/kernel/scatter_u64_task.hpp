#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view scatter_u64_task_id = "task.scatter.keyed.u64/v1";
inline constexpr std::string_view scatter_u64_scalar_implementation_id = "scalar-u64-scatter-keyed/v1";
inline constexpr std::string_view scatter_u64_unroll4_implementation_id = "unroll4-u64-scatter-keyed/v1";
inline constexpr std::string_view scatter_u64_generator_profile = "epycbench.generator.mix64-shuffled-keyed-records/v1";
inline constexpr std::string_view scatter_u64_correctness_profile = "epycbench.task.scatter-keyed-u64.permutation/v1";
inline constexpr std::uint64_t scatter_u64_input_magic = UINT64_C(0x3130544143534245);
inline constexpr std::uint64_t scatter_u64_maximum_count = 65536U;
inline constexpr std::size_t scatter_u64_header_words = 3U;
inline constexpr std::byte scatter_u64_padding_sentinel{0xa5};

class ScatterU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t count, std::uint64_t seed = 0U)
        -> std::expected<ScatterU64Config, core::Error>;
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return *ElementCount::create(count_); }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * (scatter_u64_header_words + count_));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t { return UINT64_MAX / count_; }
    [[nodiscard]] auto work(TaskRepetitionCount count) const -> std::expected<TaskWork, core::Error> {
        return TaskWork::create(element_count(), count);
    }
    auto operator<=>(const ScatterU64Config&) const = default;
  private:
    ScatterU64Config(std::uint64_t count, std::uint64_t seed) noexcept
        : count_{count}, seed_{seed} {}
    std::uint64_t count_, seed_;
};

class ScatterU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const ScatterU64Config& { return config_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& { return output_; }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount count) const noexcept -> std::uint64_t {
        return answer_ * count.value();
    }
  private:
    ScatterU64Reference(ScatterU64Config config, core::Sha256Digest input, core::Sha256Digest output,
                          std::uint64_t answer) noexcept : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    ScatterU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_scatter_u64_reference(const ScatterU64Config&, std::span<const std::byte>,
        std::span<std::byte>) -> std::expected<ScatterU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_scatter_u64_input(const ScatterU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_scatter_u64_reference(const ScatterU64Config&, std::span<const std::byte>,
    std::span<std::byte>) -> std::expected<ScatterU64Reference, core::Error>;
[[nodiscard]] auto reset_scatter_u64_output(const ScatterU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_scatter_u64_output(const ScatterU64Reference&, std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;

} // namespace epycbench::kernel
