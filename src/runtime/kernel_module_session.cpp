#include "runtime/kernel_module_session.hpp"
#include "epycbench/kernel/registry.hpp"
#include "platform_linux/kernel_module_environment.hpp"
#include "core/json_ownership.hpp"

#include <nlohmann/json.hpp>
#include <type_traits>

namespace epycbench::runtime {
namespace {
using Catalog = platform_linux::kernel_modules::Catalog;
struct Session {
    bool attempted{};
    std::optional<Catalog> catalog;
    std::string failure;
};
auto session() -> Session& { static Session value; return value; }

auto snapshot(const Catalog& catalog, const std::filesystem::path& directory) -> std::string {
    auto modules = nlohmann::json::array();
    core::detail::JsonCleanup cleanup{modules};
    for (const auto& module : catalog.modules()) {
        const auto& m = module.metadata();
        modules.push_back({{"kernel", m.kernel_id}, {"implementation", m.implementation_id},
            {"family", m.family}, {"version", m.version}, {"compiler", m.compiler},
            {"build", m.build_info}, {"sha256", m.sha256}});
    }
    return nlohmann::json{{"directory", directory.string()}, {"modules", modules}}.dump();
}

auto initialize(std::optional<std::filesystem::path> directory,
                const std::optional<std::string>& expected) -> std::expected<void, std::string> {
    auto& state = session();
    if (state.attempted) {
        if (state.catalog) return {};
        return std::unexpected{state.failure};
    }
    state.attempted = true;
    const auto fail = [&](std::string message) -> std::expected<void, std::string> {
        state.failure = std::move(message);
        return std::unexpected{state.failure};
    };
    try {
        if (!directory) {
            auto found = platform_linux::kernel_modules::default_directory();
            if (!found) return fail(found.error().detail);
            directory = std::move(*found);
        }
        *directory = std::filesystem::absolute(*directory).lexically_normal();
        auto loaded = Catalog::discover(*directory);
        if (!loaded) return fail(loaded.error().path.string() + ": " + loaded.error().detail);
        const auto captured = snapshot(*loaded, *directory);
        if (expected && captured != *expected)
            return fail("Kernel modules changed between the controller and runner; restart EpycBench.");
        std::vector<kernel::KernelDescriptor> descriptors;
        for (const auto& module : loaded->modules()) {
            const auto& m = module.metadata();
            const auto id = kernel::parse_kernel_id(m.kernel_id);
            if (!id) return fail("Unsupported kernel identity: " + m.kernel_id);
            const auto implementation = kernel::parse_kernel_implementation_id(m.implementation_id, id->family());
            const auto family = id->family() == kernel::KernelFamily::bandwidth
                ? EPYCBENCH_KERNEL_BANDWIDTH : EPYCBENCH_KERNEL_LATENCY;
            if (!implementation || m.family != family) return fail("Kernel module family mismatch: " + m.kernel_id);
            auto descriptor = kernel::KernelDescriptor::create(*id, *implementation,
                kernel::ModuleIdentity{m.version, m.compiler + "; " + m.build_info, m.sha256});
            if (!descriptor) return fail("Invalid kernel descriptor: " + m.kernel_id);
            descriptors.push_back(std::move(*descriptor));
        }
        if (!kernel::install_kernel_descriptors(descriptors))
            return fail("Kernel catalog could not be installed.");
        if (!expected && !platform_linux::publish_kernel_module_session(captured))
            return fail("Kernel module session could not be passed to the runner.");
        state.catalog.emplace(std::move(*loaded));
        return {};
    } catch (const std::exception& error) { return fail(error.what()); }
}

template<class Prepared, class Id>
auto bind(Prepared& prepared, const Id& id, std::uint32_t family)
    -> std::expected<void, ModuleBindingError> {
    const auto* catalog = kernel_module_catalog();
    if (!catalog) {
#ifdef EPYCBENCH_TEST_REFERENCE_KERNELS
        return {}; // Existing deterministic test fixtures use the reference functions.
#else
        return std::unexpected{ModuleBindingError::no_session};
#endif
    }
    const platform_linux::kernel_modules::Module* module{};
    for (const auto& candidate : catalog->modules()) {
        const auto& m = candidate.metadata();
        const auto& token = [&]() -> const std::string& {
            if constexpr (std::is_same_v<Id, kernel::KernelId>) return m.kernel_id;
            else return m.implementation_id;
        }();
        if (token == id.token() && m.family == family) { module = &candidate; break; }
    }
    if (!module) return std::unexpected{ModuleBindingError::unavailable};
    const auto& api = module->api();
    if (api.family != family) return std::unexpected{ModuleBindingError::family};
    const auto bytes = prepared.input().logical_region();
    const EpycBenchKernelRequestV1 request{
        reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), 1, 0};
    if (api.prepare(&request) != 0) return std::unexpected{ModuleBindingError::preparation};
    prepared.bind_module(api);
    return {};
}
}

auto initialize_kernel_modules(std::optional<std::filesystem::path> directory)
    -> std::expected<void, std::string> { return initialize(std::move(directory), {}); }
auto initialize_runner_kernel_modules() -> std::expected<void, std::string> {
    try {
        const auto inherited = platform_linux::inherited_kernel_module_session();
        if (!inherited) return std::unexpected{std::string{"Runner kernel module session is missing."}};
        auto metadata = nlohmann::json::parse(*inherited);
        core::detail::JsonCleanup cleanup{metadata};
        return initialize(metadata.at("directory").get<std::string>(), inherited);
    } catch (...) { return std::unexpected{std::string{"Invalid runner kernel module session."}}; }
}
auto initialize_qualification_kernel_modules() -> std::expected<void, std::string> {
    if (kernel_module_catalog()) return {};
    try {
        if (platform_linux::inherited_kernel_module_session()) return initialize_runner_kernel_modules();
        return initialize_kernel_modules();
    } catch (...) { return std::unexpected{std::string{"Kernel module initialization failed."}}; }
}
auto kernel_module_catalog() noexcept -> const Catalog* {
    const auto& state = session();
    return state.catalog ? &*state.catalog : nullptr;
}
auto bind_kernel_module(kernel::PreparedSequentialReadBandwidth& prepared, kernel::KernelId id)
    -> std::expected<void, ModuleBindingError> { return bind(prepared, id, EPYCBENCH_KERNEL_BANDWIDTH); }
auto bind_kernel_module(kernel::PreparedDependentLoadLatency& prepared, kernel::KernelId id)
    -> std::expected<void, ModuleBindingError> { return bind(prepared, id, EPYCBENCH_KERNEL_LATENCY); }
auto bind_kernel_module(kernel::PreparedSequentialReadBandwidth& prepared, kernel::KernelImplementationId id)
    -> std::expected<void, ModuleBindingError> { return bind(prepared, id, EPYCBENCH_KERNEL_BANDWIDTH); }
auto bind_kernel_module(kernel::PreparedDependentLoadLatency& prepared, kernel::KernelImplementationId id)
    -> std::expected<void, ModuleBindingError> { return bind(prepared, id, EPYCBENCH_KERNEL_LATENCY); }
}
