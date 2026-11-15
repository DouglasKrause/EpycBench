#pragma once

#include "runtime/finalized_evidence_wire_startup_codec.hpp"

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::runtime::prepared_resources_v1 {

inline constexpr std::string_view prepared_offer_profile =
    "epycbench.finalized-evidence-prepared-offer/v1";
inline constexpr std::string_view prepared_ack_profile =
    "epycbench.finalized-evidence-prepared-ack/v1";

enum class PreparedResourceErrorCode : std::uint8_t {
    invalid_input, arithmetic_overflow, size_unrepresentable, allocation_failure,
    malformed, noncanonical, shape, domain, digest_mismatch, resource_mismatch,
    invalid_state
};
struct PreparedResourceError final {
    PreparedResourceErrorCode code;
    std::uint32_t index{};
    std::uint64_t limit{};
    std::uint64_t observed{};
    friend constexpr auto operator==(const PreparedResourceError&,
                                     const PreparedResourceError&) noexcept -> bool = default;
};

enum class PreparedDefinitionKind : std::uint8_t { scalar, object, array, option, sum };
struct PreparedDefinitionStock final {
    std::string_view definition_id;
    PreparedDefinitionKind kind;
    std::uint64_t owner_occurrences{};
    std::optional<std::uint64_t> local_array_cardinality;
    std::optional<std::uint64_t> decoded_utf8_bytes;
    friend constexpr auto operator==(const PreparedDefinitionStock&,
                                     const PreparedDefinitionStock&) noexcept -> bool = default;
};

struct PreparedScalarStocks final {
    std::uint64_t logical_bytes{}, logical_probe_bytes{}, parser_nodes{}, parser_depth{};
    std::uint64_t builder_slots{}, builder_initialized_bits{}, array_owners{}, array_elements{};
    std::uint64_t string_owners{}, decoded_string_bytes{};
    std::uint64_t results_raw_graph_slots{}, results_raw_graph_elements{},
        results_raw_graph_string_bytes{};
    std::uint64_t results_first_metrics{}, results_second_metrics{},
        results_first_observation_ids{}, results_second_observation_ids{}, results_block_ids{},
        results_paired_effects{}, results_first_outlier_states{}, results_second_outlier_states{};
    std::uint64_t statistics_metric_scratch{}, statistics_double_scratch_primary{},
        statistics_double_scratch_secondary{}, statistics_effect_resample{},
        statistics_stopping_prefix{}, results_stopping_replays{},
        results_mismatch_diagnostics{}, active_primary_entries{}, active_secondary_entries{};
    friend constexpr auto operator==(const PreparedScalarStocks&,
                                     const PreparedScalarStocks&) noexcept -> bool = default;
};

struct PreparedResourceValue final {
    core::Sha256Digest wire_registry_sha256;
    core::Sha256Digest source_manifest_sha256;
    core::Sha256Digest capacity_offer_body_sha256;
    std::uint64_t comparison_bootstrap_replicates{};
    std::uint64_t stopping_bootstrap_replicates{};
    std::vector<PreparedDefinitionStock> definitions;
    PreparedScalarStocks stocks;
    friend auto operator==(const PreparedResourceValue&, const PreparedResourceValue&) noexcept
        -> bool = default;
};

struct PreparedResourceLimits final {
    std::uint64_t size_max{static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())};
    std::uint64_t definition_container_max{static_cast<std::uint64_t>(
        std::vector<PreparedDefinitionStock>{}.max_size())};
};

[[nodiscard]] auto derive_prepared_resource(
    const wire_v1::detail::AuthoredCapacityOffer& capacity_offer,
    const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
    PreparedResourceLimits limits = {}) noexcept
    -> std::expected<PreparedResourceValue, PreparedResourceError>;
[[nodiscard]] auto encode_prepared_resource(const PreparedResourceValue&) noexcept
    -> std::expected<std::string, PreparedResourceError>;

struct AuthoredPreparedOffer final {
    wire_v1::detail::StartupSessionId session_id;
    ipc::RequestId request_id;
    core::Sha256Digest request_body_sha256;
    core::Sha256Digest capacity_offer_body_sha256;
    PreparedResourceValue resource;
    std::string canonical_resource_body;
    core::Sha256Digest prepared_resource_sha256;
    std::string canonical_body;
    core::Sha256Digest body_sha256;
};
struct ParsedPreparedOffer final {
    wire_v1::detail::StartupSessionId session_id;
    ipc::RequestId request_id;
    core::Sha256Digest request_body_sha256;
    core::Sha256Digest capacity_offer_body_sha256;
    PreparedResourceValue resource;
    std::string canonical_body;
    core::Sha256Digest body_sha256;
    core::Sha256Digest prepared_resource_sha256;
};
struct AcceptedPreparedAck final {
    std::string canonical_body;
    core::Sha256Digest body_sha256;
};

[[nodiscard]] auto author_prepared_offer(
    const wire_v1::detail::AuthoredCapacityOffer&,
    const runtime::detail::PairwiseHostCapacityRequestTranscript&) noexcept
    -> std::expected<AuthoredPreparedOffer, PreparedResourceError>;
[[nodiscard]] auto parse_prepared_offer(
    std::string_view, const wire_v1::detail::StartupCapacityValue& runner_capacity,
    const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
    core::Sha256Digest accepted_capacity_offer_body_sha256) noexcept
    -> std::expected<ParsedPreparedOffer, PreparedResourceError>;
[[nodiscard]] auto accept_prepared_offer(
    const ParsedPreparedOffer&) noexcept
    -> std::expected<AcceptedPreparedAck, PreparedResourceError>;
[[nodiscard]] auto parse_prepared_ack(std::string_view, const AuthoredPreparedOffer&) noexcept
    -> std::expected<AcceptedPreparedAck, PreparedResourceError>;

enum class PreparedPipelineState : std::uint8_t { prepared, active, consumed, failed };
// Slice-1 one-use capability state. This is not the later runner/controller
// active-pipeline lifecycle; consume() transfers the prepared value to that
// future role-specific owner exactly once.
class PreparedResourceOwner final {
  public:
    PreparedResourceOwner(const PreparedResourceOwner&) = delete;
    PreparedResourceOwner(PreparedResourceOwner&&) noexcept = default;
    auto operator=(const PreparedResourceOwner&) -> PreparedResourceOwner& = delete;
    auto operator=(PreparedResourceOwner&&) -> PreparedResourceOwner& = delete;
    [[nodiscard]] static auto create(PreparedResourceValue) noexcept
        -> std::expected<PreparedResourceOwner, PreparedResourceError>;
    [[nodiscard]] auto activate() noexcept -> std::expected<void, PreparedResourceError>;
    [[nodiscard]] auto consume() noexcept -> std::expected<void, PreparedResourceError>;
    [[nodiscard]] auto fail() noexcept -> std::expected<void, PreparedResourceError>;
    [[nodiscard]] auto state() const noexcept -> PreparedPipelineState { return state_; }
    [[nodiscard]] auto resource() const noexcept -> const PreparedResourceValue& { return value_; }
  private:
    explicit PreparedResourceOwner(PreparedResourceValue value) noexcept : value_{std::move(value)} {}
    PreparedResourceValue value_;
    PreparedPipelineState state_{PreparedPipelineState::prepared};
};

} // namespace epycbench::runtime::prepared_resources_v1
