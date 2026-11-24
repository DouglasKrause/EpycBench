#include "platform_linux/hugetlb_backing.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <sys/mman.h>

namespace epycbench::platform_linux {
namespace {
auto failed(std::string_view code, std::string_view message) {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{std::string{"linux.hugetlb_backing."} + std::string{code}},
        .message = std::string{message},
        .operation = "observe_hugetlb_backing",
        .os_error = {},
        .context = nlohmann::json::object(),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::invalidate_observation}};
}
auto trim(std::string_view s) -> std::string_view {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == s.npos)
        return {};
    return s.substr(begin, s.find_last_not_of(" \t") - begin + 1U);
}
auto header(std::string_view line) -> std::optional<std::pair<std::uintptr_t, std::uintptr_t>> {
    const auto dash = line.find('-');
    const auto space = line.find(' ');
    if (dash == line.npos || space == line.npos || dash >= space)
        return {};
    std::uintptr_t first{}, last{};
    auto a = std::from_chars(line.data(), line.data() + dash, first, 16);
    auto b = std::from_chars(line.data() + dash + 1, line.data() + space, last, 16);
    if (a.ec != std::errc{} || b.ec != std::errc{} || a.ptr != line.data() + dash ||
        b.ptr != line.data() + space || first >= last)
        return {};
    return std::pair{first, last};
}
auto kib(std::string_view value) -> std::optional<std::uint64_t> {
    value = trim(value);
    if (value.empty())
        return {};
    std::uint64_t count{};
    auto n = std::from_chars(value.data(), value.data() + value.size(), count);
    if (n.ec != std::errc{} ||
        trim(value.substr(static_cast<std::size_t>(n.ptr - value.data()))) != "kB" ||
        count > std::numeric_limits<std::uint64_t>::max() / 1024U)
        return {};
    return count * 1024U;
}
auto huge_flag(std::string_view flags) -> bool {
    while (!(flags = trim(flags)).empty()) {
        const auto end = flags.find_first_of(" \t");
        if (flags.substr(0U, end) == "ht")
            return true;
        if (end == flags.npos)
            break;
        flags.remove_prefix(end);
    }
    return false;
}
} // namespace

auto detail::explicit_hugetlb_2m_flags() noexcept -> int { return MAP_HUGETLB | MAP_HUGE_2MB; }

auto detail::parse_hugetlb_smaps(std::istream& input, std::uintptr_t start, std::size_t extent)
    -> std::expected<HugeTlbBacking, core::Error> {
    if (extent == 0U || extent > std::numeric_limits<std::uintptr_t>::max() - start)
        return failed("extent", "Invalid owned extent");
    HugeTlbBacking backing{};
    using Member = std::uint64_t HugeTlbBacking::*;
    constexpr std::array<std::pair<std::string_view, Member>, 5> fields{
        {{"Size", &HugeTlbBacking::extent_bytes},
         {"KernelPageSize", &HugeTlbBacking::kernel_page_bytes},
         {"MMUPageSize", &HugeTlbBacking::mmu_page_bytes},
         {"Private_Hugetlb", &HugeTlbBacking::private_hugetlb_bytes},
         {"Shared_Hugetlb", &HugeTlbBacking::shared_hugetlb_bytes}}};
    std::array<bool, fields.size()> seen{};
    bool inside{}, flags_seen{};
    std::array<char, 8192U> buffer{};
    std::size_t consumed{};
    while (input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {
        consumed += static_cast<std::size_t>(input.gcount());
        if (consumed > 16U * 1024U * 1024U)
            return failed("limit", "Mapping metadata exceeded the read bound");
        const std::string_view line{buffer.data()};
        if (const auto range = header(line)) {
            if (inside || range->first >= start + extent)
                break;
            if (range->second <= start)
                continue;
            if (range->first != start || range->second != start + extent)
                return failed("extent", "Mapping metadata spans a different interval");
            inside = true;
            continue;
        }
        if (!inside)
            continue;
        const auto colon = line.find(':');
        if (colon == line.npos)
            return failed("malformed", "Malformed mapping metadata row");
        const auto name = line.substr(0U, colon);
        const auto value = line.substr(colon + 1U);
        if (name == "VmFlags") {
            if (flags_seen)
                return failed("malformed", "Repeated mapping flags");
            flags_seen = true;
            backing.hugetlb_vma = huge_flag(value);
        }
        for (std::size_t i{}; i < fields.size(); ++i) {
            if (name != fields[i].first)
                continue;
            const auto bytes = kib(value);
            if (seen[i] || !bytes)
                return failed("malformed", "Malformed mapping size field");
            seen[i] = true;
            backing.*fields[i].second = *bytes;
        }
    }
    if (input.bad() || (input.fail() && !input.eof()))
        return failed("read", "Mapping metadata read failed");
    if (!inside || !flags_seen || !std::ranges::all_of(seen, [](bool v) { return v; }))
        return failed("missing", "Required owned mapping metadata unavailable");
    return backing;
}
auto observe_hugetlb_backing(std::span<const std::byte> mapping)
    -> std::expected<HugeTlbBacking, core::Error> {
    std::ifstream input{"/proc/self/smaps"};
    if (!input)
        return failed("open", "Could not open self mapping metadata");
    return detail::parse_hugetlb_smaps(input, reinterpret_cast<std::uintptr_t>(mapping.data()),
                                       mapping.size());
}
} // namespace epycbench::platform_linux
