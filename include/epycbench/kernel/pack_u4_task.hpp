#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view pack_u4_task_id = "task.pack.u4/v1";
inline constexpr std::string_view pack_u4_pair_implementation_id = "scalar-u4-pack-pair/v1";
inline constexpr std::string_view pack_u4_unroll4_implementation_id = "scalar-u4-pack-unroll4/v1";
inline constexpr std::string_view pack_u4_generator_profile = "epycbench.generator.mix64-low-nibble/v1";
inline constexpr std::string_view pack_u4_correctness_profile = "epycbench.task.pack-u4.low-nibble-first/v1";
inline constexpr std::uint64_t pack_u4_maximum_count = UINT64_C(1) << 20U;
inline constexpr std::byte pack_u4_padding_sentinel{0xa5};

struct PackU4Work {
    TaskRepetitionCount completed_tasks;
    core::WorkCount processed_input_elements;
    core::ByteCount packed_output_bytes;
};

class PackU4Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t elements, std::uint64_t seed = 0)
        -> std::expected<PackU4Config, core::Error>;
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return elements_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(elements_.value());
    }
    [[nodiscard]] auto output_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units((elements_.value() + 1U) / 2U);
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t { return static_cast<std::size_t>(elements_.value()); }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t { return UINT64_MAX / elements_.value(); }
    [[nodiscard]] auto work(TaskRepetitionCount repetitions) const -> std::expected<PackU4Work, core::Error>;
    auto operator<=>(const PackU4Config&) const = default;
  private:
    PackU4Config(ElementCount elements, std::uint64_t seed) noexcept : elements_{elements}, seed_{seed} {}
    ElementCount elements_;
    std::uint64_t seed_;
};

class PackU4Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const PackU4Config& { return config_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& { return output_; }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return sum_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount repetitions) const noexcept -> std::uint64_t {
        return repetitions.value() * sum_;
    }
  private:
    PackU4Reference(PackU4Config config, core::Sha256Digest input, core::Sha256Digest output, std::uint64_t sum)
        : config_{config}, input_{input}, output_{output}, sum_{sum} {}
    PackU4Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t sum_;
    friend auto prepare_pack_u4_reference(const PackU4Config&, std::span<const std::byte>, std::span<std::byte>)
        -> std::expected<PackU4Reference, core::Error>;
};

[[nodiscard]] auto initialize_pack_u4_input(const PackU4Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_pack_u4_reference(const PackU4Config&, std::span<const std::byte>, std::span<std::byte>)
    -> std::expected<PackU4Reference, core::Error>;
[[nodiscard]] auto reset_pack_u4_output(const PackU4Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_pack_u4_output(const PackU4Reference&, std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;
} // namespace epycbench::kernel
