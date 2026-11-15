#pragma once
#include "results/host_system_controls.hpp"
#include "results/private_topology_serialization.hpp"

namespace epycbench::results::system_controls_serialization {
inline constexpr std::string_view profile="epycbench.private-system-controls-artifact/v1";
using Kind=topology_serialization::Kind;
using SlotState=topology_serialization::SlotState;
using Reason=topology_serialization::Reason;
using FailureReference=topology_serialization::FailureReference;
enum class Origin { session_selected,decoded_unassociated };
enum class Error { state,reference,identity,invalid_limits,capacity,encoding,schema,source,child };
struct Quantities {
    std::size_t nodes{},depth{},text_bytes{},canonical_bytes{},provenance_bytes{},controls_input_bytes{},
        topology_preparation_bytes{},error_preparation_bytes{},controls_preparation_bytes{},publication_bytes{},retained_bytes{};
    auto operator==(const Quantities&)const->bool=default;
};
struct Limits:Quantities {host_controls::Limits controls;topology_serialization::Limits topology;error_serialization::Limits child;};
struct Usage:Quantities {
    std::optional<host_controls::Usage> controls;
    std::optional<topology_serialization::Usage> topology;
    std::array<std::optional<error_serialization::Usage>,6> errors;
    auto operator==(const Usage&)const->bool=default;
};
struct Provenance {
    std::string run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
    std::array<std::optional<FailureReference>,6> failures;std::optional<Reason> reason;
    auto operator==(const Provenance&)const->bool=default;
};
class Owner;
namespace detail {
struct View {
    std::string_view run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
    SlotState slot_state;std::array<std::optional<FailureReference>,6> failures;std::optional<Reason> reason;
};
struct Children {std::optional<topology_serialization::Owner> topology;std::array<std::optional<error_serialization::Owner>,6> errors;};
class ResolvedInput final {
    const host_controls::Owner* controls;View view;Children* children;std::size_t upper_fixed;Usage usage;
    ResolvedInput(const host_controls::Owner* c,View v,Children* p,std::size_t f)noexcept:controls(c),view(v),children(p),upper_fixed(f){}
    friend class process_subject::Session;friend class system_controls_serialization::Owner;
  public:
    ResolvedInput(const ResolvedInput&)=delete;ResolvedInput(ResolvedInput&&)=delete;
    auto operator=(const ResolvedInput&)->ResolvedInput& =delete;auto operator=(ResolvedInput&&)->ResolvedInput& =delete;
};
struct State {
    std::optional<host_controls::Owner> controls;Children children;Provenance provenance;SlotState slot_state;
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
    [[nodiscard]] auto controls()const noexcept->const host_controls::Owner*;
    [[nodiscard]] auto topology()const noexcept->const topology_serialization::Owner*;
    [[nodiscard]] auto failure_error()const noexcept->const error_serialization::Owner*;
    [[nodiscard]] auto observation_error(std::size_t)const noexcept->const error_serialization::Owner*;
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
    static auto preflight(detail::ResolvedInput&,const Limits&)->std::expected<void,Error>;
    static auto child_phase(detail::ResolvedInput&,const Limits&,bool)->std::expected<void,Error>;
    static auto prepare(detail::ResolvedInput&,Limits)->std::expected<Owner,Error>;
    friend class process_subject::Session;
};
} // namespace epycbench::results::system_controls_serialization
