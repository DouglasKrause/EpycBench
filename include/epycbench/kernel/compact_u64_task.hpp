#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view compact_u64_task_id = "task.compact.even.u64/v1";
inline constexpr std::string_view compact_u64_scalar_implementation_id = "scalar-u64-compact-even/v1";
inline constexpr std::string_view compact_u64_mask4_implementation_id = "mask4-u64-compact-even/v1";
inline constexpr std::string_view compact_u64_generator_profile = "epycbench.generator.counted-mix64/v1";
inline constexpr std::string_view compact_u64_correctness_profile = "epycbench.task.compact-even-u64.ordered-output/v1";
inline constexpr std::uint64_t compact_u64_maximum_count = UINT64_C(1) << 20U;
inline constexpr std::byte compact_u64_unused_byte{0xd3};

class CompactU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t elements, std::uint64_t seed = 0U)
        -> std::expected<CompactU64Config, core::Error>;
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return elements_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    // Both roles contain a count word and N slots. The output count may be zero.
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * (elements_.value() + 1U));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t { return UINT64_MAX / elements_.value(); }
    auto operator<=>(const CompactU64Config&) const = default;
  private:
    CompactU64Config(ElementCount elements, std::uint64_t seed) noexcept : elements_{elements}, seed_{seed} {}
    ElementCount elements_;
    std::uint64_t seed_;
};

class CompactU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const CompactU64Config& { return config_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& { return output_; }
    [[nodiscard]] auto selected_elements() const noexcept -> std::uint64_t { return selected_; }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return answer_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount repetitions) const noexcept -> std::uint64_t {
        return repetitions.value() * answer_;
    }
  private:
    CompactU64Reference(CompactU64Config config, core::Sha256Digest input, core::Sha256Digest output,
                        std::uint64_t selected, std::uint64_t answer) noexcept
        : config_{config}, input_{input}, output_{output}, selected_{selected}, answer_{answer} {}
    CompactU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t selected_, answer_;
    friend auto prepare_compact_u64_reference(const CompactU64Config&, std::span<const std::byte>, std::span<std::byte>)
        -> std::expected<CompactU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_compact_u64_input(const CompactU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_compact_u64_reference(const CompactU64Config&, std::span<const std::byte>,
    std::span<std::byte>) -> std::expected<CompactU64Reference, core::Error>;
[[nodiscard]] auto reset_compact_u64_output(const CompactU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_compact_u64_output(const CompactU64Reference&, std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;
} // namespace epycbench::kernel
