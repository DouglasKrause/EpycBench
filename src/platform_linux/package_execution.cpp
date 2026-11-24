#include "platform_linux/package_execution.hpp"

#include "epycbench/platform_linux/file_descriptor.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"
#include "kernel/package_manifest.hpp"
#include "kernel/package_layout_rules.hpp"
#include "core/binary_digest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <link.h>
#include <map>
#include <mutex>
#include <new>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux::package_execution {
namespace {
auto error(std::string_view code, std::string_view message, std::string_view operation,
           nlohmann::json context = nlohmann::json::object(), int error_number = 0)
    -> core::Error {
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{std::string{code}},
            .message = std::string{message},
            .operation = std::string{operation},
            .os_error = error_number == 0
                ? std::nullopt
                : std::optional{core::OsError::capture({error_number, std::generic_category()})},
            .context = std::move(context)};
}

auto binary_error(std::string_view code, std::string_view message,
                  const std::filesystem::path& path, int error_number = 0) -> core::Error {
    return error(code, message, "Package.load", {{"path", path.string()}}, error_number);
}

auto callback_error(std::string_view callback, int status) -> core::Error {
    return error("kernel.package.callback_failed", "Task callback failed", "Package.prepare",
                 {{"callback", callback}, {"status", status}});
}

template<class Callback>
auto task_json(Callback callback, std::string_view name) -> std::expected<nlohmann::json, core::Error> {
    std::array<char, 64U * 1024U> bytes{};
    EpycBenchPackageJsonV1 out{bytes.data(), static_cast<std::uint32_t>(bytes.size()), 0};
    if (const auto status = callback(&out); status != 0) return std::unexpected{callback_error(name, status)};
    const auto invalid = [&] {
        return std::unexpected{error("kernel.package.task_json_invalid", "Task returned an invalid bounded JSON object",
                                     "Package.inspect", {{"callback", name}})};
    };
    if (out.data != bytes.data() || out.capacity != bytes.size() || out.bytes_written == 0 ||
        out.bytes_written > out.capacity) return invalid();
    bool depth_exceeded = false;
    const auto value = nlohmann::json::parse(bytes.data(), bytes.data() + out.bytes_written,
        [&](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > 32) depth_exceeded = true;
            return depth <= 32;
        }, false);
    if (depth_exceeded || !value.is_object()) return invalid();
    return value;
}

auto snapshot(const std::filesystem::path& path, const core::Sha256Digest& expected)
    -> std::expected<FileDescriptor, core::Error> {
    constexpr auto maximum_binary_bytes = kernel::packages::maximum_binary_bytes;
    const auto input_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input_fd < 0)
        return std::unexpected{binary_error("kernel.package.binary_open_failed",
                                           "Cannot open package binary", path, errno)};
    auto input = FileDescriptor::adopt(input_fd);
    if (!input) return std::unexpected{std::move(input.error())};
    struct stat metadata {};
    if (::fstat(input_fd, &metadata) != 0)
        return std::unexpected{binary_error("kernel.package.binary_open_failed",
                                           "Cannot inspect package binary", path, errno)};
    if (!S_ISREG(metadata.st_mode) || metadata.st_size <= 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > maximum_binary_bytes)
        return std::unexpected{binary_error("kernel.package.binary_size_invalid",
                                           "Package binary is not a bounded regular file", path)};

    const auto copy_fd = ::memfd_create("epycbench-package", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (copy_fd < 0)
        return std::unexpected{binary_error("kernel.package.snapshot_failed",
                                           "Cannot create package binary snapshot", path, errno)};
    auto copy = FileDescriptor::adopt(copy_fd);
    if (!copy) return std::unexpected{std::move(copy.error())};

    core::BinaryDigest digest;
    std::array<std::byte, 64U * 1024U> buffer;
    std::uint64_t total = 0;
    for (;;) {
        const auto count = ::read(input_fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::unexpected{binary_error("kernel.package.binary_read_failed",
                                               "Cannot read package binary", path, errno)};
        }
        if (count == 0) break;
        const auto bytes = std::span{buffer}.first(static_cast<std::size_t>(count));
        total += bytes.size();
        if (total > maximum_binary_bytes || !digest.append(bytes))
            return std::unexpected{binary_error("kernel.package.binary_size_invalid",
                                               "Package binary exceeds the size bound", path)};
        for (std::size_t offset = 0; offset < bytes.size();) {
            const auto written = ::write(copy_fd, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0)
                return std::unexpected{binary_error("kernel.package.snapshot_failed",
                                                   "Cannot write package binary snapshot", path,
                                                   written < 0 ? errno : 0)};
            offset += static_cast<std::size_t>(written);
        }
    }
    if (digest.finish() != expected)
        return std::unexpected{binary_error("kernel.package.binary_identity_mismatch",
                                           "Package binary changed after discovery", path)};
    if (::fcntl(copy_fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0)
        return std::unexpected{binary_error("kernel.package.snapshot_failed",
                                           "Cannot seal package binary snapshot", path, errno)};
    return copy;
}

struct CloseLibrary {
    void operator()(void* library) const noexcept { ::dlclose(library); }
};

struct ImageRange {
    std::uintptr_t begin, end;
    bool readable, executable;
};

struct Library {
    FileDescriptor image;
    std::unique_ptr<void, CloseLibrary> handle;
    link_map* owner{};
    std::vector<ImageRange> ranges;

    [[nodiscard]] auto owns(const void* address, std::size_t bytes, bool code = false) const noexcept -> bool {
        const auto value = reinterpret_cast<std::uintptr_t>(address);
        if (!std::ranges::any_of(ranges, [&](const auto& range) {
                return value >= range.begin && value < range.end && bytes <= range.end - value &&
                    (code ? range.executable : range.readable);
            })) return false;
        Dl_info information{};
        void* mapping{};
        return ::dladdr1(address, &information, &mapping, RTLD_DL_LINKMAP) != 0 && mapping == owner;
    }

    template<class Object>
    [[nodiscard]] auto object(const Object* pointer) const noexcept -> bool {
        return reinterpret_cast<std::uintptr_t>(pointer) % alignof(Object) == 0 && owns(pointer, sizeof(Object));
    }

    template<class Callback>
    [[nodiscard]] auto callback(Callback function) const noexcept -> bool {
        return owns(reinterpret_cast<const void*>(function), 1U, true);
    }

    [[nodiscard]] auto symbol(const char* name) const noexcept -> void* {
        auto* address = ::dlsym(handle.get(), name);
        return owns(address, 1U, true) ? address : nullptr;
    }
};

template<class Integer>
auto hexadecimal(std::string_view text, Integer& value) noexcept -> bool {
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value, 16);
    return result.ec == std::errc{} && result.ptr == end;
}

auto mapped_snapshot(const link_map& owner, int descriptor) -> std::vector<ImageRange> {
    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0) return {};
    const auto address = reinterpret_cast<std::uintptr_t>(owner.l_ld);
    bool dynamic_section_matches = false;
    std::vector<ImageRange> ranges;
    std::ifstream maps{"/proc/self/maps"};
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream columns{line};
        std::string range, permissions, offset, device;
        std::uint64_t inode{};
        if (!(columns >> range >> permissions >> offset >> device >> inode)) continue;
        const auto dash = range.find('-');
        const auto colon = device.find(':');
        if (dash == std::string::npos || colon == std::string::npos) continue;
        const std::string_view addresses{range}, numbers{device};
        std::uintptr_t first{}, last{};
        unsigned int major_number{}, minor_number{};
        if (!hexadecimal(addresses.substr(0, dash), first) || !hexadecimal(addresses.substr(dash + 1U), last) ||
            first >= last || permissions.size() < 3U || inode != metadata.st_ino ||
            !hexadecimal(numbers.substr(0, colon), major_number) ||
            !hexadecimal(numbers.substr(colon + 1U), minor_number) ||
            major_number != ::major(metadata.st_dev) || minor_number != ::minor(metadata.st_dev))
            continue;
        ranges.push_back({first, last, permissions[0] == 'r', permissions[2] == 'x'});
        dynamic_section_matches = dynamic_section_matches || (address >= first && address < last);
    }
    if (!dynamic_section_matches) return {};
    return ranges;
}

