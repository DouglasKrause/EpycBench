#pragma once

#include "runtime/finalized_evidence_wire_source_projection.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace epycbench::runtime::wire_v1::detail {

inline constexpr std::string_view capacity_offer_profile =
    "epycbench.finalized-pairwise-evidence-capacity-offer/v1";
inline constexpr std::string_view capacity_accepted_profile =
    "epycbench.finalized-pairwise-evidence-capacity-accepted/v1";

enum class StartupCodecErrorCode : std::uint8_t {
    invalid_argument,
    malformed,
    noncanonical,
    shape,
    domain,
    digest_mismatch,
    capacity_mismatch,
    allocation_failure,
};

struct StartupCodecError final {
    StartupCodecErrorCode code;
    friend constexpr auto operator==(const StartupCodecError&,
                                     const StartupCodecError&) noexcept -> bool = default;
};

class StartupSessionId final {
  public:
    [[nodiscard]] static auto parse(std::string_view) noexcept
        -> std::expected<StartupSessionId, StartupCodecError>;
    [[nodiscard]] auto text() const noexcept -> std::string_view { return text_; }
    auto operator==(const StartupSessionId&) const noexcept -> bool = default;

  private:
    explicit StartupSessionId(std::string text) : text_{std::move(text)} {}
    std::string text_;
};

class StartupCapacityValue final {
  public:
    [[nodiscard]] auto session_id() const noexcept -> const StartupSessionId& { return session_id_; }
    [[nodiscard]] auto request_id() const noexcept -> const ipc::RequestId& { return request_id_; }
    [[nodiscard]] auto projected() const noexcept -> const ProjectedCapacityContext& { return projected_; }
    [[nodiscard]] auto family() const noexcept -> std::string_view { return family_; }
    friend auto operator==(const StartupCapacityValue&, const StartupCapacityValue&) noexcept
        -> bool = default;

  private:
    StartupCapacityValue(StartupSessionId session, ipc::RequestId request,
                         ProjectedCapacityContext projected, std::string family) noexcept
        : session_id_{std::move(session)}, request_id_{request}, projected_{std::move(projected)},
          family_{std::move(family)} {}
    StartupSessionId session_id_;
    ipc::RequestId request_id_;
    ProjectedCapacityContext projected_;
    std::string family_;
    friend auto construct_controller_startup_capacity(
        StartupSessionId, const runtime::detail::PairwiseHostCapacityRequestTranscript&,
        const RunnerContext&,
        const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority&, LocalResourceLimits) noexcept
        -> std::expected<StartupCapacityValue, StartupCodecError>;
    friend auto construct_runner_startup_capacity(
        StartupSessionId, const runtime::detail::PairwiseHostCapacityRequestTranscript&,
        const AdmittedPairwiseProductionInputs&,
        const runtime::detail::PairwiseMonitoringHostCapacityAdmission&, LocalResourceLimits) noexcept
        -> std::expected<StartupCapacityValue, StartupCodecError>;
};

struct AuthoredCapacityOffer final {
    StartupCapacityValue capacity;
    std::string canonical_body;
    core::Sha256Digest body_sha256;
    core::Sha256Digest capacity_body_sha256;
};

struct ParsedCapacityOffer final {
    StartupSessionId session_id;
    ipc::RequestId request_id;
    core::Sha256Digest request_body_sha256;
    core::Sha256Digest source_manifest_sha256;
    core::Sha256Digest projection_context_profile_sha256;
    std::string canonical_capacity_body;
    std::string canonical_body;
    core::Sha256Digest body_sha256;
    core::Sha256Digest capacity_body_sha256;
};

struct AcceptedCapacityAck final {
    StartupSessionId session_id;
    ipc::RequestId request_id;
    core::Sha256Digest request_body_sha256;
    core::Sha256Digest offer_body_sha256;
    core::Sha256Digest capacity_body_sha256;
    std::string canonical_body;
    core::Sha256Digest body_sha256;
};

[[nodiscard]] auto construct_controller_startup_capacity(
    StartupSessionId, const runtime::detail::PairwiseHostCapacityRequestTranscript&, const RunnerContext&,
    const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority&, LocalResourceLimits = {}) noexcept
    -> std::expected<StartupCapacityValue, StartupCodecError>;

[[nodiscard]] auto construct_runner_startup_capacity(
    StartupSessionId, const runtime::detail::PairwiseHostCapacityRequestTranscript&,
    const AdmittedPairwiseProductionInputs&,
    const runtime::detail::PairwiseMonitoringHostCapacityAdmission&, LocalResourceLimits = {}) noexcept
    -> std::expected<StartupCapacityValue, StartupCodecError>;

[[nodiscard]] auto encode_startup_capacity(const StartupCapacityValue&) noexcept
    -> std::expected<std::string, StartupCodecError>;
[[nodiscard]] auto author_capacity_offer(StartupCapacityValue) noexcept
    -> std::expected<AuthoredCapacityOffer, StartupCodecError>;
// Evidence consistency only: no authenticated local capacity or acknowledgement.
struct CapacityFactsParseLimits final { std::size_t bytes{}, nodes{}, depth{}; };
[[nodiscard]] auto parse_capacity_offer_facts(std::string_view, CapacityFactsParseLimits) noexcept
    -> std::expected<ParsedCapacityOffer, StartupCodecError>;
[[nodiscard]] auto parse_capacity_offer(std::string_view) noexcept
    -> std::expected<ParsedCapacityOffer, StartupCodecError> = delete;
[[nodiscard]] auto parse_capacity_offer(std::string_view,
                                        const StartupCapacityValue& runner_local) noexcept
    -> std::expected<ParsedCapacityOffer, StartupCodecError>;

[[nodiscard]] auto accept_capacity_offer(
    const ParsedCapacityOffer&, const StartupCapacityValue&) noexcept
    -> std::expected<AcceptedCapacityAck, StartupCodecError>;
[[nodiscard]] auto parse_accepted_capacity_ack(
    std::string_view, const AuthoredCapacityOffer&) noexcept
    -> std::expected<AcceptedCapacityAck, StartupCodecError>;

} // namespace epycbench::runtime::wire_v1::detail
