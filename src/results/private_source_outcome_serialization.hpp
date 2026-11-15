#pragma once
#include "results/private_source_serialization.hpp"
namespace epycbench::results::source_outcome_serialization {
inline constexpr std::string_view profile="epycbench.private-nonresolved-source-outcome/v1";
enum class Origin { session_selected,decoded_unassociated };
enum class Error { state,reference,identity,invalid_limits,capacity,encoding,schema,source,child };
struct Quantities {
    std::size_t diagnostics{},diagnostic_bytes{},nodes{},depth{},text_bytes{},canonical_bytes{},
        provenance_bytes{},source_preparation_bytes{},upstream_preparation_bytes{},publication_bytes{},retained_bytes{};
    auto operator==(const Quantities&)const->bool=default;
};
struct Limits:Quantities {source_serialization::Limits source;error_serialization::Limits upstream;};
struct Usage:Quantities {source_serialization::Usage source;std::optional<error_serialization::Usage> upstream;auto operator==(const Usage&)const->bool=default;};
struct Provenance {std::string run,time;process_subject::Domain domain;std::uint64_t sequence;auto operator==(const Provenance&)const->bool=default;};
class Owner;
namespace detail {
struct View {std::string_view run,time;process_subject::Domain domain;std::uint64_t sequence;};
class ResolvedInput final {
    const authored_source::ResolutionRecord* record;View view;
    std::optional<source_serialization::Owner>* source;std::optional<error_serialization::Owner>* upstream;
    std::size_t upper_fixed;
    ResolvedInput(const authored_source::ResolutionRecord* r,View v,std::optional<source_serialization::Owner>* s,std::optional<error_serialization::Owner>* e,std::size_t f)noexcept:record(r),view(v),source(s),upstream(e),upper_fixed(f){}
    friend class process_subject::Session;friend class source_outcome_serialization::Owner;
  public:
    ResolvedInput(const ResolvedInput&)=delete;ResolvedInput(ResolvedInput&&)=delete;
    auto operator=(const ResolvedInput&)->ResolvedInput& =delete;auto operator=(ResolvedInput&&)->ResolvedInput& =delete;
};
struct State {
    Provenance provenance;authored_source::State outcome{};std::vector<std::string> diagnostics;
    std::optional<source_serialization::Owner> source;std::optional<error_serialization::Owner> upstream;
    std::string canonical;core::Sha256Digest digest{core::sha256(std::string_view{})};Usage usage;Origin origin;
};
[[nodiscard]] auto fixed_stock()noexcept->std::size_t;
[[nodiscard]] auto session_fixed_stock()noexcept->std::size_t;
}
class Owner {
  public:
    ~Owner();Owner(Owner&&)noexcept;auto operator=(Owner&&)noexcept->Owner&;
    Owner(const Owner&)=delete;auto operator=(const Owner&)->Owner& =delete;
    [[nodiscard]] auto provenance()const noexcept->const Provenance*;
    [[nodiscard]] auto outcome()const noexcept->std::optional<authored_source::State>;
    [[nodiscard]] auto diagnostics()const noexcept->std::span<const std::string>;
    [[nodiscard]] auto source()const noexcept->const source_serialization::Owner*;
    [[nodiscard]] auto upstream_error()const noexcept->const error_serialization::Owner*;
    [[nodiscard]] auto canonical()const noexcept->std::string_view;
    [[nodiscard]] auto digest()const noexcept->const core::Sha256Digest*;
    [[nodiscard]] auto usage()const noexcept->Usage;
    [[nodiscard]] auto origin()const noexcept->std::optional<Origin>;
    [[nodiscard]] static auto decode(std::string_view,Limits)->std::expected<Owner,Error>;
  private:
    std::unique_ptr<detail::State> state_;explicit Owner(std::unique_ptr<detail::State>)noexcept;
    static auto preflight(const detail::ResolvedInput&,const Limits&)->std::expected<void,Error>;
    static auto upstream_preflight(const detail::ResolvedInput&,const Limits&)->std::expected<void,Error>;
    static auto prepare(const detail::ResolvedInput&,Limits)->std::expected<Owner,Error>;
    friend class process_subject::Session;
};
}
