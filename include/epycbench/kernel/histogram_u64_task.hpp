#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view histogram_u64_task_id = "task.group.histogram-weighted.u64/v1";
inline constexpr std::string_view histogram_u64_direct_implementation_id =
    "direct-u64-weighted-histogram/v1";
inline constexpr std::string_view histogram_u64_hash_implementation_id =
    "hash512-u64-weighted-histogram/v1";
inline constexpr std::string_view histogram_u64_generator_profile =
    "epycbench.generator.mix64-hot-four-keys/v1";
inline constexpr std::string_view histogram_u64_correctness_profile =
    "epycbench.task.histogram-weighted-u64.ordered-bins/v1";
inline constexpr std::uint64_t histogram_u64_input_magic = UINT64_C(0x3130485349484245);
inline constexpr std::uint64_t histogram_u64_maximum_records = 1048576U;
inline constexpr std::size_t histogram_u64_bins = 256U;
inline constexpr std::size_t histogram_u64_hash_slots = 512U;
inline constexpr std::size_t histogram_u64_header_words = 2U;
inline constexpr std::byte histogram_u64_padding_sentinel{0xa5};

class HistogramU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t records, std::uint64_t seed = 0U)
        -> std::expected<HistogramU64Config, core::Error>;
    [[nodiscard]] auto record_count() const noexcept -> std::uint64_t { return records_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount {
        return *ElementCount::create(records_);
    }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * (histogram_u64_header_words + records_));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto output_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(16U * histogram_u64_bins);
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / (records_ > histogram_u64_bins ? records_ : histogram_u64_bins);
    }
    [[nodiscard]] auto work(TaskRepetitionCount count) const
        -> std::expected<TaskWork, core::Error>;
    auto operator<=>(const HistogramU64Config&) const = default;

  private:
    HistogramU64Config(std::uint64_t records, std::uint64_t seed) noexcept
        : records_{records}, seed_{seed} {}
    std::uint64_t records_, seed_;
};
class HistogramU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const HistogramU64Config& {
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
    HistogramU64Reference(HistogramU64Config config, core::Sha256Digest input,
                          core::Sha256Digest output, std::uint64_t answer) noexcept
        : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    HistogramU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_histogram_u64_reference(const HistogramU64Config&,
                                                std::span<const std::byte>, std::span<std::byte>)
        -> std::expected<HistogramU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_histogram_u64_input(const HistogramU64Config&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_histogram_u64_reference(const HistogramU64Config&,
                                                   std::span<const std::byte>, std::span<std::byte>)
    -> std::expected<HistogramU64Reference, core::Error>;
[[nodiscard]] auto reset_histogram_u64_output(const HistogramU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_histogram_u64_output(const HistogramU64Reference&,
                                               std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;

} // namespace epycbench::kernel
