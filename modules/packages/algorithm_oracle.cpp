#include "task_u64_support.hpp"
#if defined(EPYCBENCH_BUNDLED_GATHER_TASK)
#include "kernel/gather_inspection.hpp"
#endif

#include "../output_task_module_support.hpp"

#if defined(EPYCBENCH_BUNDLED_ALGORITHM)
#include EPYCBENCH_ALGORITHM_HEADER
#elif defined(EPYCBENCH_BUNDLED_AFFINE_TASK)
#include "epycbench/kernel/affine_f64_task.hpp"
#elif defined(EPYCBENCH_BUNDLED_GATHER_TASK)
#include "epycbench/kernel/gather_u64_task.hpp"
#elif defined(EPYCBENCH_BUNDLED_GEMM_TASK)
#include "epycbench/kernel/gemm_u64_task.hpp"
#elif defined(EPYCBENCH_BUNDLED_HISTOGRAM_TASK)
#include "epycbench/kernel/histogram_u64_task.hpp"
#elif defined(EPYCBENCH_BUNDLED_SCATTER_TASK)
#include "epycbench/kernel/scatter_u64_task.hpp"
#endif

#include <span>

namespace {

namespace kernel = epycbench::kernel;
using namespace epycbench::bundled_packages;

struct Shape {
    std::uint64_t input_bytes, output_bytes, units_per_repetition, output_count;
};

using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;

#if defined(EPYCBENCH_BUNDLED_ALGORITHM)
#include "algorithm_reference.inc"
#elif defined(EPYCBENCH_BUNDLED_AFFINE_TASK)
using Config = kernel::AffineF64Config;
constexpr auto initial_data = kernel::initialize_affine_f64_input;
constexpr auto make_reference = kernel::prepare_affine_f64_reference;
auto config(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) -> std::optional<Config> {
    const auto total = unsigned_parameter(request, "element_count").value_or(0);
    if (total > kernel::affine_f64_maximum_elements()) return std::nullopt;
    const auto slice = logical_partition(request, total);
    if (!slice) return std::nullopt;
    const auto value = Config::create(slice->count, seed + slice->begin);
    return value ? std::optional{*value} : std::nullopt;
}
auto shape(const Config& value) -> Shape {
    const auto count = value.element_count().value();
    return {value.native_bytes(), value.native_bytes(), count, count};
}
auto extent(const EpycBenchPackageTaskRequestV1& request, const Config&) -> Partition {
    return *logical_partition(request, *unsigned_parameter(request, "element_count"));
}
#elif defined(EPYCBENCH_BUNDLED_GATHER_TASK)
using Config = kernel::GatherU64Config;
constexpr auto initial_data = kernel::initialize_gather_u64_input;
auto make_reference(const Config& value, Input input, Output) {
    return kernel::prepare_gather_u64_reference(value, input);
}
auto config(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) -> std::optional<Config> {
    constexpr std::string_view name = "index_pattern";
    std::optional<kernel::GatherU64IndexPattern> pattern;
    for (std::uint64_t index{}; index < request.parameter_count; ++index) {
        const auto& item = request.parameters[index];
        if (item.type == EPYCBENCH_PACKAGE_ENUM && item.name.bytes == name.size() && item.name.data &&
            name == std::string_view{item.name.data, name.size()} && item.value.enum_value.data &&
            item.value.enum_value.bytes <= std::numeric_limits<std::size_t>::max())
            pattern = kernel::parse_gather_u64_index_pattern({item.value.enum_value.data,
                static_cast<std::size_t>(item.value.enum_value.bytes)});
    }
    if (!pattern) return std::nullopt;
    const auto value = Config::create(unsigned_parameter(request, "value_count").value_or(0),
        unsigned_parameter(request, "index_count").value_or(0), *pattern, seed);
    return value ? std::optional{*value} : std::nullopt;
}
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), 0, value.index_count().value(), 1};
}
auto extent(const EpycBenchPackageTaskRequestV1&, const Config& value) -> Partition {
    return {0, value.index_count().value()};
}
#elif defined(EPYCBENCH_BUNDLED_GEMM_TASK)
struct Config {
    kernel::GemmU64Config whole;
    std::optional<kernel::GemmU64RowPartition> partition;

    [[nodiscard]] auto local_shape() const noexcept -> const kernel::GemmU64Config& {
        return partition ? partition->local_shape() : whole;
    }
    [[nodiscard]] auto maximum_repetitions() const noexcept -> std::uint64_t {
        return whole.maximum_repetitions();
    }
};

