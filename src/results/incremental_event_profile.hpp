#pragma once

#include "results/private_run_creation_serialization.hpp"
#include "results/private_normalization_serialization.hpp"
#include "results/private_source_outcome_serialization.hpp"
#include <variant>

namespace epycbench::results::validation_event { class Owner; }
namespace epycbench::results::producer_progress { class Adapter; }
namespace epycbench::results::native_lifecycle { class Adapter; }
namespace epycbench::results::preparation_observation { class Adapter; }
namespace epycbench::results::event_profile {
inline constexpr std::string_view schema_name = "epycbench.event-journal/v1";
inline constexpr std::string_view progress_schema_name = "epycbench.event-journal/v2";
inline constexpr std::string_view compact_schema_name = "epycbench.event-journal/v3";
enum class Version : unsigned char { supplied_v1, progress_v2, compact_v3 };
[[nodiscard]] inline constexpr auto is_progress(Version version) noexcept -> bool {
    return version == Version::progress_v2 || version == Version::compact_v3;
}
[[nodiscard]] inline constexpr auto schema_for(Version version) noexcept -> std::string_view {
    switch (version) {
    case Version::supplied_v1: return schema_name;
    case Version::progress_v2: return progress_schema_name;
    case Version::compact_v3: return compact_schema_name;
    }
    return {};
}
enum class AvailabilitySource : unsigned char { producer_callback, controller_observation, supplied_process_fact };
struct Availability {
    AvailabilitySource source{};
    std::uint64_t operation{}, transition{}, delivery{};
    std::optional<std::uint64_t> quiescence_epoch;
    bool native_platform{}, acquired_utc{};
    auto operator==(const Availability&) const -> bool = default;
};
// Stack-borrowed admission, issued only inside the bound Results callback.
// Decoded Availability is factual metadata and cannot construct this capability.
class ProducerAdmission final {
  public:
    ProducerAdmission(const ProducerAdmission&) = delete;
    [[nodiscard]] auto availability() const noexcept -> const Availability& { return availability_; }
  private:
    explicit ProducerAdmission(Availability value) noexcept : availability_{value} {}
    Availability availability_;
    friend class producer_progress::Adapter;
    friend class native_lifecycle::Adapter;
    friend class preparation_observation::Adapter;
};
enum class Kind { creation, normalization, source_outcome, topology, failure,
    planning,preflight,pair_started,request_accepted,pair_terminal,event_eof,child_reaped,lifecycle_progress,
    placement,preparation,calibration,warmup,block_opened,block_closed,observation,failed_observation,
    monitoring,stopping,analysis,finalization,run_closed };
enum class RefKind { creation, topology, creation_failure, ordinary_failure, source_resolution_failure };
enum class Role { primary, secondary };
enum class Error { state, limits, capacity, framing, schema, canonical, identity, sequence,
    digest, child, reference, association, unsupported };
struct Reference {
    std::uint64_t sequence{}, ordinal{};
    RefKind kind{};
    auto operator==(const Reference&) const -> bool = default;
};
using Artifact = std::variant<run_creation_serialization::Owner,
    normalization_serialization::Owner, source_outcome_serialization::Owner,
    topology_serialization::Owner, error_serialization::Owner, std::monostate>;
using ArtifactView = std::variant<const run_creation_serialization::Owner*,
    const normalization_serialization::Owner*, const source_outcome_serialization::Owner*,
    const topology_serialization::Owner*, const error_serialization::Owner*>;
struct Input {
    const journal::Timestamp& timestamp;
    ArtifactView artifact;
    std::optional<Reference> creation, topology, primary;
    Role role{Role::primary};
};
struct Scope {
    std::optional<std::uint64_t> experiment,pair,block,observation;
    auto operator==(const Scope&) const -> bool = default;
};
struct SuppliedEvent {
    const journal::Timestamp& timestamp;
    Kind kind;
    process_subject::Producer producer;
    Scope scope;
    const nlohmann::json& facts;
    const ProducerAdmission* admission{};
};
struct CompositionLimits;
struct ChildLimits {
    run_creation_serialization::Limits creation;
    normalization_serialization::Limits normalization;
    source_outcome_serialization::Limits source_outcome;
    topology_serialization::Limits topology;
    error_serialization::Limits error;
};
struct Limits {
    std::size_t body_bytes{}, nodes{}, depth{}, text_bytes{}, canonical_bytes{},
        origin_entries{}, references{}, retained_bytes{}, candidate_bytes{},
        simultaneous_bytes{}, work{};
    std::uint64_t records{};
    ChildLimits child;
    std::shared_ptr<const CompositionLimits> composition{};
    Version version{Version::supplied_v1};
};
struct Usage {
    std::size_t canonical_bytes{}, nodes{}, text_bytes{}, origin_entries{}, references{},
        retained_bytes{}, candidate_bytes{}, simultaneous_bytes{}, work{};
    auto operator==(const Usage&) const -> bool = default;
};
struct Record {
    std::uint64_t sequence{};
    Kind kind{};
    std::string timestamp, canonical;
    core::Sha256Digest digest;
    Artifact artifact;
    std::optional<Reference> creation, topology, primary;
    Role role{Role::primary};
    std::shared_ptr<const nlohmann::json> event_facts{};
    Scope scope;
    process_subject::Producer producer{process_subject::Producer::controller};
    Version version{Version::supplied_v1};
    std::optional<Availability> availability;
};
namespace detail {
struct Shape {
    std::size_t nodes{}, members{}, text{}, depth{}, bytes{};
    std::size_t maximum_token_bytes{32}, key_text_bytes{};
};
// Complete Error values remain encoded child strings. Authoring and replay use
// this same closed semantic coupling; the outer v2 envelope is unchanged.
[[nodiscard]] auto preparation_failure_matches(const core::Error&,std::string_view target,
    std::optional<bool> cleanup={}) noexcept -> bool;
[[nodiscard]] auto controller_check_snapshot_valid(const nlohmann::json&,const error_serialization::Limits&) noexcept -> bool;
[[nodiscard]] auto controller_check_failure_batch(const nlohmann::json&,const core::Error*,
    std::span<const core::Error>) noexcept -> bool;
[[nodiscard]] auto controller_check_snapshot_matches(const nlohmann::json&,const validation_event::Owner&,
    const nlohmann::json& temperatures) noexcept -> bool;
struct Work {
    std::size_t used{}, limit{};
    std::size_t peak{}, peak_limit{SIZE_MAX};
    void reserve_peak(std::size_t);
    void debit(std::size_t);
    [[nodiscard]] auto equal(std::string_view, std::string_view) -> bool;
};
struct Candidate { Record record; Shape shape; std::size_t child_retained{}, child_peak{}; };
[[nodiscard]] auto add(std::size_t, std::size_t) -> std::size_t;
[[nodiscard]] auto multiply(std::size_t, std::size_t) -> std::size_t;
void require(bool, Error);
[[nodiscard]] auto shape(std::string_view, const Limits&, Work&) -> Shape;
[[nodiscard]] auto decode(std::string_view, std::string_view, std::uint64_t,
    const std::optional<core::Sha256Digest>&, const Limits&, Work&) -> Candidate;
[[nodiscard]] auto encode(const Input&, std::string_view, std::uint64_t,
    const std::optional<core::Sha256Digest>&, const Limits&, Work&) -> std::string;
[[nodiscard]] auto encode(const SuppliedEvent&, std::string_view, std::uint64_t,
    const std::optional<core::Sha256Digest>&, const Limits&, Work&) -> std::string;
[[nodiscard]] auto event_name(Kind) -> std::string_view;
[[nodiscard]] auto child_allowance(Kind, const ChildLimits&) -> std::size_t;
[[nodiscard]] auto json_stock(const Shape&) -> std::size_t;
[[nodiscard]] auto workspace(const Shape&) -> std::size_t;
[[nodiscard]] auto dom_workspace(const Shape&, std::size_t depth) -> std::size_t;
// Caller installs JsonCleanup immediately on the returned bounded tree.
[[nodiscard]] auto parse_tree(std::string_view, const Limits&, Work&) -> nlohmann::json;
// DOM-only construction for an already validated canonical source; the same
// guarded Builder owns partial trees without canonicalization/preimage stock.
[[nodiscard]] auto parse_tree_dom(std::string_view, const Limits&, Work&) -> nlohmann::json;
// The v3 journal stores only these two fixed parent artifacts with their known
// hex leaves as exact UTF-8 strings. Original child codecs and octets are not
// changed. Legacy string input remains borrowed; compact input is reconstructed
// into the caller's empty string before ordinary child admission.
[[nodiscard]] auto compact_artifact(Kind, std::string_view, const Limits&, Work&,
    std::size_t overlap = 0) -> nlohmann::json;
[[nodiscard]] auto artifact_bytes(Kind, const nlohmann::json&, Version,
    const Limits&, Work&, std::string& reconstructed, std::size_t overlap = 0) -> std::string_view;
}
} // namespace epycbench::results::event_profile
