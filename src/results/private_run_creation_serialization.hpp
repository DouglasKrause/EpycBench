#pragma once
#include "results/private_source_serialization.hpp"
#include "results/private_base_build_serialization.hpp"
#include "results/private_host_serialization.hpp"

namespace epycbench::results::run_creation_serialization {
inline constexpr std::string_view profile="epycbench.private-run-creation-artifact/v1";
inline constexpr std::size_t maximum_failures=1024,maximum_targets=65536,maximum_bindings=65536,maximum_consumed=18499;
enum class Origin { session_selected,decoded_unassociated };
enum class Error { state,reference,identity,invalid_limits,capacity,encoding,schema,source,child };
struct Binding {process_subject::Target target;std::uint64_t failure_ordinal;auto operator==(const Binding&)const->bool=default;};
struct Quantities {
    std::size_t artifact_slots{},failure_facts{},targets{},bindings{},consumed_references{},nodes{},depth{},text_bytes{},canonical_bytes{},provenance_bytes{},
        table_preparation_bytes{},error_preparation_bytes{};
    std::array<std::size_t,3> child_preparation_bytes{};
    std::size_t identity_index_bytes{},publication_bytes{},retained_bytes{};
    auto operator==(const Quantities&)const->bool=default;
};
struct Limits:Quantities {source_serialization::Limits source;base_build_serialization::Limits base_build;host_serialization::Limits host;error_serialization::Limits error;};
struct Usage:Quantities {
    source_serialization::Usage source;base_build_serialization::Usage base_build;host_serialization::Usage host;
    error_serialization::Usage root_errors;
    auto operator==(const Usage&)const->bool=default;
};
struct Provenance {std::string run,time;process_subject::Domain domain;process_subject::Producer reporter;std::uint64_t sequence;auto operator==(const Provenance&)const->bool=default;};
class Owner;
namespace detail {
struct View {std::string_view run,time;process_subject::Domain domain;process_subject::Producer reporter;std::uint64_t sequence;};
struct Children {std::optional<source_serialization::Owner> source;std::optional<base_build_serialization::Owner> base_build;std::optional<host_serialization::Owner> host;std::vector<error_serialization::Owner> failures;std::vector<Binding> bindings;};
struct TargetPosition {std::size_t failure,target;};
struct Staging {Children children;std::vector<TargetPosition> targets;std::vector<std::size_t> binding_index;std::vector<std::string_view> wire_errors;};
class ResolvedInput final {
    View view;std::size_t failures,bindings,upper_fixed;Staging* staging;Usage usage;
    ResolvedInput(View v,std::size_t n,std::size_t b,Staging* s,std::size_t f)noexcept:view(v),failures(n),bindings(b),upper_fixed(f),staging(s){}
    friend class process_subject::Session;friend class run_creation_serialization::Owner;
  public:
    ResolvedInput(const ResolvedInput&)=delete;ResolvedInput(ResolvedInput&&)=delete;
    auto operator=(const ResolvedInput&)->ResolvedInput& =delete;auto operator=(ResolvedInput&&)->ResolvedInput& =delete;
};
struct State {Children children;Provenance provenance;std::string canonical;core::Sha256Digest digest{core::sha256(std::string_view{})};Usage usage;Origin origin;};
[[nodiscard]] auto fixed_stock()noexcept->std::size_t;
[[nodiscard]] auto session_fixed_stock()noexcept->std::size_t;
[[nodiscard]] auto checked_add(std::size_t&,std::size_t)noexcept->bool;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t)noexcept->std::optional<std::size_t>;
}
class Owner {
  public:
    ~Owner();Owner(Owner&&)noexcept;auto operator=(Owner&&)noexcept->Owner&;
    Owner(const Owner&)=delete;auto operator=(const Owner&)->Owner& =delete;
    [[nodiscard]] auto source()const noexcept->const source_serialization::Owner*;
    [[nodiscard]] auto base_build()const noexcept->const base_build_serialization::Owner*;
    [[nodiscard]] auto host()const noexcept->const host_serialization::Owner*;
    [[nodiscard]] auto failures()const noexcept->std::span<const error_serialization::Owner>;
    [[nodiscard]] auto failure_at(std::uint64_t)const noexcept->const error_serialization::Owner*;
    [[nodiscard]] auto bindings()const noexcept->std::span<const Binding>;
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
    static auto error_phase(detail::ResolvedInput&,const Limits&)->std::expected<void,Error>;
    static auto index_phase(detail::ResolvedInput&,const Limits&)->std::expected<void,Error>;
    static auto child_phase(detail::ResolvedInput&,const Limits&,std::size_t)->std::expected<void,Error>;
    static auto prepare(detail::ResolvedInput&,Limits)->std::expected<Owner,Error>;
    friend class process_subject::Session;
};
} // namespace epycbench::results::run_creation_serialization
