// Included with the task adapter after the legacy oracle's common types.

auto integer(const EpycBenchPackageTaskRequestV1& request, std::string_view name) -> std::uint64_t {
    return unsigned_parameter(request, name).value_or(0U);
}

#if defined(EPYCBENCH_ALGORITHM_SEARCH_TREE_U64) || defined(EPYCBENCH_ALGORITHM_MERGE_U64) || \
    defined(EPYCBENCH_ALGORITHM_VARINT14)
auto enumeration(const EpycBenchPackageTaskRequestV1& request, std::string_view name) -> std::string_view {
    for (std::uint64_t i{}; i < request.parameter_count; ++i) {
        const auto& item = request.parameters[i];
        if (item.type == EPYCBENCH_PACKAGE_ENUM && item.name.data && item.name.bytes == name.size() &&
            name == std::string_view{item.name.data, name.size()} && item.value.enum_value.data &&
            item.value.enum_value.bytes <= std::numeric_limits<std::size_t>::max())
            return {item.value.enum_value.data, static_cast<std::size_t>(item.value.enum_value.bytes)};
    }
    return {};
}
#endif

#if defined(EPYCBENCH_ALGORITHM_BFS_U64)
using Config = kernel::BfsU64Config;
constexpr auto initial_data = kernel::initialize_bfs_u64_input;
constexpr auto independent_reference = kernel::prepare_bfs_u64_reference;
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) {
    return Config::create(integer(request, "vertex_count"), seed);
}
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), value.output_bytes().value(), value.visited_vertices(), value.visited_vertices()};
}
#elif defined(EPYCBENCH_ALGORITHM_CSR_U64)
using Config = kernel::CsrU64Config;
constexpr auto initial_data = kernel::initialize_csr_u64_input;
constexpr auto independent_reference = kernel::prepare_csr_u64_reference;
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) {
    return Config::create(integer(request, "rows"), integer(request, "columns"), seed);
}
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), value.output_bytes().value(), value.rows(), value.rows()};
}
#elif defined(EPYCBENCH_ALGORITHM_COMPACT_U64)
using Config = kernel::CompactU64Config;
constexpr auto initial_data = kernel::initialize_compact_u64_input;
constexpr auto independent_reference = kernel::prepare_compact_u64_reference;
#elif defined(EPYCBENCH_ALGORITHM_TRANSPOSE_U64) || defined(EPYCBENCH_ALGORITHM_STENCIL_U64)
#if defined(EPYCBENCH_ALGORITHM_TRANSPOSE_U64)
using Config = kernel::TransposeU64Config;
constexpr auto initial_data = kernel::initialize_transpose_u64_input;
constexpr auto independent_reference = kernel::prepare_transpose_u64_reference;
#else
using Config = kernel::StencilU64Config;
constexpr auto initial_data = kernel::initialize_stencil_u64_input;
constexpr auto independent_reference = kernel::prepare_stencil_u64_reference;
#endif
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) {
    return Config::create(integer(request, "rows"), integer(request, "columns"), seed);
}
auto shape(const Config& value) -> Shape {
    const auto count = value.element_count().value();
    return {value.native_bytes(), value.native_bytes(), count, count};
}
#elif defined(EPYCBENCH_ALGORITHM_CONVOLUTION_U64)
using Config = kernel::ConvolutionU64Config;
constexpr auto initial_data = kernel::initialize_convolution_u64_input;
constexpr auto independent_reference = kernel::prepare_convolution_u64_reference;
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) {
    return Config::create(integer(request, "sample_count"), seed);
}
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), value.output_bytes().value(), value.output_samples(), value.output_samples()};
}
#elif defined(EPYCBENCH_ALGORITHM_STABLE_KEY_SORT_U64)
using Config = kernel::StableKeySortU64Config;
constexpr auto initial_data = kernel::initialize_stable_key_sort_u64_input;
constexpr auto independent_reference = kernel::prepare_stable_key_sort_u64_reference;
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) {
    return Config::create(integer(request, "record_count"), seed);
}
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), value.output_bytes().value(), value.record_count(), value.record_count()};
}
#elif defined(EPYCBENCH_ALGORITHM_SEARCH_TREE_U64)
using Config = kernel::SearchTreeU64Config;
constexpr auto initial_data = kernel::initialize_search_tree_u64_input;
constexpr auto independent_reference = kernel::prepare_search_tree_u64_reference;
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) -> std::optional<Config> {
    const auto order = kernel::parse_search_tree_u64_order(enumeration(request, "query_order"));
    if (!order) return std::nullopt;
    const auto value = Config::create(integer(request, "query_count"), *order, seed);
    return value ? std::optional{*value} : std::nullopt;
}
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), value.output_bytes().value(), value.query_count(), value.query_count()};
}
#elif defined(EPYCBENCH_ALGORITHM_SERIALIZE_U64)
using Config = kernel::SerializeU64Config;
constexpr auto independent_reference = kernel::prepare_serialize_u64_reference;
auto initial_data(const Config& value, Output input) {
    return kernel::initialize_sum_u64_input(value.input_configuration(), input);
}
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) {
    return Config::create(integer(request, "element_count"), seed);
}
auto shape(const Config& value) -> Shape {
    const auto count = value.element_count().value();
    return {value.native_bytes(), value.native_bytes(), count, count};
}
#elif defined(EPYCBENCH_ALGORITHM_PACK_U4)
using Config = kernel::PackU4Config;
constexpr auto initial_data = kernel::initialize_pack_u4_input;
constexpr auto independent_reference = kernel::prepare_pack_u4_reference;
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), value.output_bytes().value(), value.element_count().value(),
            value.output_bytes().value()};
}
#elif defined(EPYCBENCH_ALGORITHM_MERGE_U64)
using Config = kernel::MergeU64Config;
constexpr auto initial_data = kernel::initialize_merge_u64_input;
constexpr auto independent_reference = kernel::prepare_merge_u64_reference;
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) -> std::optional<Config> {
    const auto pattern = enumeration(request, "input_pattern");
    if (pattern != "interleaved" && pattern != "separated") return std::nullopt;
    using Pattern = kernel::MergeU64InputPattern;
    const auto value = Config::create(integer(request, "element_count"),
        pattern == "interleaved" ? Pattern::interleaved : Pattern::separated, seed);
    return value ? std::optional{*value} : std::nullopt;
}
auto shape(const Config& value) -> Shape {
    const auto count = value.element_count().value();
    return {value.native_bytes(), value.native_bytes(), count, count};
}
#elif defined(EPYCBENCH_ALGORITHM_PREFIX_SCAN_U64)
using Config = kernel::PrefixScanU64Config;
constexpr auto initial_data = kernel::initialize_prefix_scan_u64_input;
constexpr auto independent_reference = kernel::prepare_prefix_scan_u64_reference;
#elif defined(EPYCBENCH_ALGORITHM_VARINT14)
using Config = kernel::Varint14Config;
constexpr auto initial_data = kernel::initialize_varint14_input;
constexpr auto independent_reference = kernel::prepare_varint14_reference;
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) -> std::optional<Config> {
    const auto distribution = enumeration(request, "distribution");
    if (distribution != "grouped" && distribution != "shuffled") return std::nullopt;
    using Distribution = kernel::Varint14Distribution;
    const auto value = Config::create(integer(request, "record_count"),
        distribution == "grouped" ? Distribution::grouped : Distribution::shuffled, seed);
    return value ? std::optional{*value} : std::nullopt;
}
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), 0U, value.record_count().value(), 1U};
}
#elif defined(EPYCBENCH_ALGORITHM_CHASE4_U64)
using Config = kernel::Chase4U64Config;
constexpr auto initial_data = kernel::initialize_chase4_u64_input;
constexpr auto independent_reference = kernel::prepare_chase4_u64_reference;
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) {
    return Config::create(integer(request, "nodes_per_lane"), seed);
}
#elif defined(EPYCBENCH_ALGORITHM_MIN_U64)
using Config = kernel::MinU64Config;
constexpr auto initial_data = kernel::initialize_min_u64_input;
constexpr auto independent_reference = kernel::prepare_min_u64_reference;
#elif defined(EPYCBENCH_ALGORITHM_MAX_U64)
using Config = kernel::MaxU64Config;
constexpr auto initial_data = kernel::initialize_max_u64_input;
constexpr auto independent_reference = kernel::prepare_max_u64_reference;
#elif defined(EPYCBENCH_ALGORITHM_SUM_F64)
using Config = kernel::SumF64Config;
constexpr auto initial_data = kernel::initialize_sum_f64_input;
constexpr auto independent_reference = kernel::prepare_sum_f64_reference;
#elif defined(EPYCBENCH_ALGORITHM_DOT_F64)
using Config = kernel::DotF64Config;
constexpr auto initial_data = kernel::initialize_dot_f64_input;
constexpr auto independent_reference = kernel::prepare_dot_f64_reference;
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t) {
    return Config::create(integer(request, "element_count"));
}
#elif defined(EPYCBENCH_ALGORITHM_NORM_L1_F64)
using Config = kernel::NormL1F64Config;
constexpr auto initial_data = kernel::initialize_norm_l1_f64_input;
constexpr auto independent_reference = kernel::prepare_norm_l1_f64_reference;
#endif

