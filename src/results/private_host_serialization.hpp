#pragma once
#include "results/private_topology_serialization.hpp"
#include "results/private_system_controls_serialization.hpp"
#include "results/private_kernel_identity_serialization.hpp"
#include "results/private_pu_cpu_identity_serialization.hpp"
#include "results/private_frequency_policy_serialization.hpp"
#include "results/private_anonymous_thp_policy_serialization.hpp"
#include "results/private_process_thp_restriction_serialization.hpp"

namespace epycbench::results::host_serialization {
inline constexpr std::string_view profile="epycbench.private-host-artifact/v1";
inline constexpr std::size_t maximum_error_references=18497;
using Kind=topology_serialization::Kind;
enum class Origin { session_selected,decoded_unassociated };
enum class Error { state,reference,identity,invalid_limits,capacity,encoding,schema,source,child };
struct Quantities {
    std::size_t categories{},error_references{},nodes{},depth{},text_bytes{},canonical_bytes{},provenance_bytes{};
    std::array<std::size_t,7> child_preparation_bytes{};
    std::size_t identity_index_bytes{},publication_bytes{},retained_bytes{};
    auto operator==(const Quantities&)const->bool=default;
};
struct Limits:Quantities {
    topology_serialization::Limits topology;
    system_controls_serialization::Limits system_controls;
    kernel_identity_serialization::Limits kernel_identity;
    pu_identity_serialization::Limits pu_identity;
    frequency_policy_serialization::Limits frequency;
    anonymous_thp_policy_serialization::Limits anonymous_thp;
    process_thp_restriction_serialization::Limits process_thp;
};
struct Usage:Quantities {
    topology_serialization::Usage topology;
    system_controls_serialization::Usage system_controls;
    kernel_identity_serialization::Usage kernel_identity;
    pu_identity_serialization::Usage pu_identity;
    frequency_policy_serialization::Usage frequency;
    anonymous_thp_policy_serialization::Usage anonymous_thp;
    process_thp_restriction_serialization::Usage process_thp;
    auto operator==(const Usage&)const->bool=default;
};
struct Provenance {
    std::string run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
    auto operator==(const Provenance&)const->bool=default;
};
class Owner;
namespace detail {
struct View {
    std::string_view run,time;process_subject::Domain domain;process_subject::Producer reporter;Kind kind;std::uint64_t sequence;
};
struct Children {
    std::optional<topology_serialization::Owner> topology;
    std::optional<system_controls_serialization::Owner> system_controls;
    std::optional<kernel_identity_serialization::Owner> kernel_identity;
    std::optional<pu_identity_serialization::Owner> pu_identity;
    std::optional<frequency_policy_serialization::Owner> frequency;
    std::optional<anonymous_thp_policy_serialization::Owner> anonymous_thp;
    std::optional<process_thp_restriction_serialization::Owner> process_thp;
};
struct ErrorIndexRow {
    error_serialization::Kind kind{};std::uint64_t sequence{};
    std::optional<std::uint64_t> ordinal;const error_serialization::Owner* error{};
};
class ResolvedInput final {
    View view;Children* children;std::size_t upper_fixed;Usage usage;
    ResolvedInput(View v,Children* c,std::size_t f)noexcept:view(v),children(c),upper_fixed(f){}
    friend class process_subject::Session;friend class host_serialization::Owner;
  public:
    ResolvedInput(const ResolvedInput&)=delete;ResolvedInput(ResolvedInput&&)=delete;
    auto operator=(const ResolvedInput&)->ResolvedInput& =delete;auto operator=(ResolvedInput&&)->ResolvedInput& =delete;
};
struct State {
    Children children;Provenance provenance;std::string canonical;
    core::Sha256Digest digest{core::sha256(std::string_view{})};Usage usage;Origin origin;
};
[[nodiscard]] auto fixed_stock()noexcept->std::size_t;
[[nodiscard]] auto session_fixed_stock()noexcept->std::size_t;
[[nodiscard]] auto checked_add(std::size_t&,std::size_t)noexcept->bool;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t)noexcept->std::optional<std::size_t>;
// Pure bounded ordering seam; never constructs an Owner or supplies provenance.
void sort_error_index(std::span<ErrorIndexRow>)noexcept;
}
class Owner {
  public:
    ~Owner();Owner(Owner&&)noexcept;auto operator=(Owner&&)noexcept->Owner&;
    Owner(const Owner&)=delete;auto operator=(const Owner&)->Owner& =delete;
    [[nodiscard]] auto topology()const noexcept->const topology_serialization::Owner*;
    [[nodiscard]] auto system_controls()const noexcept->const system_controls_serialization::Owner*;
    [[nodiscard]] auto kernel_identity()const noexcept->const kernel_identity_serialization::Owner*;
    [[nodiscard]] auto pu_identity()const noexcept->const pu_identity_serialization::Owner*;
    [[nodiscard]] auto frequency()const noexcept->const frequency_policy_serialization::Owner*;
    [[nodiscard]] auto anonymous_thp()const noexcept->const anonymous_thp_policy_serialization::Owner*;
    [[nodiscard]] auto process_thp()const noexcept->const process_thp_restriction_serialization::Owner*;
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
    static auto child_phase(detail::ResolvedInput&,const Limits&,std::size_t)->std::expected<void,Error>;
    static auto prepare(detail::ResolvedInput&,Limits)->std::expected<Owner,Error>;
    friend class process_subject::Session;
};
} // namespace epycbench::results::host_serialization
