#pragma once
#include "results/process_thp_restriction.hpp"
#include "results/private_topology_serialization.hpp"

namespace epycbench::results::process_thp_restriction_serialization {
inline constexpr std::string_view profile="epycbench.private-process-thp-restriction-artifact/v1";
using Kind=topology_serialization::Kind;
using SlotState=topology_serialization::SlotState;
using Reason=topology_serialization::Reason;
using FailureReference=topology_serialization::FailureReference;
enum class Origin { session_selected,decoded_unassociated };
enum class Error { state,reference,identity,invalid_limits,capacity,encoding,schema,source,child };
struct Quantities {
    std::size_t observations{},error_children{},nodes{},depth{},text_bytes{},canonical_bytes{},provenance_bytes{},
        source_input_bytes{},topology_preparation_bytes{},error_preparation_bytes{},
        source_preparation_bytes{},publication_bytes{},retained_bytes{};
    auto operator==(const Quantities&)const->bool=default;
};
struct Limits:Quantities {process_thp_restriction::Limits source;topology_serialization::Limits topology;error_serialization::Limits child;};
struct Usage:Quantities {
    std::optional<process_thp_restriction::Usage> source;
    std::optional<topology_serialization::Usage> topology;
    std::optional<error_serialization::Usage> preparation_error;
    auto operator==(const Usage&)const->bool=default;
};
struct Provenance {
    std::string run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
    std::optional<FailureReference> failure;std::optional<Reason> reason;
    auto operator==(const Provenance&)const->bool=default;
};
struct ObservationError {
    std::optional<FailureReference> reference;
    std::optional<error_serialization::Owner> error;
};
struct SubjectAssociation {
    process_thp_restriction::Role role;std::uint64_t creation_sequence;
    auto operator==(const SubjectAssociation&)const->bool=default;
};
class Owner;
namespace detail {
struct Binding {
    process_subject::Target target;
    std::optional<FailureReference> reference;
    std::optional<error_serialization::Address> address;
};
struct View {
    std::string_view run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
    SlotState slot_state;std::optional<FailureReference> failure;std::optional<Reason> reason;
};
struct WireError {std::string_view bytes;std::optional<FailureReference> reference;};
struct ObservationView {
    process_thp_restriction::State state{};std::optional<process_thp_restriction::Restriction> value;
    std::string_view time;std::optional<process_thp_restriction::Reason> reason;
    std::optional<std::uint64_t> failure;WireError wire;
};
struct Children {
    std::optional<topology_serialization::Owner> topology;
    std::optional<error_serialization::Owner> preparation_error;
    ObservationError observation;
};
struct Staging {ObservationView observation;Children children;};
class ResolvedInput final {
    const process_thp_restriction::Owner* source;View view;std::optional<SubjectAssociation> subject;
    const Binding* observation;Staging* staging;std::size_t upper_fixed;Usage usage;
    ResolvedInput(const process_thp_restriction::Owner* i,View v,std::optional<SubjectAssociation> a,const Binding* e,Staging* s,std::size_t f)noexcept
        :source(i),view(v),subject(a),observation(e),staging(s),upper_fixed(f){}
    friend class process_subject::Session;friend class process_thp_restriction_serialization::Owner;
  public:
    ResolvedInput(const ResolvedInput&)=delete;ResolvedInput(ResolvedInput&&)=delete;
    auto operator=(const ResolvedInput&)->ResolvedInput& =delete;auto operator=(ResolvedInput&&)->ResolvedInput& =delete;
};
struct State {
    std::optional<process_thp_restriction::Owner> source;Children children;Provenance provenance;SlotState slot_state;std::optional<SubjectAssociation> subject;
    std::string canonical;core::Sha256Digest digest{core::sha256(std::string_view{})};Usage usage;Origin origin;
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
    [[nodiscard]] auto source()const noexcept->const process_thp_restriction::Owner*;
    [[nodiscard]] auto topology()const noexcept->const topology_serialization::Owner*;
    [[nodiscard]] auto failure_error()const noexcept->const error_serialization::Owner*;
    [[nodiscard]] auto observation_error()const noexcept->const ObservationError*;
    [[nodiscard]] auto subject()const noexcept->const SubjectAssociation*;
    [[nodiscard]] auto slot_state()const noexcept->std::optional<SlotState>;
    [[nodiscard]] auto provenance()const noexcept->const Provenance*;
    [[nodiscard]] auto canonical()const noexcept->std::string_view;
    [[nodiscard]] auto digest()const noexcept->const core::Sha256Digest*;
    [[nodiscard]] auto origin()const noexcept->std::optional<Origin>;
    [[nodiscard]] auto usage()const noexcept->Usage;
    [[nodiscard]] static auto decode(std::string_view,Limits)->std::expected<Owner,Error>;
  private:
    std::unique_ptr<detail::State> state_;
    explicit Owner(std::unique_ptr<detail::State>)noexcept;
    static auto before_bindings(const process_thp_restriction::Owner*,const detail::View&,const Limits&,std::size_t)->std::expected<void,Error>;
    static auto preflight(detail::ResolvedInput&,const Limits&)->std::expected<void,Error>;
    static auto child_phase(detail::ResolvedInput&,const Limits&,bool)->std::expected<void,Error>;
    static auto prepare(detail::ResolvedInput&,Limits)->std::expected<Owner,Error>;
    friend class process_subject::Session;
};
} // namespace epycbench::results::process_thp_restriction_serialization
