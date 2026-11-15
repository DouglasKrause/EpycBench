#include "epycbench/platform_linux/numa_policy.hpp"

#include "platform_linux/numa_operations.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <exception>
#include <linux/mempolicy.h>
#include <new>
#include <numaif.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace epycbench::platform_linux::detail {
namespace {

[[nodiscard]] auto numa_error(std::string_view code, std::string_view message,
                              std::string_view operation, std::string_view classification,
                              std::optional<int> number = std::nullopt) -> core::Error {
    std::optional<core::OsError> os_error;
    if (number && *number != 0) {
        os_error = core::OsError::capture(std::error_code{*number, std::generic_category()});
    }
    return {.category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{std::string{code}},
            .message = std::string{message},
            .operation = std::string{operation},
            .os_error = std::move(os_error),
            .context = nlohmann::json{{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

[[nodiscard]] auto bind_classification(int number) -> std::string_view {
    switch (number) {
    case EFAULT: return "invalid_address_range";
    case EINVAL: return "invalid_bind_request";
    case ENOMEM: return "resource_unavailable";
    case EPERM: return "permission_denied";
    case ENODEV: return "node_unavailable";
    default: return "bind_failure";
    }
}

[[nodiscard]] auto production_bind(void*, void* address, unsigned long length, int mode,
                                   const unsigned long* mask, unsigned long maxnode,
                                   unsigned flags) noexcept -> RawNumaResult {
    const auto value = ::mbind(address, length, mode, mask, maxnode, flags);
    return value == -1 ? RawNumaResult{value, errno} : RawNumaResult{value, 0};
}

[[nodiscard]] auto production_query(void*, int pid, unsigned long count, void** pages,
                                    const int* nodes, int* status, int flags) noexcept
    -> RawNumaResult {
    const auto value = ::move_pages(pid, count, pages, nodes, status, flags);
    return value == -1 ? RawNumaResult{value, errno} : RawNumaResult{value, 0};
}
auto production_allowed(void*, unsigned long* mask, unsigned long maxnode,
                        unsigned long flags) noexcept -> RawNumaResult {
    const auto value =
        ::get_mempolicy(nullptr, mask, maxnode, nullptr, static_cast<unsigned>(flags));
    return value == -1 ? RawNumaResult{value, errno} : RawNumaResult{value, 0};
}

auto allocate_words(std::size_t count) -> std::vector<unsigned long> {
    return std::vector<unsigned long>(count, 0UL);
}

} // namespace

auto production_numa_operations() noexcept -> NumaOperations {
    return {.context = nullptr,
            .bind = production_bind,
            .query_pages = production_query,
            .allowed_mems = production_allowed};
}
auto NumaAccess::checked_domain(const core::NodeSet& possible, std::uint64_t native_max,
                                std::size_t size_max)
    -> std::expected<NumaDomainMaxNode, core::Error> {
    if (possible.empty()) {
        return std::unexpected{numa_error(
            "linux.numa.invalid_argument", "NUMA domain must not be empty",
            "NumaDomainMaxNode.create", "empty_node_set")};
    }
    auto last = possible.begin();
    for (auto node = possible.begin(); node != possible.end(); ++node) last = node;
    const auto highest = static_cast<std::uint64_t>((*last).value());
    if (highest == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected{numa_error("linux.numa.mask_overflow", "NUMA domain size overflow",
                                          "NumaDomainMaxNode.create", "nodemask_overflow")};
    }
    const auto maxnode = highest + 1U;
    constexpr auto word_bits = static_cast<std::uint64_t>(sizeof(unsigned long) * CHAR_BIT);
    const auto words64 = maxnode / word_bits + (maxnode % word_bits != 0U ? 1U : 0U);
    if (maxnode > native_max || words64 > size_max ||
        words64 > size_max / sizeof(unsigned long)) {
        return std::unexpected{numa_error("linux.numa.mask_overflow", "NUMA domain size overflow",
                                          "NumaDomainMaxNode.create", "nodemask_overflow")};
    }
    return NumaDomainMaxNode{static_cast<unsigned long>(maxnode),
                             static_cast<std::size_t>(words64)};
}

auto NumaAccess::reset_allowed_workspace(AllowedMemsWorkspace& workspace,
                                         NumaDomainMaxNode domain,
                                         WordAllocator allocator)
    -> std::expected<void, core::Error> {
    try {
        if (workspace.words.size() != domain.word_count()) {
            workspace.words = allocator != nullptr ? allocator(domain.word_count())
                                                   : allocate_words(domain.word_count());
            if (workspace.words.size() != domain.word_count()) throw std::bad_alloc{};
        } else {
            std::fill(workspace.words.begin(), workspace.words.end(), 0UL);
        }
        workspace.einval_error.emplace(numa_error(
            "linux.numa.bind_failed", "Linux NUMA bind failed", "bind_memory",
            "invalid_bind_request", EINVAL));
        workspace.contained_standard.emplace(numa_error(
            "linux.numa.contained_exception", "Linux NUMA bind contained an exception",
            "bind_memory", "contained_exception"));
        workspace.contained_unknown.emplace(numa_error(
            "linux.numa.contained_unknown_failure",
            "Linux NUMA bind contained an unknown failure", "bind_memory",
            "contained_unknown_failure"));
        return {};
    } catch (...) {
        return std::unexpected{numa_error(
            "linux.numa.conversion_failed", "NUMA workspace allocation failed",
            "AllowedMemsWorkspace.create", "allocation_failure")};
    }
}

auto NumaAccess::take_contained(AllowedMemsWorkspace& workspace, bool standard) noexcept
    -> NumaBindFailure {
    auto& slot = standard ? workspace.contained_standard : workspace.contained_unknown;
    if (!slot) std::terminate();
    auto error = std::move(*slot);
    slot.reset();
    return {std::move(error), std::nullopt};
}

auto NumaAccess::create_allowed_workspace(NumaDomainMaxNode domain, WordAllocator allocator)
    -> std::expected<AllowedMemsWorkspace, core::Error> {
    AllowedMemsWorkspace workspace;
    auto reset = reset_allowed_workspace(workspace, domain, allocator);
    if (!reset) return std::unexpected{std::move(reset.error())};
    return workspace;
}

auto NumaAccess::diagnose_invocation(
    void* address, std::size_t length, std::uint64_t base_page_bytes, int mode,
    unsigned flags, bool selected_mask_nonempty,
    bool selected_maxnode_within_possible_domain, bool selected_nodes_effective,
    bool selected_nodes_online, bool selected_nodes_memory_backed) noexcept
    -> NumaBindEinvalDiagnosis {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (base_page_bytes == 0U || start % base_page_bytes != 0U)
        return NumaBindEinvalDiagnosis::mapping_not_base_page_aligned;
    if (length > std::numeric_limits<std::uintptr_t>::max() - start)
        return NumaBindEinvalDiagnosis::native_range_wraps;
    if (length % base_page_bytes != 0U)
        return NumaBindEinvalDiagnosis::native_range_not_base_page_multiple;
    if (mode != (MPOL_BIND | MPOL_F_STATIC_NODES))
        return NumaBindEinvalDiagnosis::bind_mode_not_static_absolute;
    if (flags != 0U) return NumaBindEinvalDiagnosis::bind_flags_not_zero;
    if (!selected_mask_nonempty) return NumaBindEinvalDiagnosis::selected_mask_empty;
    if (!selected_maxnode_within_possible_domain)
        return NumaBindEinvalDiagnosis::selected_maxnode_outside_possible_domain;
    if (!selected_nodes_effective)
        return NumaBindEinvalDiagnosis::selected_nodes_not_effective;
    if (!selected_nodes_online) return NumaBindEinvalDiagnosis::selected_nodes_not_online;
    if (!selected_nodes_memory_backed)
        return NumaBindEinvalDiagnosis::selected_nodes_not_memory_backed;
    return NumaBindEinvalDiagnosis::well_formed_request_rejected;
}

auto NumaAccess::bind_range_diagnosed(
    void* address, std::size_t length, const NativeNumaMask& nodes,
    NumaDomainMaxNode domain, const NumaBindInvocationFacts& facts,
    AllowedMemsWorkspace& workspace,
    const NumaOperations& operations, std::uint64_t native_length_max)
    -> std::expected<void, NumaBindFailure> {
    if (address == nullptr || length == 0 || length > native_length_max ||
        nodes.maxnode() == 0 || nodes.maxnode() > domain.value() ||
        workspace.words.size() != domain.word_count() || !workspace.einval_error ||
        operations.bind == nullptr || operations.allowed_mems == nullptr) {
        return std::unexpected{NumaBindFailure{
            numa_error("linux.numa.invalid_argument", "Invalid diagnosed NUMA bind input",
                       "bind_memory", "invalid_diagnostic_context"),
            std::nullopt}};
    }

    const auto bound = operations.bind(
        operations.context, address, static_cast<unsigned long>(length),
        MPOL_BIND | MPOL_F_STATIC_NODES, nodes.data(), nodes.native_bind_maxnode_, 0);
    if (bound.value == 0 && bound.error_number == 0) return {};
    if (bound.value != -1 || bound.error_number != EINVAL) {
        auto error = bound.value == -1 && bound.error_number != 0
                         ? numa_error("linux.numa.bind_failed", "Linux NUMA bind failed",
                                      "bind_memory", bind_classification(bound.error_number),
                                      bound.error_number)
                         : numa_error("linux.numa.bind_failed",
                                      "Linux NUMA bind returned inconsistent status",
                                      "bind_memory", "unexpected_native_result");
        return std::unexpected{NumaBindFailure{std::move(error), std::nullopt}};
    }

    std::fill(workspace.words.begin(), workspace.words.end(), 0UL);
    const auto observed = operations.allowed_mems(
        operations.context, workspace.words.data(), domain.value(), MPOL_F_MEMS_ALLOWED);
    auto disposition = AllowedMemsDisposition::allowed_mems_observation_failed;
    if (observed.value == 0 && observed.error_number == 0) {
        disposition = AllowedMemsDisposition::selected_nodes_within_allowed_mems;
        constexpr auto word_bits = sizeof(unsigned long) * CHAR_BIT;
        for (unsigned long value = 0; value < nodes.maxnode(); ++value) {
            const auto node = core::LinuxNumaNodeId{static_cast<std::uint32_t>(value)};
            if (nodes.contains(node) &&
                (workspace.words[value / word_bits] & (1UL << (value % word_bits))) == 0) {
                disposition = AllowedMemsDisposition::selected_nodes_outside_allowed_mems;
                break;
            }
        }
    }
    auto error = std::move(*workspace.einval_error);
    workspace.einval_error.reset();
    auto diagnosis = NumaBindEinvalDiagnosis::allowed_mems_observation_failed;
    if (disposition == AllowedMemsDisposition::selected_nodes_outside_allowed_mems) {
        diagnosis = NumaBindEinvalDiagnosis::selected_nodes_outside_allowed_mems;
    } else if (disposition == AllowedMemsDisposition::selected_nodes_within_allowed_mems) {
        diagnosis = diagnose_invocation(
            address, length, facts.base_page_bytes(), MPOL_BIND | MPOL_F_STATIC_NODES, 0U,
            nodes.maxnode() != 0U, nodes.maxnode() <= domain.value(),
            facts.selected_nodes_effective(), facts.selected_nodes_online(),
            facts.selected_nodes_memory_backed());
    }
    return std::unexpected{NumaBindFailure{std::move(error), diagnosis}};
}

auto NumaAccess::checked_layout(std::uint64_t highest, std::uint64_t native_max,
                                std::size_t size_max)
    -> std::expected<MaskLayout, core::Error> {
    constexpr std::string_view operation = "NativeNumaMask.layout";
    if (highest >= std::numeric_limits<std::uint64_t>::max() - 1U) {
        return std::unexpected{numa_error("linux.numa.mask_overflow", "NUMA mask size overflow",
                                          operation, "nodemask_overflow")};
    }
    const auto maxnode = highest + 1U;
    const auto native_bind_maxnode = maxnode + 1U;
    if (native_bind_maxnode > native_max) {
        return std::unexpected{numa_error("linux.numa.mask_overflow", "NUMA mask size overflow",
                                          operation, "nodemask_overflow")};
    }
    constexpr auto word_bits = static_cast<std::uint64_t>(sizeof(unsigned long) * CHAR_BIT);
    const auto words64 = native_bind_maxnode / word_bits +
                         (native_bind_maxnode % word_bits != 0U ? 1U : 0U);
    if (words64 > size_max || words64 > size_max / sizeof(unsigned long)) {
        return std::unexpected{numa_error("linux.numa.mask_overflow", "NUMA mask size overflow",
                                          operation, "nodemask_overflow")};
    }
    return MaskLayout{static_cast<unsigned long>(maxnode),
                      static_cast<unsigned long>(native_bind_maxnode),
                      static_cast<std::size_t>(words64)};
}

auto NumaAccess::create_mask(const core::LinuxNumaNodeSet& nodes, WordAllocator allocator)
    -> std::expected<NativeNumaMask, core::Error> {
    constexpr std::string_view operation = "NativeNumaMask.from_node_set";
    if (nodes.empty()) {
        return std::unexpected{numa_error("linux.numa.invalid_argument",
                                          "NUMA node set must not be empty", operation,
                                          "empty_node_set")};
    }
    try {
        auto highest = nodes.begin();
        for (auto iterator = nodes.begin(); iterator != nodes.end(); ++iterator) highest = iterator;
        auto layout = checked_layout((*highest).value());
        if (!layout) return std::unexpected{layout.error()};
        auto words = allocator(layout->word_count);
        if (words.size() != layout->word_count) {
            return std::unexpected{numa_error("linux.numa.conversion_failed",
                                              "NUMA mask allocation failed", operation,
                                              "allocation_failure")};
        }
        std::fill(words.begin(), words.end(), 0UL);
        constexpr auto word_bits = sizeof(unsigned long) * CHAR_BIT;
        for (const auto node : nodes) {
            const auto value = node.value();
            words[value / word_bits] |= static_cast<unsigned long>(1) << (value % word_bits);
        }
        return NativeNumaMask{std::move(words), layout->maxnode,
                              layout->native_bind_maxnode};
    } catch (const std::bad_alloc&) {
        return std::unexpected{numa_error("linux.numa.conversion_failed",
                                          "NUMA mask allocation failed", operation,
                                          "allocation_failure")};
    } catch (const std::exception&) {
        return std::unexpected{numa_error("linux.numa.contained_exception",
                                          "NUMA mask construction contained an exception",
                                          operation, "contained_exception")};
    } catch (...) {
        return std::unexpected{numa_error("linux.numa.contained_unknown_failure",
                                          "NUMA mask construction contained an unknown failure",
                                          operation, "contained_unknown_failure")};
    }
}

auto NumaAccess::bind_range(void* address, std::size_t length, const NativeNumaMask& nodes,
                            const NumaOperations& operations, std::uint64_t native_length_max)
    -> std::expected<void, core::Error> {
    constexpr std::string_view operation = "bind_memory";
    if (address == nullptr || length == 0 || nodes.word_count() == 0 || nodes.maxnode() == 0) {
        return std::unexpected{numa_error("linux.numa.invalid_argument", "Invalid NUMA bind input",
                                          operation, "invalid_mapping")};
    }
    if (length > native_length_max) {
        return std::unexpected{numa_error("linux.numa.conversion_failed",
                                          "NUMA bind length is not representable", operation,
                                          "length_out_of_range")};
    }
    try {
        const auto raw = operations.bind(operations.context, address,
                                         static_cast<unsigned long>(length),
                                         MPOL_BIND | MPOL_F_STATIC_NODES,
                                         nodes.data(), nodes.native_bind_maxnode_, 0);
        if (raw.value == 0 && raw.error_number == 0) return {};
        if (raw.value == -1 && raw.error_number != 0) {
            return std::unexpected{numa_error("linux.numa.bind_failed", "Linux NUMA bind failed",
                                              operation, bind_classification(raw.error_number),
                                              raw.error_number)};
        }
        return std::unexpected{numa_error("linux.numa.bind_failed",
                                          "Linux NUMA bind returned inconsistent status",
                                          operation, "unexpected_native_result")};
    } catch (const std::exception&) {
        return std::unexpected{numa_error("linux.numa.contained_exception",
                                          "Linux NUMA bind contained an exception", operation,
                                          "contained_exception")};
    } catch (...) {
        return std::unexpected{numa_error("linux.numa.contained_unknown_failure",
                                          "Linux NUMA bind contained an unknown failure", operation,
                                          "contained_unknown_failure")};
    }
}

} // namespace epycbench::platform_linux::detail

namespace epycbench::platform_linux {

NativeNumaMask::NativeNumaMask(std::vector<unsigned long> words, unsigned long maxnode,
                               unsigned long native_bind_maxnode) noexcept
    : words_{std::move(words)}, maxnode_{maxnode},
      native_bind_maxnode_{native_bind_maxnode} {}
auto NativeNumaMask::from_node_set(const core::LinuxNumaNodeSet& nodes)
    -> std::expected<NativeNumaMask, core::Error> {
    return detail::NumaAccess::create_mask(nodes, detail::allocate_words);
}
auto NativeNumaMask::data() const noexcept -> const unsigned long* { return words_.data(); }
auto NativeNumaMask::word_count() const noexcept -> std::size_t { return words_.size(); }
auto NativeNumaMask::maxnode() const noexcept -> unsigned long { return maxnode_; }
auto NativeNumaMask::contains(core::LinuxNumaNodeId node) const noexcept -> bool {
    constexpr auto bits = sizeof(unsigned long) * CHAR_BIT;
    const auto value = node.value();
    return value < maxnode_ &&
           (words_[value / bits] & (static_cast<unsigned long>(1) << (value % bits))) != 0;
}
auto bind_memory(const AnonymousMapping& mapping, const NativeNumaMask& nodes)
    -> std::expected<void, core::Error> {
    if (!mapping.active()) {
        return std::unexpected{detail::numa_error("linux.numa.invalid_argument",
                                                  "NUMA bind requires an active mapping",
                                                  "bind_memory", "invalid_mapping")};
    }
    return detail::NumaAccess::bind_range(const_cast<std::byte*>(mapping.data()), mapping.size(),
                                           nodes, detail::production_numa_operations());
}

} // namespace epycbench::platform_linux