auto initial_data(const Config& value, Output output) {
    if (value.partition) return value.partition->initialize_input(output);
    return kernel::initialize_gemm_u64_input(value.whole, output);
}
auto make_reference(const Config& value, Input input, Output output)
    -> std::expected<std::uint64_t, epycbench::core::Error> {
    if (value.partition) return value.partition->prepare_reference(input, output);
    auto reference = kernel::prepare_gemm_u64_reference(value.whole, input, output);
    if (!reference) return std::unexpected{std::move(reference.error())};
    return reference->single_task_answer();
}
auto config(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) -> std::optional<Config> {
    if (request.rank_count == 0 || request.rank_count > 2U || request.rank >= request.rank_count)
        return std::nullopt;
    const auto value = kernel::GemmU64Config::create(unsigned_parameter(request, "rows").value_or(0),
        unsigned_parameter(request, "inner").value_or(0), unsigned_parameter(request, "columns").value_or(0), seed);
    if (!value) return std::nullopt;
    if (request.rank_count == 1U) return Config{*value, std::nullopt};
    const auto part = kernel::GemmU64RowPartition::for_rank(*value, request.rank);
    if (!part) return std::nullopt;
    return Config{*value, *part};
}
auto shape(const Config& value) -> Shape {
    const auto& local = value.local_shape();
    const auto outputs = local.output_elements().value();
    return {local.native_input_bytes(), local.native_output_bytes(), outputs * local.inner(), outputs};
}
auto extent(const EpycBenchPackageTaskRequestV1&, const Config& value) -> Partition {
    return {value.partition ? value.partition->first_row() : 0U, value.local_shape().rows()};
}
#elif defined(EPYCBENCH_BUNDLED_HISTOGRAM_TASK)
using Config = kernel::HistogramU64Config;
constexpr auto initial_data = kernel::initialize_histogram_u64_input;
constexpr auto make_reference = kernel::prepare_histogram_u64_reference;
auto config(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) -> std::optional<Config> {
    const auto value = Config::create(unsigned_parameter(request, "record_count").value_or(0), seed);
    return value ? std::optional{*value} : std::nullopt;
}
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), value.output_bytes().value(), value.record_count(), kernel::histogram_u64_bins};
}
auto extent(const EpycBenchPackageTaskRequestV1&, const Config& value) -> Partition {
    return {0, value.record_count()};
}
#elif defined(EPYCBENCH_BUNDLED_SCATTER_TASK)
using Config = kernel::ScatterU64Config;
constexpr auto initial_data = kernel::initialize_scatter_u64_input;
constexpr auto make_reference = kernel::prepare_scatter_u64_reference;
auto config(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) -> std::optional<Config> {
    const auto value = Config::create(unsigned_parameter(request, "element_count").value_or(0), seed);
    return value ? std::optional{*value} : std::nullopt;
}
auto shape(const Config& value) -> Shape {
    const auto count = value.element_count().value();
    return {value.native_bytes(), value.native_bytes(), count, count};
}
auto extent(const EpycBenchPackageTaskRequestV1&, const Config& value) -> Partition {
    return {0, value.element_count().value()};
}
#endif

auto request_config(const EpycBenchPackageTaskRequestV1* request) -> std::optional<Config> {
    if (!request || !request->parameters || request->repetitions == 0) return std::nullopt;
#if !defined(EPYCBENCH_BUNDLED_AFFINE_TASK) && !defined(EPYCBENCH_BUNDLED_GEMM_TASK)
    if (request->rank != 0 || request->rank_count != 1U || request->partition_begin != 0)
        return std::nullopt;
#endif
#if defined(EPYCBENCH_ALGORITHM_DOT_F64)
    auto value = config(*request, 0U);
#else
    const auto seed = unsigned_parameter(*request, "input_seed");
    if (!seed) return std::nullopt;
    auto value = config(*request, *seed + request->seed);
#endif
    if (!value || request->repetitions > value->maximum_repetitions()) return std::nullopt;
    if (request->partition_begin != 0 || request->partition_count != 0) {
        const auto domain = extent(*request, *value);
        if (domain.begin != request->partition_begin || domain.count != request->partition_count)
            return std::nullopt;
    }
    return value;
}

