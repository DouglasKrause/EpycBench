#pragma once
#include "results/frequency_policy_owner.hpp"
#include "results/private_topology_serialization.hpp"

namespace epycbench::results::frequency_policy_serialization {
inline constexpr std::string_view profile="epycbench.private-frequency-policy-artifact/v1";
using Kind=topology_serialization::Kind;
using SlotState=topology_serialization::SlotState;
using Reason=topology_serialization::Reason;
using FailureReference=topology_serialization::FailureReference;
enum class Origin { session_selected,decoded_unassociated };
enum class Error { state,reference,identity,invalid_limits,capacity,encoding,schema,source,child };
struct Quantities {
    std::size_t policies{},memberships{},online_memberships{},uncovered{},error_children{},nodes{},depth{},text_bytes{},canonical_bytes{},provenance_bytes{},
        binding_bytes{},source_input_bytes{},row_staging_bytes{},identity_index_bytes{},topology_preparation_bytes{},error_preparation_bytes{},
        source_preparation_bytes{},publication_bytes{},retained_bytes{};
    auto operator==(const Quantities&)const->bool=default;
};
struct Limits:Quantities {frequency_policy_owner::Limits source;topology_serialization::Limits topology;error_serialization::Limits child;};
struct Usage:Quantities {
    std::optional<frequency_policy_owner::Usage> source;
    std::optional<topology_serialization::Usage> topology;
    std::optional<error_serialization::Usage> preparation_error;
    auto operator==(const Usage&)const->bool=default;
};
struct Provenance {
    std::string run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
    std::optional<FailureReference> failure;std::optional<Reason> reason;
    auto operator==(const Provenance&)const->bool=default;
};
struct ErrorRow {
    core::LinuxCpuId cpu{0};
    std::optional<FailureReference> reference;
    std::optional<error_serialization::Owner> error;
};
class Owner;
namespace detail {
struct Binding {
    process_subject::Target target;
    std::optional<FailureReference> reference;
    std::optional<error_serialization::Address> address;
};
struct BindingRow {core::LinuxCpuId cpu{0};Binding failure;};
struct View {
    std::string_view run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
    SlotState slot_state;std::optional<FailureReference> failure;std::optional<Reason> reason;
};
struct AssociationView {core::LinuxCpuId cpu{0};std::optional<std::string_view> stable;bool online{};};
struct LabelView {std::string_view encoded;std::size_t size{};};
struct PolicyView {
    std::string_view time;monitoring::FrequencySourceKind kind{};LabelView driver,governor;
    std::uint64_t minimum{},maximum{};std::size_t member_begin{},member_count{},online_begin{},online_count{};
};
struct WireError {std::string_view bytes;std::optional<FailureReference> reference;};
struct UncoveredView {
    AssociationView association;frequency_policy_owner::State state{};std::string_view time;
    std::optional<frequency_policy_owner::Reason> reason;std::optional<std::uint64_t> failure;WireError wire;
};
struct IdentityRow {FailureReference reference;std::size_t row;};
struct Tables {
    std::vector<PolicyView> policies;std::vector<AssociationView> members;
    std::vector<core::LinuxCpuId> online;std::vector<UncoveredView> uncovered;
};
struct Children {
    std::optional<topology_serialization::Owner> topology;
    std::optional<error_serialization::Owner> preparation_error;
    std::vector<ErrorRow> rows;
};
struct Staging {Tables tables;std::vector<IdentityRow> index;Children children;};
class ResolvedInput final {
    const frequency_policy_owner::Owner* source;View view;std::span<const BindingRow> bindings;
    Staging* staging;std::size_t upper_fixed,upper_dynamic;Usage usage;
    ResolvedInput(const frequency_policy_owner::Owner* i,View v,std::span<const BindingRow> b,Staging* s,std::size_t f,std::size_t d)noexcept
        :source(i),view(v),bindings(b),staging(s),upper_fixed(f),upper_dynamic(d){}
    friend class process_subject::Session;friend class frequency_policy_serialization::Owner;
  public:
    ResolvedInput(const ResolvedInput&)=delete;ResolvedInput(ResolvedInput&&)=delete;
    auto operator=(const ResolvedInput&)->ResolvedInput& =delete;auto operator=(ResolvedInput&&)->ResolvedInput& =delete;
};
struct State {
    std::optional<frequency_policy_owner::Owner> source;Children children;Provenance provenance;SlotState slot_state;
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
    [[nodiscard]] auto source()const noexcept->const frequency_policy_owner::Owner*;
    [[nodiscard]] auto topology()const noexcept->const topology_serialization::Owner*;
    [[nodiscard]] auto failure_error()const noexcept->const error_serialization::Owner*;
    [[nodiscard]] auto errors()const noexcept->std::span<const ErrorRow>;
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
    static auto before_bindings(const frequency_policy_owner::Owner*,const detail::View&,const Limits&,std::size_t)->std::expected<std::size_t,Error>;
    static auto preflight(detail::ResolvedInput&,const Limits&)->std::expected<void,Error>;
    static auto child_phase(detail::ResolvedInput&,const Limits&,bool)->std::expected<void,Error>;
    static auto prepare(detail::ResolvedInput&,Limits)->std::expected<Owner,Error>;
    friend class process_subject::Session;
};
} // namespace epycbench::results::frequency_policy_serialization
