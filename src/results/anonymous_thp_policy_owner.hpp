#pragma once
#include "epycbench/core/quantity.hpp"
#include "epycbench/results/journal_records.hpp"
#include "results/topology_evidence_owner.hpp"
#include <span>
#include <vector>

namespace epycbench::results::anonymous_thp_policy_owner {
inline constexpr std::string_view profile = "epycbench.results.anonymous-thp-size-policy/v1";
enum class Producer { controller, runner };
enum class EnumerationState { not_attempted, complete, unavailable, failed };
enum class State { not_attempted, available, unavailable, failed };
enum class Mode { always, madvise, never, inherit };
enum class Reason { not_supported, source_missing, access_denied, read_failed, malformed_source, unsupported_value };
struct EnumerationInput {
    EnumerationState state{EnumerationState::not_attempted};
    const journal::Timestamp* observed_at{};
    std::optional<Reason> reason{};
    std::optional<std::uint64_t> failure_event{};
};
struct RowInput {
    core::ByteCount size;
    State state{State::not_attempted};
    std::optional<Mode> mode{};
    const journal::Timestamp* observed_at{};
    std::optional<Reason> reason{};
    std::optional<std::uint64_t> failure_event{};
};
struct Input {
    const journal::RunId& run;
    std::uint64_t ordinal;
    Producer producer;
    EnumerationInput enumeration;
    std::span<const RowInput> rows;
    const topology_evidence::Owner* topology{};
};
struct Limits { std::size_t rows, canonical_bytes, retained_bytes; };
struct Usage {
    std::size_t rows{}, canonical_bytes{}, retained_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct Enumeration {
    EnumerationState state{EnumerationState::not_attempted};
    std::optional<std::string> observed_at;
    std::optional<Reason> reason;
    std::optional<std::string> failure_event;
};
struct OwnedRow {
    core::ByteCount size;
    State state;
    std::optional<Mode> mode;
    std::optional<std::string> observed_at;
    std::optional<Reason> reason;
    std::optional<std::string> failure_event;
};
struct TopologyBinding {
    core::Sha256Digest canonical_sha256;
    topology::TopologyFingerprints fingerprints;
    auto operator==(const TopologyBinding&) const -> bool = default;
};
class Owner {
  public:
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    [[nodiscard]] auto enumeration() const noexcept -> const Enumeration*;
    [[nodiscard]] auto rows() const noexcept -> std::span<const OwnedRow>;
    [[nodiscard]] auto topology() const noexcept -> const TopologyBinding*;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view { return canonical_; }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto usage() const noexcept -> Usage { return usage_; }
    [[nodiscard]] auto run_id() const noexcept -> std::string_view { return live_ ? std::string_view{run_} : std::string_view{}; }
    [[nodiscard]] auto producer() const noexcept -> std::optional<Producer> { return live_ ? std::optional{producer_} : std::nullopt; }
    [[nodiscard]] auto ordinal() const noexcept -> std::optional<std::uint64_t> { return live_ ? std::optional{ordinal_} : std::nullopt; }
  private:
    Owner() = default;
    bool live_{true};
    std::string run_;
    Producer producer_{};
    std::uint64_t ordinal_{};
    Enumeration enumeration_;
    std::vector<OwnedRow> rows_;
    std::optional<TopologyBinding> topology_;
    std::string canonical_;
    core::Sha256Digest digest_{core::sha256(std::string_view{})};
    Usage usage_{};
    friend auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
};
[[nodiscard]] auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
#ifdef EPYCBENCH_ANONYMOUS_THP_POLICY_OWNER_TESTING
namespace testing { inline std::size_t output_calls{}; }
#endif
} // namespace epycbench::results::anonymous_thp_policy_owner