auto open_library(FileDescriptor image, const std::filesystem::path& path, const core::Sha256Digest& digest)
    -> std::expected<std::shared_ptr<const Library>, core::Error> {
    // Cache only after an opened snapshot has been re-authenticated. Keep its
    // proc pathname reserved for the loaded image until process retirement.
    // glibc can retain that name for GNU-unique or NODELETE binaries.
    static std::mutex cache_lock;
    static std::map<core::Sha256Digest, std::shared_ptr<const Library>> cache;
    const std::scoped_lock lock{cache_lock};
    if (const auto held = cache.find(digest); held != cache.end()) return held->second;
    const auto name = "/proc/self/fd/" + std::to_string(image.native_handle());
    std::unique_ptr<void, CloseLibrary> handle{::dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL)};
    if (!handle) {
        const auto* message = ::dlerror();
        return std::unexpected{binary_error("kernel.package.library_load_failed",
                                           message ? message : "Cannot load package binary", path)};
    }
    link_map* owner{};
    if (::dlinfo(handle.get(), RTLD_DI_LINKMAP, &owner) != 0 || !owner)
        return std::unexpected{binary_error("kernel.package.library_load_failed",
                                           "Cannot identify loaded package binary", path)};
    auto ranges = mapped_snapshot(*owner, image.native_handle());
    if (ranges.empty())
        return std::unexpected{binary_error("kernel.package.mapped_snapshot_mismatch",
                                           "Loaded object does not match its sealed snapshot", path)};
    auto library = std::make_shared<const Library>(Library{std::move(image), std::move(handle), owner, std::move(ranges)});
    cache.emplace(digest, library);
    return library;
}

template<class Descriptor, class Entry>
auto descriptor(const Library& library, const char* symbol, const std::filesystem::path& path)
    -> std::expected<const Descriptor*, core::Error> {
    const auto entry = reinterpret_cast<Entry>(library.symbol(symbol));
    if (!entry)
        return std::unexpected{binary_error("kernel.package.entry_missing",
                                           "Package entry is absent from the admitted binary", path)};
    const auto* api = entry();
    if (!library.object(api) || api->abi_version != EPYCBENCH_PACKAGE_ABI_VERSION ||
        api->struct_size != sizeof(Descriptor))
        return std::unexpected{binary_error("kernel.package.abi_incompatible",
                                           "Unsupported package descriptor", path)};
    return api;
}

auto text(std::string_view value) noexcept -> EpycBenchPackageTextV1 {
    return {value.data(), value.size()};
}

auto values(const kernel::packages::ParameterDefinitions& definitions,
            const nlohmann::json& resolved) -> std::vector<EpycBenchPackageValueV1> {
    using kernel::packages::ParameterType;
    std::vector<EpycBenchPackageValueV1> result;
    result.reserve(definitions.size());
    for (const auto& [name, definition] : definitions) {
        const auto found = resolved.find(name);
        if (found == resolved.end()) continue;
        EpycBenchPackageValueV1 item{};
        item.name = text(name);
        switch (definition.type) {
        case ParameterType::uint64:
            item.type = EPYCBENCH_PACKAGE_UINT64;
            item.value.unsigned_value = found->get<std::uint64_t>();
            break;
        case ParameterType::int64:
            item.type = EPYCBENCH_PACKAGE_INT64;
            item.value.signed_value = found->get<std::int64_t>();
            break;
        case ParameterType::float64:
            item.type = EPYCBENCH_PACKAGE_FLOAT64;
            item.value.float_value = found->get<double>();
            break;
        case ParameterType::boolean:
            item.type = EPYCBENCH_PACKAGE_BOOL;
            item.value.bool_value = found->get<bool>() ? 1U : 0U;
            break;
        case ParameterType::enumeration:
            item.type = EPYCBENCH_PACKAGE_ENUM;
            item.value.enum_value = text(found->get_ref<const std::string&>());
            break;
        }
        result.push_back(item);
    }
    return result;
}

auto validate_layout(const EpycBenchPackageLayoutV1& layout, std::uint64_t budget,
                     const Request& request, bool shared = false)
    -> std::expected<void, core::Error> {
    if (layout.region_count > EPYCBENCH_PACKAGE_MAX_REGIONS)
        return std::unexpected{error("kernel.package.layout_invalid", "Too many package regions",
                                     "Package.prepare")};
    if (layout.partition_count > std::numeric_limits<std::uint64_t>::max() - layout.partition_begin ||
        (!shared && (request.partition_begin != 0 || request.partition_count != 0) &&
         (request.partition_begin != layout.partition_begin || request.partition_count != layout.partition_count)))
        return std::unexpected{error("kernel.package.layout_invalid", "Invalid effective task partition",
                                     "Package.prepare")};

    std::array<std::string_view, EPYCBENCH_PACKAGE_MAX_REGIONS> names{};
    std::uint64_t available = budget;
    for (std::uint32_t i = 0; i < layout.region_count; ++i) {
        const auto& region = layout.regions[i];
        const auto length = ::strnlen(region.name, sizeof(region.name));
        const std::string_view name{region.name, length};
        const auto preceding_names = std::span{names}.first(i);
        if (!kernel::packages::valid_package_region(name, region.kind, region.bytes, region.alignment, shared) ||
            std::ranges::find(preceding_names, name) != preceding_names.end())
            return std::unexpected{error("kernel.package.layout_invalid", "Invalid package region",
                                         "Package.prepare", {{"region_index", i}})};
        const auto charge = std::max(region.bytes, region.alignment);
        const auto copies = region.kind == EPYCBENCH_PACKAGE_OUTPUT || shared ? 2U : 1U;
        if (charge > available / copies)
            return std::unexpected{error("kernel.package.region_budget_exceeded",
                                        "Package regions exceed the allocation budget",
                                        "Package.prepare", {{"budget_bytes", budget}})};
        available -= charge * copies;
        names[i] = name;
    }
    if (layout.preparation_bytes > available ||
        layout.preparation_bytes > std::numeric_limits<std::ptrdiff_t>::max())
        return std::unexpected{error("kernel.package.region_budget_exceeded",
                                    "Task preparation exceeds the allocation budget", "Package.prepare",
                                    {{"budget_bytes", budget}, {"preparation_bytes", layout.preparation_bytes}})};
    return {};
}

struct TaskCall {
    nlohmann::json parameters;
    std::vector<EpycBenchPackageValueV1> values;
    std::vector<EpycBenchPackageLocalityV1> locality;
    EpycBenchPackageTaskRequestV1 request;
};

