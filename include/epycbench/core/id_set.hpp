#pragma once

#include "epycbench/core/id.hpp"

#include <algorithm>
#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace epycbench::core {

enum class IdSetParseErrorCode {
    empty_input,
    empty_component,
    invalid_number,
    value_out_of_range,
    descending_range,
};

struct IdSetParseError {
    IdSetParseErrorCode code;
    std::size_t offset;

    constexpr auto operator<=>(const IdSetParseError&) const noexcept = default;
};

template <typename Id> class SparseIdSet {
  private:
    struct Range {
        std::uint32_t first;
        std::uint32_t last;

        constexpr auto operator<=>(const Range&) const noexcept = default;
    };

  public:
    class const_iterator {
      public:
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;
        using value_type = Id;
        using reference = Id;

        constexpr const_iterator() noexcept = default;

        [[nodiscard]] constexpr auto operator*() const noexcept -> Id { return Id{value_}; }

        constexpr auto operator++() noexcept -> const_iterator& {
            const auto& range = (*ranges_)[range_index_];
            if (value_ < range.last) {
                ++value_;
            } else {
                ++range_index_;
                if (range_index_ < ranges_->size()) {
                    value_ = (*ranges_)[range_index_].first;
                } else {
                    value_ = 0;
                }
            }
            return *this;
        }

        constexpr auto operator++(int) noexcept -> const_iterator {
            auto previous = *this;
            ++(*this);
            return previous;
        }

        constexpr auto operator==(const const_iterator&) const noexcept -> bool = default;

      private:
        friend class SparseIdSet;

        constexpr const_iterator(const std::vector<Range>* ranges, std::size_t range_index) noexcept
            : ranges_{ranges}, range_index_{range_index},
              value_{range_index < ranges->size() ? (*ranges)[range_index].first : 0} {}

        const std::vector<Range>* ranges_{nullptr};
        std::size_t range_index_{0};
        std::uint32_t value_{0};
    };

    SparseIdSet() = default;

    SparseIdSet(std::initializer_list<Id> ids) {
        ranges_.reserve(ids.size());
        for (const auto id : ids) {
            ranges_.push_back(Range{id.value(), id.value()});
        }
        normalize();
    }

    [[nodiscard]] static auto parse(std::string_view text)
        -> std::expected<SparseIdSet, IdSetParseError> {
        if (text.empty()) {
            return std::unexpected{
                IdSetParseError{IdSetParseErrorCode::empty_input, 0},
            };
        }

        std::vector<Range> ranges;
        std::size_t component_start = 0;

        while (component_start < text.size()) {
            const auto comma = text.find(',', component_start);
            const auto component_end = comma == std::string_view::npos ? text.size() : comma;
            if (component_start == component_end) {
                return std::unexpected{
                    IdSetParseError{IdSetParseErrorCode::empty_component, component_start},
                };
            }

            const auto dash = text.find('-', component_start);
            if (dash == std::string_view::npos || dash >= component_end) {
                auto value = parse_value(text, component_start, component_end);
                if (!value) {
                    return std::unexpected{value.error()};
                }
                ranges.push_back(Range{*value, *value});
            } else {
                if (dash == component_start || dash + 1 == component_end ||
                    text.find('-', dash + 1) < component_end) {
                    return std::unexpected{
                        IdSetParseError{IdSetParseErrorCode::invalid_number, dash},
                    };
                }

                auto first = parse_value(text, component_start, dash);
                if (!first) {
                    return std::unexpected{first.error()};
                }
                auto last = parse_value(text, dash + 1, component_end);
                if (!last) {
                    return std::unexpected{last.error()};
                }
                if (*first > *last) {
                    return std::unexpected{
                        IdSetParseError{IdSetParseErrorCode::descending_range, dash},
                    };
                }
                ranges.push_back(Range{*first, *last});
            }

            if (comma == std::string_view::npos) {
                break;
            }
            component_start = comma + 1;
            if (component_start == text.size()) {
                return std::unexpected{
                    IdSetParseError{IdSetParseErrorCode::empty_component, component_start},
                };
            }
        }

        return from_ranges(std::move(ranges));
    }

    [[nodiscard]] auto to_range_list() const -> std::string {
        std::string result;
        for (const auto& range : ranges_) {
            if (!result.empty()) {
                result.push_back(',');
            }
            result += std::to_string(range.first);
            if (range.first != range.last) {
                result.push_back('-');
                result += std::to_string(range.last);
            }
        }
        return result;
    }

    [[nodiscard]] auto contains(Id id) const noexcept -> bool {
        const auto value = id.value();
        const auto candidate = std::upper_bound(
            ranges_.begin(), ranges_.end(), value,
            [](std::uint32_t needle, const Range& range) { return needle < range.first; });
        if (candidate == ranges_.begin()) {
            return false;
        }
        return value <= std::prev(candidate)->last;
    }

    [[nodiscard]] auto cardinality() const noexcept -> std::uint64_t {
        std::uint64_t result = 0;
        for (const auto& range : ranges_) {
            result += static_cast<std::uint64_t>(range.last) - range.first + 1;
        }
        return result;
    }

    [[nodiscard]] auto empty() const noexcept -> bool { return ranges_.empty(); }

    [[nodiscard]] auto begin() const noexcept -> const_iterator { return {&ranges_, 0}; }

    [[nodiscard]] auto end() const noexcept -> const_iterator { return {&ranges_, ranges_.size()}; }

    [[nodiscard]] auto set_union(const SparseIdSet& other) const -> SparseIdSet {
        auto ranges = ranges_;
        ranges.insert(ranges.end(), other.ranges_.begin(), other.ranges_.end());
        return from_ranges(std::move(ranges));
    }

    [[nodiscard]] auto intersection(const SparseIdSet& other) const -> SparseIdSet {
        std::vector<Range> result;
        std::size_t left = 0;
        std::size_t right = 0;

        while (left < ranges_.size() && right < other.ranges_.size()) {
            const auto& left_range = ranges_[left];
            const auto& right_range = other.ranges_[right];
            const auto first = std::max(left_range.first, right_range.first);
            const auto last = std::min(left_range.last, right_range.last);
            if (first <= last) {
                result.push_back(Range{first, last});
            }

            if (left_range.last < right_range.last) {
                ++left;
            } else {
                ++right;
            }
        }
        return from_ranges(std::move(result));
    }

    [[nodiscard]] auto difference(const SparseIdSet& other) const -> SparseIdSet {
        std::vector<Range> result;
        std::size_t right = 0;

        for (const auto& left_range : ranges_) {
            auto cursor = static_cast<std::uint64_t>(left_range.first);
            while (right < other.ranges_.size() && other.ranges_[right].last < cursor) {
                ++right;
            }

            auto candidate = right;
            while (candidate < other.ranges_.size() &&
                   other.ranges_[candidate].first <= left_range.last) {
                const auto& right_range = other.ranges_[candidate];
                if (right_range.first > cursor) {
                    result.push_back(
                        Range{static_cast<std::uint32_t>(cursor), right_range.first - 1});
                }
                cursor = std::max(cursor, static_cast<std::uint64_t>(right_range.last) + 1);
                if (cursor > left_range.last) {
                    break;
                }
                ++candidate;
            }

            if (cursor <= left_range.last) {
                result.push_back(Range{static_cast<std::uint32_t>(cursor), left_range.last});
            }
        }
        return from_ranges(std::move(result));
    }

    [[nodiscard]] auto is_subset_of(const SparseIdSet& other) const -> bool {
        return difference(other).empty();
    }

    auto operator==(const SparseIdSet&) const noexcept -> bool = default;

  private:
    explicit SparseIdSet(std::vector<Range> ranges) : ranges_{std::move(ranges)} { normalize(); }

    [[nodiscard]] static auto from_ranges(std::vector<Range> ranges) -> SparseIdSet {
        return SparseIdSet{std::move(ranges)};
    }

    [[nodiscard]] static auto parse_value(std::string_view text, std::size_t first,
                                          std::size_t last)
        -> std::expected<std::uint32_t, IdSetParseError> {
        std::uint32_t value = 0;
        const auto* begin = text.data() + first;
        const auto* end = text.data() + last;
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec == std::errc::result_out_of_range) {
            return std::unexpected{
                IdSetParseError{IdSetParseErrorCode::value_out_of_range, first},
            };
        }
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            return std::unexpected{
                IdSetParseError{IdSetParseErrorCode::invalid_number, first},
            };
        }
        return value;
    }

    void normalize() {
        if (ranges_.empty()) {
            return;
        }

        std::ranges::sort(ranges_, {}, &Range::first);
        std::vector<Range> normalized;
        normalized.reserve(ranges_.size());
        normalized.push_back(ranges_.front());

        for (const auto& range : ranges_ | std::views::drop(1)) {
            auto& current = normalized.back();
            const auto adjacent = current.last != std::numeric_limits<std::uint32_t>::max() &&
                                  range.first == current.last + 1;
            if (range.first <= current.last || adjacent) {
                current.last = std::max(current.last, range.last);
            } else {
                normalized.push_back(range);
            }
        }
        ranges_ = std::move(normalized);
    }

    std::vector<Range> ranges_;
};

using CpuSet = SparseIdSet<LinuxCpuId>;
using LinuxNumaNodeSet = SparseIdSet<LinuxNumaNodeId>;
using NodeSet = SparseIdSet<NumaNodeId>;

static_assert(!std::same_as<CpuSet, NodeSet>);
static_assert(!std::same_as<LinuxNumaNodeSet, CpuSet>);
static_assert(!std::same_as<LinuxNumaNodeSet, NodeSet>);

} // namespace epycbench::core
