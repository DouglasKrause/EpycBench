#include "results/host_evidence_assembly.hpp"
#include <limits>
#include <type_traits>
#include <utility>
namespace epycbench::results::host_assembly {
namespace {
auto add(std::size_t& value, std::size_t amount) noexcept -> bool {
    if (amount > std::numeric_limits<std::size_t>::max() - value) return false;
    value += amount; return true;
}
auto failure(const char* code) -> core::Error {
    return {.category = core::ErrorCategory::results,
        .code = core::StableErrorCode{"results.host_assembly." + std::string{code}},
        .message = "Supplied Host assembly rejected", .operation = "assemble_host_evidence",
        .os_error = std::nullopt, .context = nlohmann::json::object()};
}
template<class T>
auto validate(const Slot<T>& slot, std::string_view run,
              const topology_evidence::Owner* topology, Usage& usage) noexcept -> const char* {
    if (slot.valueless_by_exception()) return "invalid_input";
    if (const auto* missing = std::get_if<Unavailable>(&slot)) {
        switch (missing->reason) {
        case Reason::not_supported: case Reason::source_missing: case Reason::access_denied: return nullptr;
        }
        return "invalid_input";
    }
    const auto* owner = std::get_if<T>(&slot);
    if (!owner) return nullptr;
    if (!owner->digest()) return "invalid_input";
    std::size_t retained{};
    std::string_view canonical;
    if constexpr (std::is_same_v<T, topology_evidence::Owner>) {
        retained = owner->usage().retained; canonical = owner->canonical();
    } else {
        if (owner->run_id() != run) return "identity_mismatch";
        if (const auto* binding = owner->topology()) {
            if (!topology || !topology->digest() || !topology->fingerprints() ||
                binding->canonical_sha256 != *topology->digest() || binding->fingerprints != *topology->fingerprints())
                return "identity_mismatch";
        }
        retained = owner->usage().retained_bytes;
        if constexpr (requires { owner->private_canonical(); }) canonical = owner->private_canonical();
        else canonical = owner->canonical();
    }
    if (!detail::add_stock(usage, canonical.size(), retained, sizeof(T))) return "capacity";
    return nullptr;
}
static_assert(std::is_nothrow_move_constructible_v<Categories>);
static_assert(std::is_nothrow_move_assignable_v<Categories>);
} // namespace
auto detail::add_stock(Usage& usage, std::size_t canonical, std::size_t retained,
                       std::size_t embedded) noexcept -> bool {
    if (retained < embedded) return false;
    auto candidate = usage;
    if (!add(candidate.canonical_bytes, canonical) || !add(candidate.retained_bytes, retained - embedded)) return false;
    usage = candidate; return true;
}
Owner::Owner(Owner&& other) noexcept : live_(std::exchange(other.live_, false)),
    run_(std::move(other.run_)), categories_(std::move(other.categories_)), usage_(std::exchange(other.usage_, {})) {}
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this != &other) {
        live_ = std::exchange(other.live_, false); run_ = std::move(other.run_);
        categories_ = std::move(other.categories_); usage_ = std::exchange(other.usage_, {});
    }
    return *this;
}
auto assemble(Input& input, Limits limits) -> std::expected<Owner, core::Error> {
    if (input.run.text().empty()) return std::unexpected{failure("invalid_input")};
    Usage usage{7, 0, sizeof(Owner)};
    if (!add(usage.retained_bytes, input.run.text().size())) return std::unexpected{failure("capacity")};
    const auto* topology = std::get_if<topology_evidence::Owner>(&input.categories.topology);
    if (const auto* code = validate(input.categories.topology, input.run.text(), topology, usage)) return std::unexpected{failure(code)};
    if (const auto* code = validate(input.categories.system_controls, input.run.text(), topology, usage)) return std::unexpected{failure(code)};
    if (const auto* code = validate(input.categories.kernel_identity, input.run.text(), topology, usage)) return std::unexpected{failure(code)};
    if (const auto* code = validate(input.categories.pu_identity, input.run.text(), topology, usage)) return std::unexpected{failure(code)};
    if (const auto* code = validate(input.categories.frequency, input.run.text(), topology, usage)) return std::unexpected{failure(code)};
    if (const auto* code = validate(input.categories.anonymous_thp, input.run.text(), topology, usage)) return std::unexpected{failure(code)};
    if (const auto* code = validate(input.categories.process_thp, input.run.text(), topology, usage)) return std::unexpected{failure(code)};
    if (usage.slots > limits.slots || usage.canonical_bytes > limits.canonical_bytes || usage.retained_bytes > limits.retained_bytes)
        return std::unexpected{failure("capacity")};
    Owner owner;
    owner.run_ = input.run.text(); // Last potentially throwing operation before transfer.
    owner.usage_ = usage;
    owner.categories_ = std::move(input.categories);
    return owner;
}
} // namespace epycbench::results::host_assembly
