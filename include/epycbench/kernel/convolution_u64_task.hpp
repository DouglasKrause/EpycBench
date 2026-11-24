#pragma once

#include "epycbench/kernel/task.hpp"

#include <array>

namespace epycbench::kernel {

inline constexpr std::string_view convolution_u64_task_id = "task.signal.convolution5.u64/v1";
inline constexpr std::string_view convolution_u64_scalar_implementation_id =
    "scalar-u64-convolution5/v1";
inline constexpr std::string_view convolution_u64_block4_implementation_id =
    "block4-u64-convolution5/v1";
inline constexpr std::string_view convolution_u64_generator_profile =
    "epycbench.generator.mix64-convolution5/v1";
inline constexpr std::string_view convolution_u64_correctness_profile =
    "epycbench.task.convolution5-u64.valid/v1";
inline constexpr std::array<std::uint64_t, 5> convolution_u64_taps{2, 3, 5, 7, 11};
inline constexpr std::uint64_t convolution_u64_input_magic = UINT64_C(0x3130564e4f434245);
inline constexpr std::uint64_t convolution_u64_maximum_samples = 1048576;
inline constexpr std::size_t convolution_u64_header_words = 2U + convolution_u64_taps.size();
inline constexpr std::byte convolution_u64_padding_sentinel{0xa5};

class ConvolutionU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t samples, std::uint64_t seed = 0)
        -> std::expected<ConvolutionU64Config, core::Error>;
    [[nodiscard]] auto sample_count() const noexcept -> std::uint64_t { return samples_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto output_samples() const noexcept -> std::uint64_t {
        return samples_ - convolution_u64_taps.size() + 1U;
    }
    [[nodiscard]] auto multiply_add_pairs() const noexcept -> std::uint64_t {
        return output_samples() * convolution_u64_taps.size();
    }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount {
        return *ElementCount::create(output_samples());
    }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * (convolution_u64_header_words + samples_));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto output_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * output_samples());
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / multiply_add_pairs();
    }
    [[nodiscard]] auto work(TaskRepetitionCount count) const
        -> std::expected<TaskWork, core::Error>;
    auto operator<=>(const ConvolutionU64Config&) const = default;

  private:
    ConvolutionU64Config(std::uint64_t samples, std::uint64_t seed) noexcept
        : samples_{samples}, seed_{seed} {}
    std::uint64_t samples_, seed_;
};

class ConvolutionU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const ConvolutionU64Config& {
        return config_;
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
    ConvolutionU64Reference(ConvolutionU64Config config, core::Sha256Digest input,
                            core::Sha256Digest output, std::uint64_t answer) noexcept
        : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    ConvolutionU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_convolution_u64_reference(const ConvolutionU64Config&,
                                                  std::span<const std::byte>, std::span<std::byte>)
        -> std::expected<ConvolutionU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_convolution_u64_input(const ConvolutionU64Config&,
                                                    std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_convolution_u64_reference(const ConvolutionU64Config&,
                                                     std::span<const std::byte>,
                                                     std::span<std::byte>)
    -> std::expected<ConvolutionU64Reference, core::Error>;
[[nodiscard]] auto reset_convolution_u64_output(const ConvolutionU64Reference&,
                                                std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_convolution_u64_output(const ConvolutionU64Reference&,
                                                 std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;

} // namespace epycbench::kernel
