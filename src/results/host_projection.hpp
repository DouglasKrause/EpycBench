#pragma once
#include "results/host_evidence_assembly.hpp"
#include <compare>
#include <memory>
#include <span>
#include <vector>

namespace epycbench::results::process_subject { class Session; }
namespace epycbench::results::host_projection {
inline constexpr std::string_view profile = "epycbench.results.reduced-host/v1";
enum class SourceDomain { supplied_observation, synthetic_execution };
enum class SourceOrigin { ordinary, genesis };
enum class Producer { controller, runner, results };
enum class Role { controller, runner };
enum class Category { topology, system_controls, kernel_identity, pu_identity, frequency, anonymous_thp, process_thp };
enum class Selector { owner_preparation, smt_control, smt_active, numa_mask, thp_enabled, thp_defrag,
    sysname, release, version, signature, microcode, uncovered_cpu, enumeration, page_size, process_observation };
enum class FailureReason { none, read_failed, malformed_source, unsupported_value };
struct FactKey {
    Category category;
    Selector selector{Selector::owner_preparation};
    std::uint64_t ordinal{}, key{};
    FailureReason reason{FailureReason::none};
    auto operator<=>(const FactKey&) const = default;
};
struct ResolvedFailure {
    FactKey target;
    std::uint64_t sequence;
    std::optional<std::uint64_t> creation_ordinal;
    Producer producer;
    std::optional<std::uint64_t> process_sequence;
    auto operator==(const ResolvedFailure&) const -> bool = default;
};
struct ResolvedSubject {
    Role role;
    std::uint64_t creation_sequence;
    auto operator==(const ResolvedSubject&) const -> bool = default;
};
struct Limits {
    topology_evidence::Limits reload;
    std::size_t failure_references, sort_entries, nodes, depth, text_bytes, canonical_bytes,
        retained_bytes, consumer_simultaneous_bytes;
};
struct Usage {
    topology_evidence::Usage reload;
    std::size_t failure_references{}, sort_entries{}, nodes{}, depth{}, text_bytes{}, canonical_bytes{},
        retained_bytes{}, consumer_simultaneous_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
enum class Error { state, reference, invalid_limits, capacity, source, rendering };
enum class Disposition { not_attempted, unavailable, failed, retained };
struct PrivateProvenance {
    std::string run, handoff_time;
    SourceDomain domain;
    SourceOrigin origin;
    Producer producer;
    std::uint64_t sequence;
    std::array<Disposition, 7> slots{};
    std::array<std::optional<core::Sha256Digest>, 7> child_digests;
    std::optional<topology::TopologyFingerprints> topology;
    std::vector<ResolvedFailure> failures;
    std::optional<ResolvedSubject> subject;
};
class Owner;
namespace detail {
class ResolvedInput final {
    const host_assembly::Owner& host;
    std::string_view run, handoff_time;
    SourceDomain domain;
    SourceOrigin origin;
    Producer producer;
    std::uint64_t sequence;
    std::optional<ResolvedSubject> subject;
    std::span<const ResolvedFailure> failures;
    std::size_t session_fixed_bytes, resolved_table_bytes;
    ResolvedInput(const host_assembly::Owner& h, std::string_view r, std::string_view t,
        SourceDomain d, SourceOrigin o, Producer p, std::uint64_t s,
        std::optional<ResolvedSubject> subject_value, std::span<const ResolvedFailure> f,
        std::size_t fixed, std::size_t table) noexcept
        : host(h), run(r), handoff_time(t), domain(d), origin(o), producer(p), sequence(s),
          subject(subject_value), failures(f), session_fixed_bytes(fixed), resolved_table_bytes(table) {}
    friend class process_subject::Session;
    friend class host_projection::Owner;
  public:
    ResolvedInput(const ResolvedInput&) = delete;
    auto operator=(const ResolvedInput&) -> ResolvedInput& = delete;
    ResolvedInput(ResolvedInput&&) = delete;
    auto operator=(ResolvedInput&&) -> ResolvedInput& = delete;
};
struct State {
    PrivateProvenance provenance;
    std::string canonical;
    core::Sha256Digest digest{core::sha256(std::string_view{})};
    Usage usage;
};
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto session_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto valid_limits(const Limits&) noexcept -> bool;
}
class Owner {
  public:
    ~Owner();
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    [[nodiscard]] auto reduced_canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto reduced_digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto private_provenance() const noexcept -> const PrivateProvenance*;
    [[nodiscard]] auto usage() const noexcept -> Usage;
  private:
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
    static auto prepare(const detail::ResolvedInput&, const Limits&) -> std::expected<Owner, Error>;
    std::unique_ptr<detail::State> state_;
    friend class process_subject::Session;
};
} // namespace epycbench::results::host_projection
