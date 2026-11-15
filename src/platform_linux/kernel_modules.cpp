#include "platform_linux/kernel_modules.hpp"
#include "epycbench/core/digest.hpp"

#include <algorithm>
#include <array>
#include <dlfcn.h>
#include <fstream>
#include <limits.h>
#include <utility>
#include <unistd.h>

namespace epycbench::platform_linux::kernel_modules {
namespace {
auto failure(Error code, const std::filesystem::path& path, std::string detail)
    -> std::unexpected<Failure> {
    return std::unexpected{Failure{code, path, std::move(detail)}};
}
auto dynamic_error() -> std::string {
    const auto* message = ::dlerror();
    return message ? message : "Shared library operation failed";
}
auto fields_present(const EpycBenchKernelV1& api) noexcept -> bool {
    for (const auto* field : {api.kernel_id, api.implementation_id, api.version,
                             api.compiler, api.build_info, api.timed_symbol})
        if (!field || !*field) return false;
    return api.prepare && api.execute && api.verify && api.cleanup &&
        (api.family == EPYCBENCH_KERNEL_BANDWIDTH || api.family == EPYCBENCH_KERNEL_LATENCY);
}
} // namespace

struct Module::State {
    void* handle{};
    const EpycBenchKernelV1* api{};
    Metadata metadata;
    ~State() {
        if (api) api->cleanup();
        if (handle) ::dlclose(handle);
    }
};

Module::Module(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
Module::Module(Module&&) noexcept = default;
auto Module::operator=(Module&&) noexcept -> Module& = default;
Module::~Module() = default;
auto Module::metadata() const noexcept -> const Metadata& { return state_->metadata; }
auto Module::api() const noexcept -> const EpycBenchKernelV1& { return *state_->api; }

auto Module::load(const std::filesystem::path& path) -> std::expected<Module, Failure> {
    try {
        auto state = std::make_unique<State>();
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error);
        if (error) return failure(Error::io, path, error.message());
        std::ifstream source{absolute, std::ios::binary};
        if (!source) return failure(Error::io, absolute, "Cannot read module file");
        const std::string bytes{std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
        if (source.bad()) return failure(Error::io, absolute, "Cannot read module file");
        state->metadata.sha256 = core::sha256(bytes).to_hex();
        state->metadata.path = absolute;
        state->handle = ::dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!state->handle) return failure(Error::open, absolute, dynamic_error());
        ::dlerror();
        auto entry = reinterpret_cast<EpycBenchKernelEntryV1>(::dlsym(state->handle, "epycbench_kernel_v1"));
        if (const auto* message = ::dlerror()) return failure(Error::symbol, absolute, message);
        if (!entry) return failure(Error::symbol, absolute, "Missing module entry point");
        const auto* api = entry();
        if (!api || api->abi_version != EPYCBENCH_KERNEL_ABI_VERSION ||
            api->struct_size < sizeof(EpycBenchKernelV1))
            return failure(Error::incompatible, absolute, "Unsupported kernel module interface");
        if (!fields_present(*api)) return failure(Error::metadata, absolute, "Incomplete kernel module descriptor");
        const std::string_view id{api->kernel_id};
        const std::string_view prefix = api->family == EPYCBENCH_KERNEL_BANDWIDTH
            ? "memory.bandwidth.read/" : "memory.latency.dependent-load/";
        if (!id.starts_with(prefix) || !id.ends_with("/v1"))
            return failure(Error::metadata, absolute, "Kernel ID does not match its measurement family");
        state->api = api;
        auto& metadata = state->metadata;
        metadata.kernel_id = api->kernel_id;
        metadata.implementation_id = api->implementation_id;
        metadata.version = api->version;
        metadata.compiler = api->compiler;
        metadata.build_info = api->build_info;
        metadata.timed_symbol = api->timed_symbol;
        metadata.family = api->family;
        return Module{std::move(state)};
    } catch (const std::bad_alloc&) {
        return failure(Error::allocation, path, "Cannot allocate module storage");
    } catch (const std::filesystem::filesystem_error& error) {
        return failure(Error::io, path, error.code().message());
    }
}

auto Catalog::discover(const std::filesystem::path& directory) -> std::expected<Catalog, Failure> {
    try {
        Catalog catalog;
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) {
            if (error) return failure(Error::directory, directory, error.message());
            return catalog;
        }
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::directory_iterator{directory})
            if (entry.path().extension() == ".so" && entry.is_regular_file()) paths.push_back(entry.path());
        std::ranges::sort(paths);
        for (const auto& path : paths) {
            auto module = Module::load(path);
            if (!module) return std::unexpected{std::move(module.error())};
            if (catalog.find(module->metadata().kernel_id))
                return failure(Error::duplicate_id, path, "Duplicate kernel ID: " + module->metadata().kernel_id);
            for (const auto& installed : catalog.modules_)
                if (installed.metadata().family == module->metadata().family &&
                    installed.metadata().implementation_id == module->metadata().implementation_id)
                    return failure(Error::duplicate_id, path,
                        "Duplicate implementation ID: " + module->metadata().implementation_id);
            catalog.modules_.push_back(std::move(*module));
        }
        return catalog;
    } catch (const std::bad_alloc&) {
        return failure(Error::allocation, directory, "Cannot allocate module catalog");
    } catch (const std::filesystem::filesystem_error& error) {
        return failure(Error::directory, directory, error.code().message());
    }
}

auto Catalog::find(std::string_view kernel_id) const noexcept -> const Module* {
    for (const auto& module : modules_)
        if (module.metadata().kernel_id == kernel_id) return &module;
    return nullptr;
}

auto default_directory() -> std::expected<std::filesystem::path, Failure> {
    std::array<char, PATH_MAX> path{};
    const auto count = ::readlink("/proc/self/exe", path.data(), path.size());
    if (count < 0 || static_cast<std::size_t>(count) == path.size())
        return failure(Error::directory, {}, "Cannot locate the executable's module directory");
    return std::filesystem::path{std::string_view{path.data(), static_cast<std::size_t>(count)}}.parent_path() / "modules";
}
} // namespace epycbench::platform_linux::kernel_modules
