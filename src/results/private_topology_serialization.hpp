#pragma once
#include "results/topology_evidence_owner.hpp"
#include "results/private_error_serialization.hpp"

namespace epycbench::results::topology_serialization {
inline constexpr std::string_view profile="epycbench.private-topology-artifact/v1";
enum class Kind { ordinary,genesis };
enum class SlotState { not_attempted,unavailable,failed,retained };
enum class Reason { not_supported,source_missing,access_denied };
enum class Origin { session_selected,decoded_unassociated };
enum class Error { state,reference,identity,invalid_limits,capacity,encoding,schema,source,child };
struct Quantities {
    std::size_t nodes{},depth{},text_bytes{},canonical_bytes{},provenance_bytes{},
        topology_decode_bytes{},topology_preparation_bytes{},error_preparation_bytes{},publication_bytes{},retained_bytes{};
    auto operator==(const Quantities&)const->bool=default;
};
struct Limits:Quantities {topology_evidence::Limits topology;error_serialization::Limits child;};
struct Usage:Quantities {
    std::optional<topology_evidence::Usage> topology;
    std::optional<error_serialization::Usage> child;
    auto operator==(const Usage&)const->bool=default;
};
struct FailureReference {std::uint64_t sequence;std::optional<std::uint64_t> creation_ordinal;auto operator==(const FailureReference&)const->bool=default;};
struct Provenance {
    std::string run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
    std::optional<FailureReference> failure;std::optional<Reason> reason;
    auto operator==(const Provenance&)const->bool=default;
};
class Owner;
namespace detail {
struct View {
    std::string_view run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
    SlotState slot_state;std::optional<FailureReference> failure;std::optional<Reason> reason;
};
class ResolvedInput final {
    const topology_evidence::Owner* topology;View view;
    std::optional<error_serialization::Owner>* child;std::size_t upper_fixed;
    ResolvedInput(const topology_evidence::Owner* m,View v,std::optional<error_serialization::Owner>* e,std::size_t f)noexcept:topology(m),view(v),child(e),upper_fixed(f){}
    friend class process_subject::Session;friend class topology_serialization::Owner;
  public:
    ResolvedInput(const ResolvedInput&)=delete;ResolvedInput(ResolvedInput&&)=delete;
    auto operator=(const ResolvedInput&)->ResolvedInput& =delete;
    auto operator=(ResolvedInput&&)->ResolvedInput& =delete;
};
struct State {
    std::optional<topology_evidence::Owner> topology;std::optional<error_serialization::Owner> child;
    Provenance provenance;SlotState slot_state;
    std::string canonical;core::Sha256Digest digest{core::sha256(std::string_view{})};
    Usage usage;Origin origin;
};
[[nodiscard]] auto fixed_stock()noexcept->std::size_t;
[[nodiscard]] auto session_fixed_stock()noexcept->std::size_t;
[[nodiscard]] auto checked_add(std::size_t&,std::size_t)noexcept->bool;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t)noexcept->std::optional<std::size_t>;
}
class Owner {
  public:
    ~Owner();Owner(Owner&&)noexcept;auto operator=(Owner&&)noexcept->Owner&;
    Owner(const Owner&)=delete;auto operator=(const Owner&)->Owner& =delete;
    [[nodiscard]] auto topology()const noexcept->const topology_evidence::Owner*;
    [[nodiscard]] auto failure_error()const noexcept->const error_serialization::Owner*;
    [[nodiscard]] auto slot_state()const noexcept->std::optional<SlotState>;
    [[nodiscard]] auto reason()const noexcept->std::optional<Reason>;
    [[nodiscard]] auto provenance()const noexcept->const Provenance*;
    [[nodiscard]] auto canonical()const noexcept->std::string_view;
    [[nodiscard]] auto digest()const noexcept->const core::Sha256Digest*;
    [[nodiscard]] auto origin()const noexcept->std::optional<Origin>;
    [[nodiscard]] auto usage()const noexcept->Usage;
    [[nodiscard]] static auto decode(std::string_view,Limits)->std::expected<Owner,Error>;
  private:
    std::unique_ptr<detail::State> state_;
    explicit Owner(std::unique_ptr<detail::State>)noexcept;
    static auto preflight(const detail::ResolvedInput&,const Limits&)->std::expected<void,Error>;
    static auto prepare(const detail::ResolvedInput&,Limits)->std::expected<Owner,Error>;
    friend class process_subject::Session;
};
} // namespace epycbench::results::topology_serialization
