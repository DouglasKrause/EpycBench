#include "epycbench/kernel/registry.hpp"

#include <array>
#include <new>
#include <vector>

namespace epycbench::kernel {
namespace {
const std::array built_in_descriptors{
    *KernelDescriptor::create(KernelId::sequential_read_bandwidth_v1,
        KernelImplementationId::scalar_u64_sequential_v1),
    *KernelDescriptor::create(KernelId::dependent_load_latency_v1,
        KernelImplementationId::scalar_u64_random_cycle_v1),
};
std::vector<KernelDescriptor> installed_descriptors;
bool installed{};
} // namespace

auto kernel_implementation_id_token(const KernelImplementationId& id) noexcept
    -> std::string_view {
    return id.token();
}

auto parse_kernel_implementation_id(std::string_view token, KernelFamily family) noexcept
    -> std::optional<KernelImplementationId> {
    if ((family != KernelFamily::bandwidth && family != KernelFamily::latency) ||
        !detail::valid_kernel_identity_token(token)) return std::nullopt;
    if (token == "0" || token == "1" || token == "scalar_u64_sequential_v1" ||
        token == "scalar_u64_random_cycle_v1")
        return std::nullopt; // Reserved historical wire spellings, not new plugin IDs.
    if (token == "scalar-u64-sequential/v1" && family != KernelFamily::bandwidth)
        return std::nullopt;
    if (token == "scalar-u64-random-cycle/v1" && family != KernelFamily::latency)
        return std::nullopt;
    return KernelImplementationId{token, family};
}

auto parse_kernel_implementation_id(std::string_view token) noexcept
    -> std::optional<KernelImplementationId> {
    if (token == "scalar-u64-sequential/v1")
        return KernelImplementationId::scalar_u64_sequential_v1;
    if (token == "scalar-u64-random-cycle/v1")
        return KernelImplementationId::scalar_u64_random_cycle_v1;
    return std::nullopt;
}

auto kernel_implementation_wire_token(const KernelImplementationId& id) noexcept
    -> std::string_view {
    if (id == KernelImplementationId::scalar_u64_sequential_v1) return "scalar_u64_sequential_v1";
    if (id == KernelImplementationId::scalar_u64_random_cycle_v1) return "scalar_u64_random_cycle_v1";
    return id.token();
}

auto parse_kernel_implementation_wire_id(std::string_view token, KernelFamily family) noexcept
    -> std::optional<KernelImplementationId> {
    if (token == "scalar_u64_sequential_v1")
        return family == KernelFamily::bandwidth
            ? std::optional{KernelImplementationId::scalar_u64_sequential_v1} : std::nullopt;
    if (token == "scalar_u64_random_cycle_v1")
        return family == KernelFamily::latency
            ? std::optional{KernelImplementationId::scalar_u64_random_cycle_v1} : std::nullopt;
    return parse_kernel_implementation_id(token, family);
}

auto install_kernel_descriptors(std::span<const KernelDescriptor> values) noexcept
    -> std::expected<void, RegistryError> {
    if (installed) return std::unexpected{RegistryError::already_installed};
    for (std::size_t index{}; index < values.size(); ++index) {
        const auto& value = values[index];
        if (value.kernel_id().family() == KernelFamily::invalid ||
            value.kernel_id().family() != value.implementation_id().family())
            return std::unexpected{RegistryError::invalid_descriptor};
        for (std::size_t previous{}; previous < index; ++previous)
            if (values[previous].kernel_id() == value.kernel_id() ||
                values[previous].implementation_id() == value.implementation_id())
                return std::unexpected{RegistryError::duplicate_id};
    }
    try {
        std::vector<KernelDescriptor> owned;
        owned.reserve(values.size());
        for (const auto& value : values) owned.push_back(value);
        installed_descriptors.swap(owned);
        installed = true;
        return {};
    } catch (const std::bad_alloc&) {
        return std::unexpected{RegistryError::allocation};
    }
}

auto find_kernel_descriptor(const KernelId& id) noexcept -> const KernelDescriptor* {
    const std::span<const KernelDescriptor> values = installed
        ? std::span<const KernelDescriptor>{installed_descriptors}
        : std::span<const KernelDescriptor>{built_in_descriptors};
    for (const auto& value : values)
        if (value.kernel_id() == id) return &value;
    return nullptr;
}

} // namespace epycbench::kernel