auto task_call(const kernel::packages::TaskManifest& task, const Request& request)
    -> std::expected<TaskCall, core::Error> {
    if (request.repetitions == 0 || request.rank_count < task.ranks.minimum ||
        request.rank_count > task.ranks.maximum || request.rank >= request.rank_count ||
        (!request.locality.empty() && request.locality.size() != request.rank_count) ||
        request.partition_count > std::numeric_limits<std::uint64_t>::max() - request.partition_begin)
        return std::unexpected{error("kernel.package.request_invalid", "Invalid rank, partition, or repetitions",
                                     "Package.prepare")};
    auto parameters = kernel::packages::resolve_parameters(task.parameters, request.parameters, "parameters");
    if (!parameters) return std::unexpected{std::move(parameters.error())};
    auto named = values(task.parameters, *parameters);
    auto locality = request.locality;
    const EpycBenchPackageTaskRequestV1 call{named.data(), named.size(), request.seed,
        request.repetitions, request.rank, request.rank_count, request.partition_begin, request.partition_count, request.block,
        locality.data(), static_cast<std::uint32_t>(locality.size())};
    return TaskCall{std::move(*parameters), std::move(named), std::move(locality), call};
}

auto task_layout(const EpycBenchPackageTaskV1& task, const TaskCall& call, const Request& request,
                 std::uint64_t maximum_region_bytes) -> std::expected<EpycBenchPackageLayoutV1, core::Error> {
    EpycBenchPackageLayoutV1 result{};
    if (const auto status = task.layout(&call.request, &result); status != 0)
        return std::unexpected{callback_error("layout", status)};
    if (auto validation = validate_layout(result, maximum_region_bytes, request); !validation)
        return std::unexpected{std::move(validation.error())};
    return result;
}

auto same_layout(const EpycBenchPackageLayoutV1& actual, const EpycBenchPackageLayoutV1& planned) noexcept -> bool {
    const auto same_region = [](const auto& lhs, const auto& rhs) {
        return lhs.kind == rhs.kind && lhs.bytes == rhs.bytes && lhs.alignment == rhs.alignment &&
               std::strncmp(lhs.name, rhs.name, sizeof(lhs.name)) == 0;
    };
    return actual.region_count == planned.region_count && actual.partition_begin == planned.partition_begin &&
           actual.partition_count == planned.partition_count && actual.preparation_bytes == planned.preparation_bytes &&
           std::ranges::equal(std::span{actual.regions}.first(actual.region_count),
                              std::span{planned.regions}.first(planned.region_count), same_region);
}

auto validate_bindings(const EpycBenchPackageLayoutV1& layout, RegionBindings bindings)
    -> std::expected<void, core::Error> {
    const auto regions = std::span{layout.regions}.first(layout.region_count);
    if (bindings.expected_layout && !same_layout(layout, *bindings.expected_layout))
        return std::unexpected{error("kernel.package.layout_changed", "Task layout changed after planning",
                                     "Package.prepare")};
    if (!bindings.borrowed_inputs.empty()) {
        const auto count = std::ranges::count(regions, EPYCBENCH_PACKAGE_INPUT, &EpycBenchPackageRegionRequirementV1::kind);
        if (!bindings.inputs.empty() || bindings.borrowed_inputs.size() != static_cast<std::size_t>(count))
            return std::unexpected{error("kernel.package.borrowed_inputs_invalid",
                                         "Replica borrowers must consume all immutable input regions", "Package.prepare")};
        for (const auto& region : regions) {
            if (region.kind != EPYCBENCH_PACKAGE_INPUT) continue;
            const auto found = std::ranges::find(bindings.borrowed_inputs, region.name, &BorrowedInput::name);
            if (found == bindings.borrowed_inputs.end() || !found->owner || !found->bytes.data() ||
                found->bytes.size() != region.bytes || reinterpret_cast<std::uintptr_t>(found->bytes.data()) % region.alignment)
                return std::unexpected{error("kernel.package.borrowed_inputs_invalid",
                                             "Replica owner did not provide the required immutable input",
                                             "Package.prepare", {{"region", region.name}})};
        }
    }
    for (const auto& input : bindings.inputs) {
        const auto match = std::ranges::find_if(regions, [&](const auto& region) {
            return region.kind == EPYCBENCH_PACKAGE_INPUT && region.name == input.name;
        });
        if (match == regions.end() || match->bytes != input.bytes.size() ||
            std::ranges::count(bindings.inputs, input.name, &InputBinding::name) != 1)
            return std::unexpected{error("kernel.package.input_binding_invalid",
                                         "Input binding does not match a declared input region",
                                         "Package.prepare", {{"region", input.name}})};
    }
    for (const auto output : bindings.export_outputs) {
        const auto match = std::ranges::find_if(regions, [&](const auto& region) {
            return region.kind == EPYCBENCH_PACKAGE_OUTPUT && region.name == output;
        });
        if (match == regions.end() || std::ranges::count(bindings.export_outputs, output) != 1)
            return std::unexpected{error("kernel.package.output_binding_invalid",
                                         "Output binding does not match a declared output region",
                                         "Package.prepare", {{"region", output}})};
    }
    return {};
}

auto allocate_default(void*, const AllocationRequest& request)
    -> std::expected<Allocation, core::Error> {
    const auto alignment = std::align_val_t{std::max(request.alignment, alignof(std::max_align_t))};
    auto* data = static_cast<std::byte*>(::operator new(request.bytes, alignment));
    std::shared_ptr<void> owner{data, [alignment](void* buffer) { ::operator delete(buffer, alignment); }};
    return Allocation{std::move(owner), {data, request.bytes}};
}

struct AddressRange {
    std::uintptr_t begin, end;
};

auto allocate_region(ResourceProvider provider, const AllocationRequest& request,
                     std::vector<AddressRange>& ranges) -> std::expected<Allocation, core::Error> {
    auto allocation = provider.allocate(provider.context, request);
    if (!allocation) return std::unexpected{std::move(allocation.error())};
    const auto begin = reinterpret_cast<std::uintptr_t>(allocation->bytes.data());
    if (!allocation->owner || begin == 0 || begin % request.alignment != 0 ||
        allocation->bytes.size() < request.bytes ||
        request.bytes > std::numeric_limits<std::uintptr_t>::max() - begin)
        return std::unexpected{error("kernel.package.allocation_invalid",
                                     "Region provider returned an invalid allocation",
                                     "Package.prepare", {{"region", request.name}})};
    const auto end = begin + request.bytes;
    if (std::ranges::any_of(ranges, [begin, end](const AddressRange& held) {
            return begin < held.end && held.begin < end;
        }))
        return std::unexpected{error("kernel.package.allocation_overlaps",
                                     "Region provider returned overlapping allocations",
                                     "Package.prepare", {{"region", request.name}})};
    ranges.push_back({begin, end});
    allocation->bytes = allocation->bytes.first(request.bytes);
    return allocation;
}

auto writable(std::span<std::byte> bytes, const char* name) noexcept -> EpycBenchPackageWriteRegionV1 {
    return {name, reinterpret_cast<unsigned char*>(bytes.data()), bytes.size()};
}

auto readable(const EpycBenchPackageWriteRegionV1& view) noexcept -> EpycBenchPackageReadRegionV1 {
    return {view.name, view.data, view.bytes};
}

auto native_now(void*) -> std::expected<std::uint64_t, core::Error> {
    return raw_monotonic_now().transform([](auto timestamp) { return timestamp.nanoseconds(); });
}

auto same_work(const EpycBenchPackageWorkV1& lhs, const EpycBenchPackageWorkV1& rhs) noexcept -> bool {
    return lhs.repetitions == rhs.repetitions && lhs.work_units == rhs.work_units &&
           lhs.output_count == rhs.output_count;
}

auto shared_inputs_match(std::span<const RegionEvidence> regions) noexcept -> bool {
    return std::ranges::all_of(regions, [](const RegionEvidence& region) {
        return region.kind != EPYCBENCH_PACKAGE_SHARED_INPUT ||
               (region.before && region.before == region.after && region.before == region.reference);
    });
}
} // namespace

