#pragma once
#include "results/process_subject_facts.hpp"
#include "epycbench/core/digest.hpp"
#include <span>

namespace epycbench::results::process_subject { class Session; }
namespace epycbench::results::error_serialization {
inline constexpr std::string_view profile="epycbench.private-error-artifact/v1";
struct OrdinaryFailure { std::uint64_t sequence; };
struct CreationFailure { std::uint64_t sequence,ordinal; };
struct SourceResolutionFailure { std::uint64_t sequence; };
using Address=std::variant<OrdinaryFailure,CreationFailure,SourceResolutionFailure>;
enum class Kind { ordinary_failure,creation_failure,source_resolution };
enum class Origin { session_selected,decoded_unassociated };
enum class Error { state,reference,identity,invalid_limits,capacity,encoding,schema,source };
struct Quantities {
    std::size_t context_nodes{},context_depth{},string_bytes{},binary_bytes{},targets{},
        nodes{},depth{},text_bytes{},canonical_bytes{},provenance_bytes{},retained_bytes{},consumer_simultaneous_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits=Quantities;
using Usage=Quantities;
struct Association {
    process_subject::Producer producer;
    std::optional<std::uint64_t> process_sequence,secondary_sequence,secondary_creation_ordinal;
    std::vector<process_subject::Target> targets;
    auto operator==(const Association&) const -> bool = default;
};
struct Provenance {
    std::string run,time;
    process_subject::Domain domain;
    process_subject::Producer entry_producer;
    Kind kind;
    std::uint64_t sequence;
    std::optional<std::uint64_t> creation_ordinal;
    std::optional<Association> association;
    auto operator==(const Provenance&) const -> bool = default;
};
class Owner;
namespace detail {
struct View {
    std::string_view run,time;
    process_subject::Domain domain;
    process_subject::Producer entry_producer;
    Kind kind;
    std::uint64_t sequence;
    std::optional<std::uint64_t> creation_ordinal;
    const process_subject::FailureFact* fact;
};
class ResolvedInput final {
    const private_error::Owner* error;
    View view;
    std::size_t upper_fixed;
    ResolvedInput(const private_error::Owner* e,View v,std::size_t f) noexcept:error(e),view(v),upper_fixed(f){}
    friend class process_subject::Session;
    friend class error_serialization::Owner;
  public:
    ResolvedInput(const ResolvedInput&)=delete;
    ResolvedInput(ResolvedInput&&)=delete;
    auto operator=(const ResolvedInput&) -> ResolvedInput& =delete;
    auto operator=(ResolvedInput&&) -> ResolvedInput& =delete;
};
struct State {
    Provenance provenance;
    std::optional<private_error::Owner> error;
    std::string canonical;
    core::Sha256Digest digest{core::sha256(std::string_view{})};
    Origin origin;
    Usage usage;
};
// Complete private Error value only. No Session/artifact provenance is implied.
// Usage is published only on success; bad_alloc propagates to the caller's fixed boundary.
[[nodiscard]] auto encode_value(const private_error::Owner&,Limits,Usage&) -> std::expected<std::string,Error>;
[[nodiscard]] auto decode_value(std::string_view,Limits,Usage&) -> std::expected<private_error::Owner,Error>;
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto decode_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto session_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto checked_add(std::size_t&,std::size_t) noexcept -> bool;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
}
class Owner {
  public:
    ~Owner();
    Owner(const Owner&)=delete;
    auto operator=(const Owner&) -> Owner& =delete;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto error() const noexcept -> const core::Error*;
    [[nodiscard]] auto provenance() const noexcept -> const Provenance*;
    [[nodiscard]] auto origin() const noexcept -> std::optional<Origin>;
    [[nodiscard]] auto usage() const noexcept -> Usage;
    [[nodiscard]] static auto decode(std::string_view,Limits) -> std::expected<Owner,Error>;
  private:
    std::unique_ptr<detail::State> state_;
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
    static auto prepare(const detail::ResolvedInput&,Limits) -> std::expected<Owner,Error>;
    friend class process_subject::Session;
};
} // namespace epycbench::results::error_serialization