auto layout(const EpycBenchPackageTaskRequestV1* request, EpycBenchPackageLayoutV1* result)
    noexcept -> int {
    try {
        const auto configuration = request_config(request);
        if (!configuration || !result) return 1;
        const auto bytes = shape(*configuration);
        const auto domain = extent(*request, *configuration);
        result->partition_begin = domain.begin;
        result->partition_count = domain.count;
        result->region_count = 0;
#if defined(EPYCBENCH_ALGORITHM_VARINT14)
        result->preparation_bytes = configuration->record_count().value() * sizeof(std::uint64_t);
#elif defined(EPYCBENCH_ALGORITHM_CHASE4_U64)
        result->preparation_bytes = configuration->nodes_per_lane().value() * sizeof(std::uint64_t);
#endif
        std::uint64_t input_alignment = 64;
#if defined(EPYCBENCH_BUNDLED_GATHER_TASK)
        if (configuration->cache_set_pattern() || configuration->matched_page_pattern())
            input_alignment = kernel::gather_set_page_bytes;
#endif
        result->regions[result->region_count++] = {"input", EPYCBENCH_PACKAGE_INPUT, bytes.input_bytes, input_alignment};
        if (bytes.output_bytes != 0)
            result->regions[result->region_count++] = {"output", EPYCBENCH_PACKAGE_OUTPUT, bytes.output_bytes, 64};
        result->regions[result->region_count++] = {"checksum", EPYCBENCH_PACKAGE_OUTPUT, sizeof(std::uint64_t), 64};
#if defined(EPYCBENCH_BUNDLED_HISTOGRAM_TASK)
        result->regions[result->region_count++] = {"table", EPYCBENCH_PACKAGE_SCRATCH,
            kernel::histogram_u64_hash_slots * 3U * sizeof(std::uint64_t), 64};
#endif
        return 0;
    } catch (...) {
        return 1;
    }
}

