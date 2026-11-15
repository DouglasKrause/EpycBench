#pragma once
#include "epycbench/results/journal_records.hpp"
#include "results/topology_evidence_owner.hpp"

namespace epycbench::results::process_thp_restriction {
inline constexpr std::string_view profile = "epycbench.results.process-thp-disable/v1";
enum class Producer { controller, runner };
enum class Role { controller, runner };
enum class State { not_attempted, available, unavailable, failed };
enum class Restriction { not_disabled, disabled };
enum class Reason { not_supported, source_missing, access_denied, read_failed, malformed_source, unsupported_value };
struct SubjectInput { Role role; std::uint64_t lifecycle_event_sequence; };
struct ObservationInput {
    State state{State::not_attempted};
    std::optional<Restriction> value{};
    const journal::Timestamp* observed_at{};
    std::optional<Reason> reason{};
    std::optional<std::uint64_t> failure_event{};
};
struct Input {
    const journal::RunId& run;
    std::uint64_t ordinal;
    Producer producer;
    SubjectInput subject;
    ObservationInput observation;
    const topology_evidence::Owner* topology{};
};
struct Limits { std::size_t observations, canonical_bytes, retained_bytes; };
struct Usage {
    std::size_t observations{}, canonical_bytes{}, retained_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct Subject { Role role{Role::controller}; std::string event_id; };
struct Observation {
    State state{State::not_attempted};
    std::optional<Restriction> value;
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
    [[nodiscard]] auto subject() const noexcept -> const Subject*;
    [[nodiscard]] auto observation() const noexcept -> const Observation*;
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
    Subject subject_;
    Observation observation_;
    std::optional<TopologyBinding> topology_;
    std::string canonical_;
    core::Sha256Digest digest_{core::sha256(std::string_view{})};
    Usage usage_{};
    friend auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
};
[[nodiscard]] auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
#ifdef EPYCBENCH_PROCESS_THP_RESTRICTION_TESTING
namespace testing { inline std::size_t output_calls{}; }
#endif
} // namespace epycbench::results::process_thp_restriction