struct Module::State {
    std::shared_ptr<const Library> task_library;
    std::shared_ptr<const Library> implementation_library;
    const EpycBenchPackageTaskV1* task;
    const EpycBenchPackageImplementationV1* implementation;
    std::shared_ptr<const kernel::packages::TaskManifest> manifest;
    kernel::packages::ParameterDefinitions tuning;
    kernel::packages::AcceptedValues accepts;
    MappingCallbacks mapping{};
};

auto Module::load(const kernel::packages::ImplementationManifest& manifest)
    -> std::expected<Module, core::Error> {
    try {
        if (!manifest.task || manifest.task->oracle_sha256 == manifest.module_sha256)
            return std::unexpected{error("kernel.package.oracle_identity_invalid",
                                         "Package requires distinct task and implementation binaries",
                                         "Package.load")};
        // Bind both identities before either snapshot is executed by the dynamic loader.
        auto task_snapshot = snapshot(manifest.task->oracle_path, manifest.task->oracle_sha256);
        if (!task_snapshot) return std::unexpected{std::move(task_snapshot.error())};
        auto implementation_snapshot = snapshot(manifest.module_path, manifest.module_sha256);
        if (!implementation_snapshot) return std::unexpected{std::move(implementation_snapshot.error())};

        auto task = open_library(std::move(*task_snapshot), manifest.task->oracle_path, manifest.task->oracle_sha256);
        if (!task) return std::unexpected{std::move(task.error())};
        auto implementation = open_library(std::move(*implementation_snapshot), manifest.module_path, manifest.module_sha256);
        if (!implementation) return std::unexpected{std::move(implementation.error())};
        const auto& task_object = **task;
        const auto& implementation_object = **implementation;
        using kernel::packages::MappingResource;
        if (manifest.task->mapping != MappingResource::none) {
            auto oracle = descriptor<EpycBenchPackageMappingTaskV1, EpycBenchPackageMappingTaskEntryV1>(
                task_object, "epycbench_package_mapping_task_v1", manifest.task->oracle_path);
            if (!oracle) return std::unexpected{std::move(oracle.error())};
            auto kernel = descriptor<EpycBenchPackageMappingImplementationV1,
                EpycBenchPackageMappingImplementationEntryV1>(implementation_object,
                "epycbench_package_mapping_implementation_v1", manifest.module_path);
            if (!kernel) return std::unexpected{std::move(kernel.error())};
            const bool touch = manifest.task->mapping == MappingResource::first_touch;
            if (!task_object.callback((*oracle)->verify) ||
                (touch && (!implementation_object.callback((*kernel)->first_touch) || (*kernel)->allocate)) ||
                (!touch && (!implementation_object.callback((*kernel)->allocate) || (*kernel)->first_touch)))
                return std::unexpected{binary_error("kernel.package.mapping_invalid",
                    "Mapping resource and callbacks differ", manifest.module_path)};
            return Module{std::make_shared<State>(State{std::move(*task), std::move(*implementation),
                nullptr, nullptr, manifest.task, manifest.tuning, manifest.accepts, {*oracle, *kernel}})};
        }
        auto task_api = descriptor<EpycBenchPackageTaskV1, EpycBenchPackageTaskEntryV1>(
            task_object, "epycbench_package_task_v1", manifest.task->oracle_path);
        if (!task_api) return std::unexpected{std::move(task_api.error())};
        auto implementation_api = descriptor<EpycBenchPackageImplementationV1,
            EpycBenchPackageImplementationEntryV1>(implementation_object,
                "epycbench_package_implementation_v1", manifest.module_path);
        if (!implementation_api) return std::unexpected{std::move(implementation_api.error())};
        if (!task_object.callback((*task_api)->layout) || !task_object.callback((*task_api)->initialize) ||
            !task_object.callback((*task_api)->reference) || !task_object.callback((*task_api)->compare))
            return std::unexpected{binary_error("kernel.package.callback_invalid",
                                               "Task callback is absent from the admitted binary",
                                               manifest.task->oracle_path)};
        const auto* shared = (*task_api)->collective;
        if (manifest.task->collective != (shared != nullptr) ||
            (shared && (!task_object.object(shared) || !task_object.callback(shared->layout) ||
                !task_object.callback(shared->initialize) || !task_object.callback(shared->reference) ||
                !task_object.callback(shared->compare) || (shared->observe && !task_object.callback(shared->observe)))))
            return std::unexpected{binary_error("kernel.package.collective_invalid",
                                               "Collective manifest and task callbacks are incompatible",
                                               manifest.task->oracle_path)};
        if ((*task_api)->inspect && !task_object.callback((*task_api)->inspect))
            return std::unexpected{binary_error("kernel.package.callback_invalid",
                                               "Task inspection callback is outside the admitted binary",
                                               manifest.task->oracle_path)};
        if (!implementation_object.callback((*implementation_api)->execute))
            return std::unexpected{binary_error("kernel.package.callback_invalid",
                                               "Execute callback is absent from the admitted binary",
                                               manifest.module_path)};
        const auto finalize = (*implementation_api)->finalize;
        if (manifest.task->collective_finalizer != (finalize != nullptr) ||
            (finalize && !implementation_object.callback(finalize)))
            return std::unexpected{binary_error("kernel.package.collective_finalizer_invalid",
                                               "Collective manifest and implementation finalizer differ",
                                               manifest.module_path)};
        return Module{std::make_shared<State>(State{std::move(*task), std::move(*implementation),
            *task_api, *implementation_api, manifest.task, manifest.tuning, manifest.accepts})};
    } catch (const std::bad_alloc&) {
        return std::unexpected{error("kernel.package.allocation_failed",
                                     "Cannot allocate package loader state", "Package.load")};
    }
}

auto Module::collective() const noexcept -> bool {
    return state_ && state_->task && state_->task->collective;
}

auto Module::collective_finalizer() const noexcept -> bool {
    return state_ && state_->implementation && state_->implementation->finalize;
}

auto Module::shared_cpu_local() const noexcept -> bool {
    return state_ && state_->manifest->shared_cpu_local;
}

auto Module::per_worker_input() const noexcept -> bool {
    return state_ && state_->manifest->per_worker_input;
}

auto Module::mapping() const noexcept -> MappingCallbacks {
    return state_ ? state_->mapping : MappingCallbacks{};
}

auto Module::control_inputs() const noexcept -> std::span<const std::string> {
    return state_ ? std::span<const std::string>{state_->manifest->control_inputs} : std::span<const std::string>{};
}

auto Module::partition_extent_parameters() const noexcept -> std::span<const std::string> {
    return state_ ? std::span<const std::string>{state_->manifest->partition_extent_parameters}
                  : std::span<const std::string>{};
}

struct PreparedTrial::State {
    std::shared_ptr<const Module::State> module;
    TaskCall call;
    nlohmann::json tuning;
    std::vector<EpycBenchPackageValueV1> tuning_values;
    std::vector<Allocation> allocations, reference_allocations;
    std::vector<std::span<const std::byte>> observed_regions;
    std::vector<BorrowedInput> immutable;
    std::vector<EpycBenchPackageReadRegionV1> inputs, actual_outputs, expected_outputs;
    std::vector<EpycBenchPackageWriteRegionV1> initialize, outputs, reference, scratch;
    Trial evidence;
    std::optional<WorkerPerfBracket> counters;

