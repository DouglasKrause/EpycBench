#pragma once

#include "runtime/finalized_evidence_wire_capacity.hpp"
#include "runtime/finalized_evidence_wire_json.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace epycbench::runtime::wire_v1 {

enum class LogicalCodecErrorCode : std::uint8_t {
    byte_limit,
    node_limit,
    depth_limit,
    malformed,
    unsupported_number,
    noncanonical,
    allocation_failure,
    registry_shape,
    registry_domain,
    registry_cardinality,
};

namespace detail {
constexpr auto logical_render_failure(WireJsonFailure failure,
                                      LogicalCodecErrorCode fallback) noexcept
    -> LogicalCodecErrorCode {
    return failure == WireJsonFailure::overflow ? LogicalCodecErrorCode::allocation_failure
                                                : fallback;
}
} // namespace detail

struct LogicalCodecError final {
    LogicalCodecErrorCode code;
    CapacityCount limit{};
    CapacityCount observed{};
    friend constexpr auto operator==(const LogicalCodecError&, const LogicalCodecError&) noexcept
        -> bool = default;
};

class LogicalBody final {
  public:
    LogicalBody(LogicalBody&&) noexcept;
    auto operator=(LogicalBody&&) noexcept -> LogicalBody&;
    ~LogicalBody();

    LogicalBody(const LogicalBody&) = delete;
    auto operator=(const LogicalBody&) -> LogicalBody& = delete;

    [[nodiscard]] auto value() const noexcept -> const nlohmann::json&;

  private:
    explicit LogicalBody(detail::WireJson value, std::size_t nodes);
    detail::WireJson value_;
    std::size_t nodes_{};
    friend auto encode_logical_body(const LogicalBody&, const CalculatedEvidenceCapacity&) noexcept
        -> std::expected<std::string, LogicalCodecError>;
    friend auto decode_logical_body(std::string_view, const CalculatedEvidenceCapacity&) noexcept
        -> std::expected<LogicalBody, LogicalCodecError>;
};

// Inactive logical-body primitive. It does not select a protocol, transport, process route,
// startup profile, backend, publication path, or live authority.
[[nodiscard]] auto decode_logical_body(
    std::string_view bytes, const CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<LogicalBody, LogicalCodecError>;

[[nodiscard]] auto encode_logical_body(
    const LogicalBody& body, const CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<std::string, LogicalCodecError>;

[[nodiscard]] auto decode_evidence_component(
    EvidenceComponentKind, EvidenceComponentSide, std::string_view,
    const CalculatedEvidenceCapacity&) noexcept -> std::expected<LogicalBody, LogicalCodecError>;

// Registry-specific, still inactive evidence-body admission. This validates the
// complete closed recursive wire-v1 shape and scalar/cardinality domains after
// the bounded canonical parse; it does not construct production evidence.
[[nodiscard]] auto decode_evidence_body(
    std::string_view bytes, const CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<LogicalBody, LogicalCodecError>;

} // namespace epycbench::runtime::wire_v1
