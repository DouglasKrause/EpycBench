#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {
inline constexpr std::string_view stable_key_sort_u64_task_id = "task.order.stable-key-sort.u64/v1";
inline constexpr std::string_view stable_key_sort_u64_scan_implementation_id =
    "scan16-stable-key-sort-u64/v1";
inline constexpr std::string_view stable_key_sort_u64_count_implementation_id =
    "count16-stable-key-sort-u64/v1";
inline constexpr std::string_view stable_key_sort_u64_generator_profile =
    "epycbench.generator.mix64-high-nibble-ordinal/v1";
inline constexpr std::string_view stable_key_sort_u64_correctness_profile =
    "epycbench.task.stable-key-sort-u64.ordered-tagged-records/v1";
inline constexpr std::uint64_t stable_key_sort_u64_input_magic = UINT64_C(0x31305452534B4245);
inline constexpr std::uint64_t stable_key_sort_u64_maximum_records = 1048576U;
inline constexpr std::size_t stable_key_sort_u64_partitions = 16U;
inline constexpr std::size_t stable_key_sort_u64_header_words = 2U;
inline constexpr std::byte stable_key_sort_u64_padding_sentinel{0xa5};

class StableKeySortU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t records, std::uint64_t seed = 0U)
        -> std::expected<StableKeySortU64Config, core::Error>;
    [[nodiscard]] auto record_count() const noexcept -> std::uint64_t { return records_; }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount {
        return *ElementCount::create(records_);
    }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * (stable_key_sort_u64_header_words + records_));
    }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(logical_bytes().value());
    }
    [[nodiscard]] auto output_bytes() const noexcept -> core::ByteCount {
        return core::ByteCount::from_base_units(8U * records_);
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return UINT64_MAX / (records_ > stable_key_sort_u64_partitions
                                 ? records_
                                 : stable_key_sort_u64_partitions);
    }
    [[nodiscard]] auto work(TaskRepetitionCount count) const
        -> std::expected<TaskWork, core::Error>;
    auto operator<=>(const StableKeySortU64Config&) const = default;

  private:
    StableKeySortU64Config(std::uint64_t records, std::uint64_t seed) noexcept
        : records_{records}, seed_{seed} {}
    std::uint64_t records_, seed_;
};
class StableKeySortU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const StableKeySortU64Config& {
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
    StableKeySortU64Reference(StableKeySortU64Config config, core::Sha256Digest input,
                              core::Sha256Digest output, std::uint64_t answer) noexcept
        : config_{config}, input_{input}, output_{output}, answer_{answer} {}
    StableKeySortU64Config config_;
    core::Sha256Digest input_, output_;
    std::uint64_t answer_;
    friend auto prepare_stable_key_sort_u64_reference(const StableKeySortU64Config&,
                                                      std::span<const std::byte>,
                                                      std::span<std::byte>)
        -> std::expected<StableKeySortU64Reference, core::Error>;
};

[[nodiscard]] auto initialize_stable_key_sort_u64_input(const StableKeySortU64Config&,
                                                        std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto prepare_stable_key_sort_u64_reference(const StableKeySortU64Config&,
                                                         std::span<const std::byte>,
                                                         std::span<std::byte>)
    -> std::expected<StableKeySortU64Reference, core::Error>;
[[nodiscard]] auto reset_stable_key_sort_u64_output(const StableKeySortU64Reference&,
                                                    std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_stable_key_sort_u64_output(const StableKeySortU64Reference&,
                                                     std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;

} // namespace epycbench::kernel