    auto prepare_regions(const EpycBenchPackageLayoutV1& layout, ResourceProvider provider, RegionBindings bindings)
        -> std::expected<void, core::Error> {
        const auto requirements = std::span{layout.regions}.first(layout.region_count);
        std::vector<AddressRange> ranges;
        ranges.reserve(requirements.size() * 2U);
        evidence.regions.reserve(requirements.size());
        allocations.reserve(requirements.size());
        for (const auto& region : requirements) {
            auto& entry = evidence.regions.emplace_back(RegionEvidence{
                .name = region.name, .kind = region.kind, .bytes = region.bytes,
                .alignment = region.alignment, .before = {}, .after = {}, .reference = {}});
            if (region.kind == EPYCBENCH_PACKAGE_INPUT && !bindings.borrowed_inputs.empty()) {
                const auto& source = *std::ranges::find(bindings.borrowed_inputs, entry.name, &BorrowedInput::name);
                allocations.emplace_back();
                observed_regions.push_back(source.bytes);
                immutable.push_back({entry.name, source.owner, source.bytes});
                inputs.push_back({entry.name.c_str(), reinterpret_cast<const unsigned char*>(source.bytes.data()),
                    source.bytes.size()});
                continue;
            }
            const AllocationRequest allocation_request{entry.name, region.kind,
                static_cast<std::size_t>(region.bytes), static_cast<std::size_t>(region.alignment),
                call.request.rank, false};
            auto allocation = allocate_region(provider, allocation_request, ranges);
            if (!allocation) return std::unexpected{std::move(allocation.error())};
            const auto bytes = allocations.emplace_back(std::move(*allocation)).bytes;
            observed_regions.push_back(bytes);
            const auto view = writable(bytes, entry.name.c_str());
            if (region.kind == EPYCBENCH_PACKAGE_INPUT) {
                std::ranges::fill(bytes, std::byte{0});
                initialize.push_back(view);
                inputs.push_back(readable(view));
                immutable.push_back({entry.name, allocations.back().owner, bytes});
            } else if (region.kind == EPYCBENCH_PACKAGE_OUTPUT) {
                auto reference_request = allocation_request;
                reference_request.reference_output = true;
                auto expected = allocate_region(provider, reference_request, ranges);
                if (!expected) return std::unexpected{std::move(expected.error())};
                const auto reference_bytes = reference_allocations.emplace_back(std::move(*expected)).bytes;
                std::ranges::fill(reference_bytes, std::byte{0x5a});
                if (std::ranges::contains(bindings.export_outputs, entry.name))
                    evidence.outputs.push_back({entry.name, allocations.back().owner, bytes});
                outputs.push_back(view);
                actual_outputs.push_back(readable(view));
                reference.push_back(writable(reference_bytes, entry.name.c_str()));
                expected_outputs.push_back(readable(reference.back()));
            } else {
                std::ranges::fill(bytes, std::byte{0});
                scratch.push_back(view);
            }
        }
        return {};
    }
};

