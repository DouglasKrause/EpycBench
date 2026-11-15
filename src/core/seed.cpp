#include "epycbench/core/seed.hpp"
#include "json_ownership.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/core/digest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::core {
namespace {

constexpr std::string_view profile{"epycbench.seed-derivation.v1"};

[[nodiscard]] constexpr auto domain_token(SeedDomain domain) noexcept -> std::string_view {
    switch (domain) {
    case SeedDomain::worker_selection:
        return "worker-selection";
    case SeedDomain::pointer_cycle:
        return "pointer-cycle";
    case SeedDomain::block_order:
        return "block-order";
    case SeedDomain::bootstrap:
        return "bootstrap";
    case SeedDomain::observation_start:
        return "observation-start";
    }
    return "unknown";
}

[[nodiscard]] auto seed_error(std::string code, std::string message, std::string_view domain,
                              std::size_t component_index, std::string classification,
                              std::string underlying_code = {}) -> Error {
    auto context = nlohmann::json::object();
    detail::JsonCleanup context_cleanup{context};
    context["domain"] = domain;
    context["component_index"] = component_index;
    context["classification"] = std::move(classification);
    if (!underlying_code.empty()) {
        context["underlying_code"] = std::move(underlying_code);
    }
    return Error{
        .category = ErrorCategory::core,
        .code = StableErrorCode{std::move(code)},
        .message = std::move(message),
        .operation = "derive_seed",
        .os_error = std::nullopt,
        .context = std::move(context),
        .severity = ErrorSeverity::error,
        .continuation = ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] constexpr auto valid_component_name(std::string_view name) noexcept -> bool {
    if (name.empty() || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(name.substr(1), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_' || character == '.' || character == '-';
    });
}

} // namespace

auto derive_seed(std::uint64_t root_seed, const SeedDerivationKey& key)
    -> std::expected<DerivedSeed, Error> {
    const auto domain = domain_token(key.domain);
    auto components = key.components;
    std::ranges::sort(components, {}, &SeedComponent::name);

    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto& component = components[index];
        if (!valid_component_name(component.name)) {
            return std::unexpected{seed_error("core.seed.invalid_component_name",
                                              "Seed component name is invalid", domain, index,
                                              "invalid_component_name")};
        }
        if (index != 0U && components[index - 1U].name == component.name) {
            return std::unexpected{seed_error("core.seed.duplicate_component",
                                              "Seed component names must be unique", domain, index,
                                              "duplicate_component")};
        }

        // Preserve the established one-element array used for component validation.
        auto validation_value = nlohmann::json::array();
        detail::JsonCleanup validation_cleanup{validation_value};
        validation_value.push_back(component.value);
        auto encoded_value = canonicalize_json(validation_value);
        detail::JsonPointerCleanup<nlohmann::json> encoded_error_cleanup{
            encoded_value ? nullptr : &encoded_value.error().context};
        if (!encoded_value) {
            const auto& underlying = encoded_value.error().code.value();
            if (underlying == "core.canonical_json.invalid_utf8") {
                return std::unexpected{seed_error("core.seed.invalid_utf8",
                                                  "Seed component value is not valid UTF-8", domain,
                                                  index, "invalid_utf8", underlying)};
            }
            return std::unexpected{seed_error("core.seed.canonicalization_failed",
                                              "Seed payload canonicalization failed", domain, index,
                                              "component_value", underlying)};
        }
    }

    auto encoded_components = nlohmann::json::array();
    detail::JsonCleanup components_cleanup{encoded_components};
    for (const auto& component : components) {
        detail::append_object_json(encoded_components, [&](nlohmann::json& item) {
            item["name"] = component.name;
            item["value"] = component.value;
        });
    }
    auto payload = nlohmann::json::object();
    detail::JsonCleanup payload_cleanup{payload};
    payload["profile"] = profile;
    payload["root_seed"] = std::to_string(root_seed);
    payload["domain"] = domain;
    detail::set_json(payload, "components", std::move(encoded_components));
    auto canonical = canonicalize_json(payload);
    detail::JsonPointerCleanup<nlohmann::json> canonical_error_cleanup{
        canonical ? nullptr : &canonical.error().context};
    if (!canonical) {
        return std::unexpected{
            seed_error("core.seed.canonicalization_failed", "Seed payload canonicalization failed",
                       domain, components.size(), "payload", canonical.error().code.value())};
    }

    const auto digest = sha256(*canonical);
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(digest.bytes()[index]);
    }
    return DerivedSeed{value};
}

} // namespace epycbench::core
