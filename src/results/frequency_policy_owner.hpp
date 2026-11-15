#pragma once
#include "epycbench/monitoring/frequency.hpp"
#include "epycbench/results/journal_records.hpp"
#include "results/topology_evidence_owner.hpp"

namespace epycbench::results::frequency_policy_owner {
inline constexpr std::string_view profile = "epycbench.results.private-frequency-policy-metadata/v1";
enum class Producer { controller, runner };
enum class State { not_attempted, unavailable, failed };
enum class Reason { not_supported, source_missing, access_denied, read_failed, malformed_source, unsupported_value };
struct PolicyInput { const monitoring::FrequencyPolicyMetadata& metadata; const journal::Timestamp& observed_at; };
struct UncoveredInput {
    core::LinuxCpuId linux_cpu;
    State state{State::not_attempted};
    const journal::Timestamp* observed_at{};
    std::optional<Reason> reason{};
    std::optional<std::uint64_t> failure_event{};
};
struct Input {
    const journal::RunId& run;
    std::uint64_t ordinal;
    Producer producer;
    const topology_evidence::Owner& topology;
    std::span<const PolicyInput> policies;
    std::span<const UncoveredInput> uncovered;
};
struct Limits { std::size_t policies, memberships, uncovered, label_bytes, total_label_bytes, canonical_bytes, retained_bytes; };
struct Usage {
    std::size_t policies{}, memberships{}, uncovered{}, label_bytes{}, total_label_bytes{}, canonical_bytes{}, retained_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct Association {
    core::LinuxCpuId linux_cpu;
    std::optional<std::string> stable_pu_id;
    bool topology_online;
};
struct Policy {
    std::string observed_at;
    monitoring::FrequencySourceKind source_kind;
    std::string scaling_driver, scaling_governor;
    monitoring::FrequencyHz scaling_minimum, scaling_maximum;
    std::vector<Association> related_cpus;
    std::vector<core::LinuxCpuId> topology_online_cpus;
};
struct Uncovered {
    Association association;
    State state;
    std::optional<std::string> observed_at;
    std::optional<Reason> reason{};
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
    [[nodiscard]] auto policies() const noexcept -> std::span<const Policy>;
    [[nodiscard]] auto uncovered_cpus() const noexcept -> std::span<const Uncovered>;
    [[nodiscard]] auto topology() const noexcept -> const TopologyBinding*;
    [[nodiscard]] auto private_canonical() const noexcept -> std::string_view { return canonical_; }
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
    std::vector<Policy> policies_;
    std::vector<Uncovered> uncovered_;
    std::optional<TopologyBinding> topology_;
    std::string canonical_;
    core::Sha256Digest digest_{core::sha256(std::string_view{})};
    Usage usage_{};
    friend auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
};
[[nodiscard]] auto retain(const Input&, Limits) -> std::expected<Owner, core::Error>;
#ifdef EPYCBENCH_FREQUENCY_POLICY_OWNER_TESTING
namespace testing { inline std::size_t reload_calls{}, output_calls{}; }
#endif
} // namespace epycbench::results::frequency_policy_owner