auto initialize(const EpycBenchPackageTaskRequestV1* request,
                const EpycBenchPackageWriteRegionV1* inputs, std::uint32_t count) noexcept -> int {
    try {
        const auto configuration = request_config(request);
        if (!configuration || count != 1 || !inputs || !inputs[0].data ||
            inputs[0].bytes != shape(*configuration).input_bytes)
            return 1;
        return initial_data(*configuration, {
            reinterpret_cast<std::byte*>(inputs[0].data), static_cast<std::size_t>(inputs[0].bytes)}) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

auto reference(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageReadRegionV1* inputs,
               std::uint32_t input_count, const EpycBenchPackageWriteRegionV1* outputs,
               std::uint32_t output_count, EpycBenchPackageWorkV1* work) noexcept -> int {
    try {
        const auto configuration = request_config(request);
        if (!configuration || !work || input_count != 1 || !inputs || !inputs[0].data || !outputs) return 1;
        const auto bytes = shape(*configuration);
        const auto checksum_index = bytes.output_bytes == 0 ? 0U : 1U;
        if (inputs[0].bytes != bytes.input_bytes || output_count != checksum_index + 1 ||
            !outputs[checksum_index].data || outputs[checksum_index].bytes != sizeof(std::uint64_t))
            return 1;
        Output output;
        if (checksum_index != 0) {
            if (!outputs[0].data || outputs[0].bytes != bytes.output_bytes) return 1;
            output = {reinterpret_cast<std::byte*>(outputs[0].data), static_cast<std::size_t>(bytes.output_bytes)};
        }
        const auto result = make_reference(*configuration, {
            reinterpret_cast<const std::byte*>(inputs[0].data), static_cast<std::size_t>(bytes.input_bytes)}, output);
        if (!result) return 1;
        const auto repetitions = request->repetitions;
#if defined(EPYCBENCH_BUNDLED_GEMM_TASK)
        const auto checksum = *result;
#else
        const auto checksum = result->single_task_answer();
#endif
        epycbench::module_detail::store_xor_word(outputs[checksum_index].data, 0, checksum * repetitions);
#if defined(EPYCBENCH_ALGORITHM_COMPACT_U64)
        const auto written = result->selected_elements();
#else
        const auto written = bytes.output_count;
#endif
        *work = {repetitions, bytes.units_per_repetition * repetitions, written};
        return 0;
    } catch (...) {
        return 1;
    }
}

auto compare(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageReadRegionV1* observed,
             const EpycBenchPackageReadRegionV1* expected, std::uint32_t output_count) noexcept -> int {
#if defined(EPYCBENCH_ALGORITHM_COMPACT_U64)
    if (output_count != 2U || !observed || !expected ||
        !observed[0].data || !expected[0].data || observed[0].bytes != expected[0].bytes ||
        expected[0].bytes < sizeof(std::uint64_t) ||
        expected[0].bytes > std::numeric_limits<std::size_t>::max())
        return 1;
    const auto entries = epycbench::module_detail::load_sum_word(expected[0].data, 0U);
    if (entries >= expected[0].bytes / sizeof(std::uint64_t)) return 1;
    const auto written = static_cast<std::size_t>((entries + 1U) * sizeof(std::uint64_t));
    if (std::memcmp(observed[0].data, expected[0].data, written) != 0 ||
        compare_outputs(observed + 1, expected + 1, 1U) != 0)
        return 1;
    // Ordinary Run poisons outputs with the complement of the reference.
    // Compaction must leave the unwritten suffix at its initial poison bytes.
    const auto suffix = std::span{observed[0].data, static_cast<std::size_t>(observed[0].bytes)}.subspan(written);
    const auto poison = std::to_integer<unsigned char>(~kernel::compact_u64_unused_byte);
    return std::ranges::all_of(suffix, [poison](auto byte) { return byte == poison; }) ? 0 : 1;
#else
#if defined(EPYCBENCH_BUNDLED_GATHER_TASK) || defined(EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
    constexpr auto required = 1U;
#else
    constexpr auto required = 2U;
#endif
    return output_count == required ? compare_outputs(observed, expected, output_count) : 1;
#endif
}

#if defined(EPYCBENCH_BUNDLED_GATHER_TASK)
auto inspect_gather(const EpycBenchPackageTaskRequestV1* request,
                    const EpycBenchPackageInspectionRequestV1* geometry, EpycBenchPackageJsonV1* out) noexcept -> int {
    try {
        using epycbench::bundled_packages::text_view;
        namespace projection = kernel::detail::gather_inspection;
        using Json = nlohmann::ordered_json;
        const auto configuration = request_config(request);
        if (!configuration || !geometry) return 1;
        const auto unavailable = [](std::string_view reason) {
            return Json{{"status", "unavailable"}, {"reason", reason}};
        };
        Json report = Json::object();
        const auto page_bytes = geometry->base_page_bytes;
        if (page_bytes == 0 || page_bytes % sizeof(std::uint64_t) != 0) {
            report["gather_pages"] = unavailable("base_page_geometry_unavailable");
            report["gather_cache_sets"] = unavailable("base_page_geometry_unavailable");
        } else {
            const auto workload = text_view(geometry->workload);
            auto pages = projection::project_gather_pages(*configuration, page_bytes, workload);
            if (!pages) return 1;
            report["gather_pages"] = std::move(*pages);
            if (request->rank_count != 1 || text_view(geometry->worker).empty() || text_view(geometry->l1_cache).empty() ||
                geometry->l1_capacity_bytes == 0 || geometry->l1_line_bytes == 0 || geometry->l1_ways == 0) {
                report["gather_cache_sets"] = unavailable("single_worker_l1_geometry_unavailable");
            } else {
                auto cache = projection::project_gather_cache_sets(*configuration, page_bytes, workload,
                    {text_view(geometry->l1_cache), text_view(geometry->worker), geometry->l1_capacity_bytes,
                     geometry->l1_line_bytes, geometry->l1_ways});
                report["gather_cache_sets"] = cache ? std::move(*cache) : unavailable("page_offset_model_unavailable");
            }
        }
        auto rendered = report.dump();
        if (out && rendered.size() > out->capacity) {
            report["gather_cache_sets"] = unavailable("projection_exceeds_output_bound");
            rendered = report.dump();
        }
        return epycbench::bundled_packages::write_json(rendered, out);
    } catch (...) {
        return 1;
    }
}
constexpr auto inspection = inspect_gather;
#else
constexpr auto inspection = nullptr;
#endif

} // namespace

extern "C" [[gnu::visibility("default")]] auto epycbench_package_task_v1() noexcept
    -> const EpycBenchPackageTaskV1* {
    static constexpr EpycBenchPackageTaskV1 descriptor{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageTaskV1), layout, initialize, reference, compare, nullptr,
        inspection};
    return &descriptor;
}
