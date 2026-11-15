#pragma once

#include "runtime/stage_b_resource_policy.hpp"

#include "epycbench/runtime/ipc/protocol.hpp"

#include <expected>
#include <string>

namespace epycbench::runtime::detail {

inline constexpr std::string_view pairwise_v2_resource_startup_protocol =
    "epycbench.finalized-evidence-resource-startup/v1";

enum class PairwiseV2ResourceStartupFailure : unsigned char {
    invalid_state,
    framing,
    malformed,
    noncanonical,
    unexpected_message,
    identity_mismatch,
    io,
    contained_exception,
    contained_unknown_failure,
};

struct PairwiseV2ResourceStartupOffers final {
    std::string capacity;
    std::string prepared;
};

struct PairwiseV2ResourceStartupAcks final {
    std::string capacity;
    std::string prepared;
};

// Inactive second startup sub-prelude. Payloads remain the accepted logical
// bodies; this owner supplies only their closed outer framing and one-use
// ordering on the existing channels.
class PairwiseV2ResourceStartupController final {
  public:
    explicit PairwiseV2ResourceStartupController(ipc::RequestId request) noexcept
        : request_{request} {}
    [[nodiscard]] auto write_offers(ipc::ByteWriter&, std::string_view capacity,
                                    std::string_view prepared) noexcept
        -> std::expected<void, PairwiseV2ResourceStartupFailure>;
    // The composed owner validates the capacity body before it requests the
    // next envelope. This preserves precedence even if the later frame is bad.
    [[nodiscard]] auto read_capacity_ack(ipc::ByteReader&) noexcept
        -> std::expected<std::string, PairwiseV2ResourceStartupFailure>;
    [[nodiscard]] auto read_prepared_ack(ipc::ByteReader&) noexcept
        -> std::expected<std::string, PairwiseV2ResourceStartupFailure>;
    [[nodiscard]] auto read_acks(ipc::ByteReader&) noexcept
        -> std::expected<PairwiseV2ResourceStartupAcks,
                         PairwiseV2ResourceStartupFailure>;

  private:
    ipc::RequestId request_;
    enum class Phase : unsigned char { start, offered, capacity_read, complete, failed } phase_{};
};

class PairwiseV2ResourceStartupRunner final {
  public:
    explicit PairwiseV2ResourceStartupRunner(ipc::RequestId request) noexcept
        : request_{request} {}
    [[nodiscard]] auto read_offers(ipc::ByteReader&,
        stage_b_policy::Selection = stage_b_policy::Selection::existing_caller) noexcept
        -> std::expected<PairwiseV2ResourceStartupOffers,
                         PairwiseV2ResourceStartupFailure>;
    [[nodiscard]] auto write_acks(ipc::ByteWriter&, std::string_view capacity,
                                  std::string_view prepared) noexcept
        -> std::expected<void, PairwiseV2ResourceStartupFailure>;

  private:
    ipc::RequestId request_;
    enum class Phase : unsigned char { start, offered, complete, failed } phase_{};
};

} // namespace epycbench::runtime::detail
