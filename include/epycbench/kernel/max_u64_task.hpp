#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view max_u64_task_id = "task.max.u64/v1";
inline constexpr std::string_view max_u64_scalar_implementation_id = "scalar-u64-max-single/v1";
inline constexpr std::string_view max_u64_avx2_implementation_id = "avx2-u64-max/v1";
inline constexpr std::string_view max_u64_generator_profile = sum_u64_generator_profile;
inline constexpr std::string_view max_u64_correctness_profile =
    "epycbench.task.max-u64.independent-reference/v1";

// The input generator, LE encoding and positive N/work bounds are exactly Sum's.
// A task computes the unsigned maximum of all N words. Its answer is folded once
// per repetition with uint64 modular addition; padding is never input or work.
class MaxU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t element_count, std::uint64_t input_seed = 0U)
        -> std::expected<MaxU64Config, core::Error>;
    [[nodiscard]] auto input_configuration() const noexcept -> const SumU64Config& { return input_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return input_.element_count(); }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return input_.input_seed(); }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount { return input_.logical_bytes(); }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t { return input_.native_bytes(); }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t { return input_.maximum_repetitions(); }
    auto operator<=>(const MaxU64Config&) const = default;
  private:
    explicit MaxU64Config(SumU64Config input) noexcept : input_{input} {}
    SumU64Config input_;
};

class MaxU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const MaxU64Config& { return configuration_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return digest_; }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount repetitions) const noexcept -> std::uint64_t {
        return answer_ * repetitions.value();
    }
  private:
    MaxU64Reference(MaxU64Config configuration, core::Sha256Digest digest, std::uint64_t answer) noexcept
        : configuration_{configuration}, digest_{digest}, answer_{answer} {}
    MaxU64Config configuration_;
    core::Sha256Digest digest_;
    std::uint64_t answer_;
    friend auto prepare_max_u64_reference(const MaxU64Config&, std::span<const std::byte>)
        -> std::expected<MaxU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_max_u64_input(const MaxU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_max_u64_reference(const MaxU64Config&, std::span<const std::byte>)
    -> std::expected<MaxU64Reference, core::Error>;
} // namespace epycbench::kernel