PreparedTrial::PreparedTrial(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
PreparedTrial::PreparedTrial(PreparedTrial&&) noexcept = default;
auto PreparedTrial::operator=(PreparedTrial&&) noexcept -> PreparedTrial& = default;
PreparedTrial::~PreparedTrial() = default;

auto PreparedTrial::collective_input() const noexcept -> EpycBenchPackageCollectiveInputV1 {
    if (!state_) return {};
    return {&state_->call.request, state_->inputs.data(), static_cast<std::uint32_t>(state_->inputs.size())};
}

auto PreparedTrial::immutable_inputs() const noexcept -> std::span<const BorrowedInput> {
    return state_ ? std::span<const BorrowedInput>{state_->immutable} : std::span<const BorrowedInput>{};
}

auto layout(const Module& module, const Request& request, std::uint64_t maximum_region_bytes)
    -> std::expected<EpycBenchPackageLayoutV1, core::Error> {
    try {
        if (!module.state_ || !module.state_->task)
            return std::unexpected{error("kernel.package.request_invalid", "Package needs a prepared-region task",
                                         "Package.prepare")};
        auto call = task_call(*module.state_->manifest, request);
        if (!call) return std::unexpected{std::move(call.error())};
        if (auto accepted = kernel::packages::validate_implementation_inputs(module.state_->accepts,
                call->parameters, "parameters"); !accepted)
            return std::unexpected{std::move(accepted.error())};
        return task_layout(*module.state_->task, *call, request, maximum_region_bytes);
    } catch (const std::bad_alloc&) {
        return std::unexpected{error("kernel.package.allocation_failed",
                                     "Cannot allocate package layout state", "Package.prepare")};
    }
}

auto inspect_task(const Module& module, const Request& request, const EpycBenchPackageLayoutV1& geometry)
    -> std::expected<std::optional<nlohmann::json>, core::Error> {
    if (!module.state_ || !module.state_->task || !module.state_->task->inspect) return std::nullopt;
    try {
        auto call = task_call(*module.state_->manifest, request);
        if (!call) return std::unexpected{std::move(call.error())};
        if (!request.inspection.empty() && request.inspection.size() != request.rank_count)
            return std::unexpected{error("kernel.package.inspection_context_invalid",
                                         "Task inspection requires no context or one row per rank", "Package.inspect")};
        call->request.partition_begin = geometry.partition_begin;
        call->request.partition_count = geometry.partition_count;
        EpycBenchPackageInspectionRequestV1 inspection{};
        if (!request.inspection.empty()) {
            const auto& rank = request.inspection[request.rank];
            inspection = {text(rank.workload), text(rank.worker), text(rank.l1_cache), rank.base_page_bytes,
                rank.l1_capacity_bytes, rank.l1_line_bytes, rank.l1_ways};
        }
        auto document = task_json([&](auto* out) {
            return module.state_->task->inspect(&call->request, &inspection, out);
        }, "inspect");
        if (!document) return std::unexpected{std::move(document.error())};
        return std::optional{std::move(*document)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{error("kernel.package.allocation_failed", "Cannot allocate task inspection",
                                     "Package.inspect")};
    }
}

auto prepare(const Module& module, const Request& request, ResourceProvider provider,
             std::uint64_t maximum_region_bytes, RegionBindings bindings)
    -> std::expected<PreparedTrial, core::Error> {
    try {
        if (!module.state_ || !module.state_->task)
            return std::unexpected{error("kernel.package.request_invalid", "Package needs a prepared-region task",
                                         "Package.prepare")};
        if (!provider.allocate) provider.allocate = allocate_default;
        auto state = std::make_unique<PreparedTrial::State>();
        state->module = module.state_;
        if (request.hardware_counters)
            state->counters.emplace(WorkerPerfBracket::attach(core::WorkerId{request.rank}, request.supplied_workers));
        const auto& task = *state->module->task;
        const auto& tuning = state->module->tuning;
        auto call = task_call(*state->module->manifest, request);
        if (!call) return std::unexpected{std::move(call.error())};
        if (auto accepted = kernel::packages::validate_implementation_inputs(state->module->accepts,
                call->parameters, "parameters"); !accepted)
            return std::unexpected{std::move(accepted.error())};
        auto resolved_tuning = kernel::packages::resolve_parameters(tuning, request.tuning, "tuning");
        if (!resolved_tuning) return std::unexpected{std::move(resolved_tuning.error())};
        state->call = std::move(*call);
        state->tuning = std::move(*resolved_tuning);
        state->tuning_values = values(tuning, state->tuning);

        auto regions = task_layout(task, state->call, request, maximum_region_bytes);
        if (!regions) return std::unexpected{std::move(regions.error())};
        state->evidence.partition_begin = state->call.request.partition_begin = regions->partition_begin;
        state->evidence.partition_count = state->call.request.partition_count = regions->partition_count;
        if (auto validation = validate_bindings(*regions, bindings); !validation)
            return std::unexpected{std::move(validation.error())};
        if (auto allocation = state->prepare_regions(*regions, provider, bindings); !allocation)
            return std::unexpected{std::move(allocation.error())};
        if (bindings.borrowed_inputs.empty()) {
            if (const auto status = task.initialize(&state->call.request, state->initialize.data(),
                    static_cast<std::uint32_t>(state->initialize.size())); status != 0)
                return std::unexpected{callback_error("initialize", status)};
        }
        for (const auto& binding : bindings.inputs) {
            const auto region = std::ranges::find_if(state->initialize, [&](const auto& input) {
                return input.name == binding.name;
            });
            std::memmove(region->data, binding.bytes.data(), binding.bytes.size());
        }
        for (std::size_t i = 0; i < state->allocations.size(); ++i) {
            auto& region = state->evidence.regions[i];
            if (region.kind == EPYCBENCH_PACKAGE_INPUT)
                region.before = core::sha256(state->observed_regions[i]);
        }

        auto& expected = state->evidence.expected;
        if (const auto status = task.reference(&state->call.request, state->inputs.data(),
                static_cast<std::uint32_t>(state->inputs.size()), state->reference.data(),
                static_cast<std::uint32_t>(state->reference.size()), &expected); status != 0)
            return std::unexpected{callback_error("reference", status)};
        const bool empty_rank = task.collective && regions->partition_count == 0;
        if (expected.repetitions != request.repetitions || (expected.work_units == 0 && !empty_rank) ||
            (state->reference.empty() && expected.output_count != 0))
            return std::unexpected{error("kernel.package.reference_work_invalid",
                                         "Task reference reported invalid expected work",
                                         "Package.prepare")};
        std::size_t output_index = 0;
        for (std::size_t i = 0; i < state->allocations.size(); ++i) {
            auto& region = state->evidence.regions[i];
            if (region.kind != EPYCBENCH_PACKAGE_OUTPUT) continue;
            const auto reference = state->reference_allocations[output_index++].bytes;
            const auto output = state->allocations[i].bytes;
            std::ranges::transform(reference, output.begin(), [](std::byte value) { return ~value; });
            region.before = core::sha256(output);
            region.reference = core::sha256(reference);
        }
        return PreparedTrial{std::move(state)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{error("kernel.package.allocation_failed",
                                     "Cannot allocate package trial resources", "Package.prepare")};
    }
}

auto Trial::valid() const noexcept -> bool {
    return begin_ns && end_ns && *end_ns > *begin_ns && !timing_error && execute_status == 0 &&
           verify_status == 0 && input_unchanged && output_matches && work_matches;
}

auto measure(PreparedTrial&& prepared, Clock clock, FinishGate gate, CollectiveCall collective)
    -> std::expected<Trial, core::Error> {
    auto state = std::move(prepared.state_);
    return PreparedTrial::measure_state(state.get(), clock, gate, collective, false);
}

void PreparedTrial::reset_warmup() noexcept {
    for (const auto& scratch : state_->scratch)
        std::ranges::fill(std::span{scratch.data, scratch.bytes}, static_cast<unsigned char>(0));
    for (std::size_t i = 0; i < state_->outputs.size(); ++i) {
        const auto& output = state_->outputs[i];
        std::ranges::transform(state_->reference_allocations[i].bytes, output.data,
            [](std::byte value) { return std::to_integer<unsigned char>(~value); });
    }
}

auto PreparedTrial::measure_warmup(Clock clock, FinishGate gate) -> std::expected<Trial, core::Error> {
    return measure_state(state_.get(), clock, gate, {}, true);
}

auto PreparedTrial::measure_state(State* state, Clock clock, FinishGate gate, CollectiveCall collective,
                                  bool warmup) -> std::expected<Trial, core::Error> {
    struct Finish {
        FinishGate gate;
        CollectiveCall collective;
        RankWindow window{};
        bool healthy{};
        void wait() noexcept {
            if (const auto cancel = std::exchange(collective.cancel, nullptr); cancel && !healthy)
                cancel(collective.cancellation.context);
            if (const auto callback = std::exchange(gate.wait, nullptr)) callback(gate.context, window);
        }
        ~Finish() { wait(); }
    } finish{gate, collective};
    if (!state)
        return std::unexpected{error("kernel.package.trial_consumed", "Package trial has no resources",
                                     "Package.measure")};
    finish.window.receipt.rank = state->call.request.rank;
    if (static_cast<bool>(state->module->task->collective) !=
            (!collective.shared.empty() || !collective.shared_inputs.empty()) ||
        (state->module->task->collective && (!collective.cancellation.requested || !collective.cancel)))
        return std::unexpected{error("kernel.package.collective_call_invalid",
                                     "Timed call lacks the required collective resources", "Package.measure")};
    if (!clock.now) clock.now = native_now;
    const EpycBenchPackageCallV1 call{&state->call.request,
        state->tuning_values.data(), state->tuning_values.size(), state->inputs.data(),
        static_cast<std::uint32_t>(state->inputs.size()), state->outputs.data(),
        static_cast<std::uint32_t>(state->outputs.size()), state->scratch.data(),
        static_cast<std::uint32_t>(state->scratch.size()), collective.shared.data(),
        static_cast<std::uint32_t>(collective.shared.size()), collective.shared_inputs.data(),
        static_cast<std::uint32_t>(collective.shared_inputs.size()), collective.cancellation};
    Trial warmup_evidence;
    if (warmup) {
        warmup_evidence = state->evidence;
        warmup_evidence.outputs.clear();
    }
    auto& evidence = warmup ? warmup_evidence : state->evidence;
    auto execute = state->module->implementation->execute;
    EpycBenchPackageWorkV1 actual{};
    auto* counters = !warmup && state->counters ? &*state->counters : nullptr;
    if (counters) counters->begin();
    auto begin = clock.now(clock.context);
    if (!begin) return std::unexpected{std::move(begin.error())};
    finish.window.begin_ns = *begin;
    const auto execute_status = execute(&call, &actual);
    auto end = clock.now(clock.context);
    finish.window.receipt = {state->call.request.rank, 1, execute_status, actual};
    if (end) finish.window.end_ns = *end;
    finish.healthy = execute_status == 0 && end && *end > *begin;
    if (!finish.healthy && collective.cancel) collective.cancel(collective.cancellation.context);
    if (counters) evidence.hardware_counters = counters->finish(*begin, end ? std::optional{*end} : std::nullopt);
    finish.wait();
    evidence.execute_status = execute_status;
    evidence.actual = actual;
    evidence.begin_ns = *begin;
    if (end) {
        evidence.end_ns = *end;
        if (*end <= *begin)
            evidence.timing_error = error("kernel.package.clock_interval_invalid",
                                          "Package clock did not advance", "Package.measure");
    } else {
        evidence.timing_error = std::move(end.error());
    }

    evidence.input_unchanged = true;
    for (std::size_t i = 0; i < state->allocations.size(); ++i) {
        auto& region = evidence.regions[i];
        if (region.kind == EPYCBENCH_PACKAGE_SCRATCH) continue;
        region.after = core::sha256(state->observed_regions[i]);
        if (region.kind == EPYCBENCH_PACKAGE_INPUT && region.before != region.after)
            evidence.input_unchanged = false;
    }
    evidence.work_matches = same_work(evidence.expected, evidence.actual);
    evidence.verify_status = state->module->task->compare(&state->call.request,
        state->actual_outputs.data(), state->expected_outputs.data(),
        static_cast<std::uint32_t>(state->actual_outputs.size()));
    evidence.output_matches = evidence.verify_status == 0;
    if (!evidence.valid()) evidence.outputs.clear();
    return std::move(evidence);
}

auto PreparedTrial::stream_reference(std::string_view name) const noexcept -> std::span<const std::byte> {
    const auto found = std::ranges::find_if(state_->expected_outputs,[&](const auto& region) { return region.name == name; });
    if (found == state_->expected_outputs.end()) return {};
    return {reinterpret_cast<const std::byte*>(found->data),static_cast<std::size_t>(found->bytes)};
}

auto PreparedTrial::stream_input(std::string_view name) noexcept -> std::span<std::byte> {
    const auto found = std::ranges::find_if(state_->initialize,[&](const auto& region) { return region.name == name; });
    if (found == state_->initialize.end()) return {};
    return {reinterpret_cast<std::byte*>(found->data),static_cast<std::size_t>(found->bytes)};
}

auto PreparedTrial::StreamCallback::completed() const noexcept -> bool {
    return !error && status == 0 && begin_ns && end_ns && *begin_ns < *end_ns;
}

auto PreparedTrial::invoke_stream(Clock clock) -> StreamCallback {
    if (!clock.now) clock.now = native_now;
    const auto& state = *state_;
    const EpycBenchPackageCallV1 call{&state.call.request,state.tuning_values.data(),state.tuning_values.size(),
        state.inputs.data(),static_cast<std::uint32_t>(state.inputs.size()),state.outputs.data(),
        static_cast<std::uint32_t>(state.outputs.size()),state.scratch.data(),
        static_cast<std::uint32_t>(state.scratch.size()),nullptr,0,nullptr,0,{}};
    StreamCallback invocation;
    auto begin = clock.now(clock.context);
    if (!begin) {
        invocation.error = std::move(begin.error());
        return invocation;
    }
    invocation.begin_ns = *begin;
    invocation.status = state.module->implementation->execute(&call,&invocation.actual);
    auto end = clock.now(clock.context);
    if (!end) invocation.error = std::move(end.error());
    else invocation.end_ns = *end;
    return invocation;
}

auto PreparedTrial::retire_stream(StreamCallback invocation,std::string_view reused_output,
                                  std::span<const std::byte> handed_off) && -> Trial {
    auto& state = *state_;
    auto& evidence = state.evidence;
    evidence.begin_ns = invocation.begin_ns;
    evidence.end_ns = invocation.end_ns;
    evidence.execute_status = invocation.status;
    evidence.actual = invocation.actual;
    evidence.timing_error = std::move(invocation.error);
    evidence.input_unchanged = true;
    for (std::size_t i = 0; i < state.allocations.size(); ++i) {
        auto& region = evidence.regions[i];
        if (region.kind == EPYCBENCH_PACKAGE_SCRATCH) continue;
        auto bytes = std::span<const std::byte>{state.allocations[i].bytes};
        if (region.name == reused_output) {
            if (handed_off.size() != region.bytes) return std::move(evidence);
            const auto output = std::ranges::find_if(state.actual_outputs,[&](const auto& row) {
                return row.name == reused_output;
            });
            output->data = reinterpret_cast<const unsigned char*>(handed_off.data());
            region.before.reset();
            bytes = handed_off;
        }
        region.after = core::sha256(bytes);
        if (region.kind == EPYCBENCH_PACKAGE_INPUT && region.before != region.after) evidence.input_unchanged = false;
    }
    evidence.work_matches = same_work(evidence.expected,evidence.actual);
    evidence.verify_status = state.module->task->compare(&state.call.request,state.actual_outputs.data(),
        state.expected_outputs.data(),static_cast<std::uint32_t>(state.actual_outputs.size()));
    evidence.output_matches = evidence.verify_status == 0;
    evidence.outputs.clear();
    return std::move(evidence);
}

auto collective_layout(const Module& module, const Request& request, std::uint64_t maximum_region_bytes)
    -> std::expected<EpycBenchPackageLayoutV1, core::Error> {
    if (!module.collective() || request.rank != 0)
        return std::unexpected{error("kernel.package.collective_request_invalid",
                                     "Only the collective owner can plan shared regions", "Package.prepare")};
    auto call = task_call(*module.state_->manifest, request);
    if (!call) return std::unexpected{std::move(call.error())};
    if (auto accepted = kernel::packages::validate_implementation_inputs(module.state_->accepts,
            call->parameters, "parameters"); !accepted)
        return std::unexpected{std::move(accepted.error())};
    EpycBenchPackageLayoutV1 result{};
    if (const auto status = module.state_->task->collective->layout(&call->request, &result); status != 0)
        return std::unexpected{callback_error("collective.layout", status)};
    if (result.region_count == 0)
        return std::unexpected{error("kernel.package.collective_layout_empty",
                                     "Collective task requires shared regions", "Package.prepare")};
    if (auto validation = validate_layout(result, maximum_region_bytes, request, true); !validation)
        return std::unexpected{std::move(validation.error())};
    for (const auto& name : module.state_->manifest->control_inputs) {
        const auto regions = std::span{result.regions}.first(result.region_count);
        if (!std::ranges::any_of(regions, [&](const auto& region) {
                return region.kind == EPYCBENCH_PACKAGE_SHARED_INPUT && region.name == name;
            }))
            return std::unexpected{error("kernel.package.control_input_invalid",
                "Declared control input is not an immutable shared region", "Package.prepare", {{"region", name}})};
    }
    return result;
}

struct PreparedCollective::State {
    std::shared_ptr<const Module::State> module;
    TaskCall task;
    nlohmann::json tuning;
    std::vector<EpycBenchPackageValueV1> tuning_values;
    std::vector<Allocation> actual, expected;
    std::vector<EpycBenchPackageWriteRegionV1> initialize, shared_views, reference_views;
    std::vector<EpycBenchPackageReadRegionV1> observed, shared_inputs, reference_output;
    std::vector<RegionEvidence> regions;
    EpycBenchPackageWorkV1 expected_work{};
    std::atomic<bool> cancelled{false};

    static void cancel(void* context) noexcept {
        static_cast<State*>(context)->cancelled.store(true, std::memory_order_release);
    }
    static auto cancellation_requested(void* context) noexcept -> std::uint32_t {
        return static_cast<State*>(context)->cancelled.load(std::memory_order_acquire) ? 1U : 0U;
    }
};

PreparedCollective::PreparedCollective(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
PreparedCollective::PreparedCollective(PreparedCollective&&) noexcept = default;
auto PreparedCollective::operator=(PreparedCollective&&) noexcept -> PreparedCollective& = default;
PreparedCollective::~PreparedCollective() = default;

auto prepare_collective(const Module& module, const Request& request, const EpycBenchPackageLayoutV1& expected,
                        ResourceProvider provider, std::uint64_t maximum_region_bytes)
    -> std::expected<PreparedCollective, core::Error> {
    auto geometry = collective_layout(module, request, maximum_region_bytes);
    if (!geometry) return std::unexpected{std::move(geometry.error())};
    if (!same_layout(*geometry, expected))
        return std::unexpected{error("kernel.package.collective_layout_changed",
                                     "Collective layout changed after planning", "Package.prepare")};
    auto task = task_call(*module.state_->manifest, request);
    if (!task) return std::unexpected{std::move(task.error())};
    if (!provider.allocate) provider.allocate = allocate_default;
    auto state = std::make_unique<PreparedCollective::State>();
    state->module = module.state_;
    state->task = std::move(*task);
    auto resolved_tuning = kernel::packages::resolve_parameters(module.state_->tuning, request.tuning, "tuning");
    if (!resolved_tuning) return std::unexpected{std::move(resolved_tuning.error())};
    state->tuning = std::move(*resolved_tuning);
    state->tuning_values = values(module.state_->tuning, state->tuning);
    std::vector<AddressRange> ranges;
    state->regions.reserve(geometry->region_count);
    for (const auto& region : std::span{geometry->regions}.first(geometry->region_count)) {
        auto& evidence = state->regions.emplace_back(RegionEvidence{
            .name = region.name, .kind = region.kind, .bytes = region.bytes,
            .alignment = region.alignment, .before = {}, .after = {}, .reference = {}});
        AllocationRequest allocation_request{evidence.name, region.kind,
            static_cast<std::size_t>(region.bytes), static_cast<std::size_t>(region.alignment), 0, false};
        auto actual = allocate_region(provider, allocation_request, ranges);
        if (!actual) return std::unexpected{std::move(actual.error())};
        const auto bytes = state->actual.emplace_back(std::move(*actual)).bytes;
        const auto view = writable(bytes, evidence.name.c_str());
        state->initialize.push_back(view);
        state->observed.push_back(readable(view));
        if (region.kind == EPYCBENCH_PACKAGE_SHARED_INPUT) state->shared_inputs.push_back(readable(view));
        else state->shared_views.push_back(view);
        std::ranges::fill(bytes, std::byte{0});

        allocation_request.reference_output = true;
        auto reference = allocate_region(provider, allocation_request, ranges);
        if (!reference) return std::unexpected{std::move(reference.error())};
        const auto reference_bytes = state->expected.emplace_back(std::move(*reference)).bytes;
        state->reference_views.push_back(writable(reference_bytes, evidence.name.c_str()));
        state->reference_output.push_back(readable(state->reference_views.back()));
        std::ranges::fill(reference_bytes, std::byte{0x5a});
    }
    if (const auto status = state->module->task->collective->initialize(&state->task.request,
            state->initialize.data(), geometry->region_count); status != 0)
        return std::unexpected{callback_error("collective.initialize", status)};
    for (std::size_t i = 0; i < state->regions.size(); ++i)
        state->regions[i].before = core::sha256(state->actual[i].bytes);
    return PreparedCollective{std::move(state)};
}

auto PreparedCollective::reference(std::span<const EpycBenchPackageCollectiveInputV1> inputs)
    -> std::expected<void, core::Error> {
    const auto& request = state_->task.request;
    if (inputs.size() != request.rank_count)
        return std::unexpected{error("kernel.package.collective_inputs_invalid",
                                     "Collective reference is missing ranked inputs", "Package.prepare")};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (!inputs[i].task || inputs[i].task->rank != i || inputs[i].task->rank_count != request.rank_count)
            return std::unexpected{error("kernel.package.collective_inputs_invalid",
                                         "Collective reference received inconsistent ranked inputs",
                                         "Package.prepare")};
    }
    if (const auto status = state_->module->task->collective->reference(&request, inputs.data(), request.rank_count,
            state_->reference_views.data(), static_cast<std::uint32_t>(state_->reference_views.size()),
            &state_->expected_work); status != 0)
        return std::unexpected{callback_error("collective.reference", status)};
    if (state_->expected_work.repetitions != request.repetitions || state_->expected_work.work_units == 0)
        return std::unexpected{error("kernel.package.collective_reference_work_invalid",
                                     "Collective oracle reported invalid expected work", "Package.prepare")};
    for (std::size_t i = 0; i < state_->regions.size(); ++i) {
        auto& region = state_->regions[i];
        region.reference = core::sha256(state_->expected[i].bytes);
        if (region.kind == EPYCBENCH_PACKAGE_SHARED_INPUT && region.before != region.reference)
            return std::unexpected{error("kernel.package.collective_input_reference_mismatch",
                                         "Shared input differs from its independent reference", "Package.prepare",
                                         {{"region", region.name}})};
    }
    return {};
}

auto PreparedCollective::call() const noexcept -> CollectiveCall {
    return {state_->shared_views, state_->shared_inputs, {state_.get(), State::cancellation_requested}, State::cancel};
}

void PreparedCollective::finalize(std::span<const EpycBenchPackageRankReceiptV1> receipts, Clock clock,
                                  FinalizationEvidence& evidence) const {
    if (!clock.now) clock.now = native_now;
    const auto shared = call();
    const EpycBenchPackageCallV1 request{&state_->task.request, state_->tuning_values.data(), state_->tuning_values.size(),
        nullptr, 0, nullptr, 0, nullptr, 0, shared.shared.data(), static_cast<std::uint32_t>(shared.shared.size()),
        shared.shared_inputs.data(), static_cast<std::uint32_t>(shared.shared_inputs.size()), shared.cancellation};
    auto finalize = state_->module->implementation->finalize;
    EpycBenchPackageWorkV1 actual{};
    auto begin = clock.now(clock.context);
    if (!begin) {
        evidence.error = std::move(begin.error());
        return;
    }
    const auto status = finalize(&request, receipts.data(),
        static_cast<std::uint32_t>(receipts.size()), &actual);
    auto end = clock.now(clock.context);
    evidence.begin_ns = *begin;
    evidence.execute_status = status;
    evidence.actual = actual;
    if (end) evidence.end_ns = *end;
    else evidence.error = std::move(end.error());
}

void PreparedCollective::verify(std::span<const EpycBenchPackageRankReceiptV1> receipts,
                                CollectiveEvidence& evidence) const {
    evidence.cancelled = state_->cancelled.load(std::memory_order_acquire);
    evidence.expected = state_->expected_work;
    evidence.regions = state_->regions;
    for (std::size_t i = 0; i < state_->regions.size(); ++i)
        evidence.regions[i].after = core::sha256(state_->actual[i].bytes);
    evidence.verify_status = state_->module->task->collective->compare(&state_->task.request, state_->observed.data(),
        state_->reference_output.data(), static_cast<std::uint32_t>(state_->observed.size()), receipts.data(),
        static_cast<std::uint32_t>(receipts.size()), &evidence.actual);
    if (const auto observe = state_->module->task->collective->observe;
        observe && !evidence.cancelled && evidence.verify_status == 0 && same_work(evidence.expected, evidence.actual) &&
        shared_inputs_match(evidence.regions)) {
        auto observation = task_json([&](auto* out) {
            return observe(&state_->task.request, state_->observed.data(), static_cast<std::uint32_t>(state_->observed.size()),
                receipts.data(), static_cast<std::uint32_t>(receipts.size()), out);
        }, "collective.observe");
        if (observation) evidence.task_observation = std::move(*observation);
        else evidence.error = std::move(observation.error());
    }
}

auto CollectiveEvidence::valid() const noexcept -> bool {
    const auto finalizer_ok = !finalization || (!finalization->error && finalization->execute_status == 0 &&
        finalization->release_ns && finalization->scan_end_ns && finalization->begin_ns && finalization->end_ns &&
        *finalization->release_ns <= *finalization->scan_end_ns && *finalization->scan_end_ns <= *finalization->begin_ns &&
        *finalization->begin_ns < *finalization->end_ns && same_work(expected, finalization->actual));
    return shared_inputs_match(regions) && finalizer_ok && !error && !cancelled && verify_status == 0 &&
           expected.work_units > 0 && same_work(expected, actual) &&
           begin_ns && end_ns && *begin_ns < *end_ns &&
           overlap_begin_ns && overlap_end_ns && *overlap_begin_ns < *overlap_end_ns;
}
} // namespace epycbench::platform_linux::package_execution
