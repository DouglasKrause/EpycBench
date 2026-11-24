#pragma once

#include "epycbench/kernel/task.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view serialize_u64_task_id = "task.serialize.be64/v1";
inline constexpr std::string_view serialize_u64_bytes_implementation_id = "scalar-u64-serialize-bytes/v1";
inline constexpr std::string_view serialize_u64_bswap_implementation_id = "scalar-u64-serialize-bswap/v1";
inline constexpr std::string_view serialize_u64_correctness_profile = "epycbench.task.serialize.network-u64/v1";
inline constexpr std::uint64_t serialize_u64_maximum_count = UINT64_C(1) << 20U;
inline constexpr std::byte serialize_u64_padding_sentinel{0xa5};

struct SerializeU64Work {
    TaskRepetitionCount completed_tasks;
    core::WorkCount serialized_records;
    core::ByteCount encoded_bytes;
};

class SerializeU64Config {
  public:
    [[nodiscard]] static auto create(std::uint64_t records, std::uint64_t input_seed = 0)
        -> std::expected<SerializeU64Config, core::Error>;
    [[nodiscard]] auto input_configuration() const noexcept -> const SumU64Config& { return input_; }
    [[nodiscard]] auto element_count() const noexcept -> ElementCount { return input_.element_count(); }
    [[nodiscard]] auto input_seed() const noexcept -> std::uint64_t { return input_.input_seed(); }
    [[nodiscard]] auto logical_bytes() const noexcept -> core::ByteCount { return input_.logical_bytes(); }
    [[nodiscard]] auto native_bytes() const noexcept -> std::size_t { return input_.native_bytes(); }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t { return UINT64_MAX / logical_bytes().value(); }
    [[nodiscard]] auto work(TaskRepetitionCount) const -> std::expected<SerializeU64Work, core::Error>;
    auto operator<=>(const SerializeU64Config&) const = default;
  private:
    explicit SerializeU64Config(SumU64Config input) : input_{input} {}
    SumU64Config input_;
};

class SerializeU64Reference {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const SerializeU64Config& { return configuration_; }
    [[nodiscard]] auto input_digest() const noexcept -> const core::Sha256Digest& { return input_digest_; }
    [[nodiscard]] auto expected_output_digest() const noexcept -> const core::Sha256Digest& { return output_digest_; }
    [[nodiscard]] auto single_task_answer() const noexcept -> std::uint64_t { return checksum_; }
    [[nodiscard]] auto expected_checksum(TaskRepetitionCount repetitions) const noexcept -> std::uint64_t {
        return checksum_ * repetitions.value();
    }
  private:
    SerializeU64Reference(SerializeU64Config configuration, core::Sha256Digest input,
                          core::Sha256Digest output, std::uint64_t checksum)
        : configuration_{configuration}, input_digest_{input}, output_digest_{output}, checksum_{checksum} {}
    SerializeU64Config configuration_;
    core::Sha256Digest input_digest_, output_digest_;
    std::uint64_t checksum_;
    friend auto prepare_serialize_u64_reference(const SerializeU64Config&, std::span<const std::byte>,
        std::span<std::byte>) -> std::expected<SerializeU64Reference, core::Error>;
};

[[nodiscard]] auto prepare_serialize_u64_reference(const SerializeU64Config&, std::span<const std::byte>,
    std::span<std::byte>) -> std::expected<SerializeU64Reference, core::Error>;
[[nodiscard]] auto reset_serialize_u64_output(const SerializeU64Reference&, std::span<std::byte>)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto verify_serialize_u64_output(const SerializeU64Reference&, std::span<const std::byte>)
    -> std::expected<XorU64Verification, core::Error>;
} // namespace epycbench::kernel