#if defined(EPYCBENCH_ALGORITHM_COMPACT_U64) || defined(EPYCBENCH_ALGORITHM_PACK_U4) || \
    defined(EPYCBENCH_ALGORITHM_PREFIX_SCAN_U64) || defined(EPYCBENCH_ALGORITHM_MIN_U64) || \
    defined(EPYCBENCH_ALGORITHM_MAX_U64) || defined(EPYCBENCH_ALGORITHM_SUM_F64) || \
    defined(EPYCBENCH_ALGORITHM_NORM_L1_F64)
auto create(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) {
    return Config::create(integer(request, "element_count"), seed);
}
#endif

#if defined(EPYCBENCH_ALGORITHM_COMPACT_U64) || defined(EPYCBENCH_ALGORITHM_PREFIX_SCAN_U64)
auto shape(const Config& value) -> Shape {
    const auto count = value.element_count().value();
    return {value.native_bytes(), value.native_bytes(), count, count};
}
#endif

#if defined(EPYCBENCH_BUNDLED_CHECKSUM_ONLY) && !defined(EPYCBENCH_ALGORITHM_VARINT14)
auto shape(const Config& value) -> Shape {
    return {value.native_bytes(), 0U, value.element_count().value(), 1U};
}
#endif

auto config(const EpycBenchPackageTaskRequestV1& request, std::uint64_t seed) -> std::optional<Config> {
    const auto value = create(request, seed);
    return value ? std::optional{*value} : std::nullopt;
}

auto extent(const EpycBenchPackageTaskRequestV1&, const Config& value) -> Partition {
    return {0U, shape(value).units_per_repetition};
}

auto make_reference(const Config& value, Input input, [[maybe_unused]] Output output) {
#if defined(EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
    return independent_reference(value, input);
#else
    return independent_reference(value, input, output);
#endif
}
