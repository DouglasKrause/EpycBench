#pragma once
#include "results/private_implementation_selection.hpp"
#include "results/private_base_build_serialization.hpp"
#include "results/kernel_approval_owner.hpp"

namespace epycbench::results::build_object_association {
inline constexpr std::string_view profile="epycbench.private-build-object-association/v1";
enum class Match { equal,different,not_available,not_comparable };
enum class CompilerFamily { gcc,clang };
enum class Error { state,origin,provenance,association,capacity,encoding };
struct ReportFacts {
    std::array<Match,7> comparisons{};
    auto operator==(const ReportFacts&) const -> bool = default;
};
struct FamilyFacts {
    std::size_t declared_workloads{},bound_experiments{};
    bool approval_supplied{};
    std::array<ReportFacts,2> reports{};
    auto operator==(const FamilyFacts&) const -> bool = default;
};
struct Quantities {
    std::size_t workloads{},bindings{},approvals{},child_retained_bytes{},child_evidence_bytes{},
        canonical_bytes{},association_work{},retained_bytes{},consumer_simultaneous_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits=Quantities;
using Usage=Quantities;
using ApprovalInputs=std::array<std::optional<kernel_approval::Owner>,2>;
class Owner;
namespace detail {
struct State {
    std::optional<implementation_selection::Owner> selection;
    std::optional<base_build_serialization::Owner> base;
    ApprovalInputs approvals;
    std::array<FamilyFacts,2> families;
    std::string canonical;
    core::Sha256Digest digest;
    Usage usage;
};
[[nodiscard]] auto checked_add(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto compiler_family(std::string_view) noexcept -> std::optional<CompilerFamily>;
[[nodiscard]] auto family_index(kernel::KernelId,kernel::KernelImplementationId) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto match_token(Match) noexcept -> std::optional<std::string_view>;
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
}
class Owner {
  public:
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    Owner(const Owner&)=delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto selection() const noexcept -> const implementation_selection::Owner*;
    [[nodiscard]] auto base() const noexcept -> const base_build_serialization::Owner*;
    [[nodiscard]] auto approval(std::size_t) const noexcept -> const kernel_approval::Owner*;
    [[nodiscard]] auto families() const noexcept -> std::span<const FamilyFacts>;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto usage() const noexcept -> Usage;
  private:
    std::unique_ptr<detail::State> state_;
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
    friend auto retain(implementation_selection::Owner&&,base_build_serialization::Owner&&,ApprovalInputs&&,Limits) -> std::expected<Owner,Error>;
};
[[nodiscard]] auto retain(implementation_selection::Owner&&,base_build_serialization::Owner&&,ApprovalInputs&&,Limits) -> std::expected<Owner,Error>;
} // namespace epycbench::results::build_object_association
