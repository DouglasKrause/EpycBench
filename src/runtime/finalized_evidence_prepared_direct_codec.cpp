#include "runtime/finalized_evidence_prepared_direct_codec.hpp"
#include "core/seed_derivation_access.hpp"
#include "kernel/kernel_leaf_access.hpp"
#include "measurement/measurement_leaf_access.hpp"
#include "memory/pairwise_memory_access.hpp"
#include "monitoring/context_switch_classifier_kernel.hpp"
#include "monitoring/frequency_classifier_kernel.hpp"
#include "monitoring/page_fault_classifier_kernel.hpp"
#include "monitoring/pairwise_prepared_capture_storage.hpp"
#include "monitoring/temperature_classifier_kernel.hpp"
#include "runtime/finalized_evidence_prepared_resources.generated.hpp"
#include "runtime/finalized_evidence_wire_schema.generated.hpp"
#include "statistics/adaptive_stopping_access.hpp"
#include "statistics/bootstrap_access.hpp"
#include "statistics/log_ratio_access.hpp"
#include "topology/prepared_stable_id_access.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <concepts>
#include <limits>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

namespace epycbench::monitoring::detail {
struct PreparedEvidenceHeaderAccess final {
    [[nodiscard]] static auto policy(std::array<MonitoringChannelDisposition, 4> dispositions,
                                     bool before, bool after, bool during,
                                     std::optional<core::Duration> interval,
                                     OptionalUnavailableSourcePolicy optional) noexcept
        -> MonitoringExecutionPolicy {
        return MonitoringExecutionPolicy{dispositions, before, after, during, interval, optional};
    }
};
} // namespace epycbench::monitoring::detail

namespace epycbench::runtime::detail {
struct PreparedEvidenceHeaderAccess final {
    [[nodiscard]] static auto policy(statistics::BootstrapPolicy comparison,
                                     statistics::BootstrapPolicy stopping, double fraction,
                                     statistics::PracticalEffectThreshold threshold,
                                     statistics::RelativeHalfWidthTarget target) noexcept
        -> StatisticsProductionPolicy {
        return StatisticsProductionPolicy{comparison, stopping, fraction, threshold, target};
    }
};
} // namespace epycbench::runtime::detail

namespace epycbench::statistics::detail {
struct PreparedEvidenceHeaderAccess final {
    [[nodiscard]] static constexpr auto threshold(double delta) noexcept
        -> PracticalEffectThreshold {
        return PracticalEffectThreshold{delta};
    }
};
} // namespace epycbench::statistics::detail

namespace epycbench::runtime::prepared_codec_v1 {
namespace {
consteval auto required_definition(std::string_view id) -> std::uint32_t {
    for (std::uint32_t index{}; index < prepared_resources_v1::detail::definition_stocks.size();
         ++index)
        if (prepared_resources_v1::detail::definition_stocks[index].definition_id == id)
            return index;
    throw "prepared header definition is absent";
}
auto failure(ActivePipelineFailureKind kind, ActivePipelineStage stage, std::uint64_t limit = 0U,
             std::uint64_t observed = 0U) noexcept {
    return ActiveCodecFailure{kind, stage, 0U, limit, observed, std::nullopt};
}
auto continuation(unsigned char value) noexcept -> bool { return (value & 0xc0U) == 0x80U; }
auto valid_utf8(std::string_view value) noexcept -> bool {
    for (std::size_t i{}; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        if (lead >= 0xc2U && lead <= 0xdfU && i + 1U < value.size() &&
            continuation(static_cast<unsigned char>(value[i + 1U]))) {
            i += 2U;
            continue;
        }
        if (lead >= 0xe0U && lead <= 0xefU && i + 2U < value.size()) {
            const auto second = static_cast<unsigned char>(value[i + 1U]);
            const auto third = static_cast<unsigned char>(value[i + 2U]);
            if (continuation(second) && continuation(third) && !(lead == 0xe0U && second < 0xa0U) &&
                !(lead == 0xedU && second >= 0xa0U)) {
                i += 3U;
                continue;
            }
        }
        if (lead >= 0xf0U && lead <= 0xf4U && i + 3U < value.size()) {
            const auto second = static_cast<unsigned char>(value[i + 1U]);
            if (continuation(second) && continuation(static_cast<unsigned char>(value[i + 2U])) &&
                continuation(static_cast<unsigned char>(value[i + 3U])) &&
                !(lead == 0xf0U && second < 0x90U) && !(lead == 0xf4U && second >= 0x90U)) {
                i += 4U;
                continue;
            }
        }
        return false;
    }
    return true;
}
} // namespace

PreparedCanonicalWriter::PreparedCanonicalWriter(PreparedCanonicalWriter&& other) noexcept
    : owner_{std::move(other.owner_)}, storage_{std::move(other.storage_)},
      cursor_{std::exchange(other.cursor_, 0U)}, active_{other.active_}, finished_{other.finished_},
      failed_{other.failed_}, stage_{other.stage_} {
    if (other.owner_) static_cast<void>(other.owner_->fail());
    other.active_ = false;
    other.finished_ = false;
    other.failed_ = true;
}

auto PreparedCanonicalWriter::prepare(prepared_resources_v1::PreparedResourceOwner owner) noexcept
    -> std::expected<PreparedCanonicalWriter, prepared_resources_v1::PreparedResourceError> {
    try {
        const auto bytes = owner.resource().stocks.logical_bytes;
        if (bytes > static_cast<std::uint64_t>(std::vector<char>{}.max_size()) ||
            bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::size_unrepresentable}};
        std::vector<char> storage(static_cast<std::size_t>(bytes));
        // Touch the complete admitted stock before activation.
        for (auto& byte : storage)
            byte = '\0';
        return PreparedCanonicalWriter{std::move(owner), std::move(storage)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedCanonicalWriter::prepare_progress(std::size_t bytes) noexcept
    -> std::expected<PreparedCanonicalWriter, prepared_resources_v1::PreparedResourceError> {
    using Error = prepared_resources_v1::PreparedResourceError;
    using Code = prepared_resources_v1::PreparedResourceErrorCode;
    if (!bytes || bytes > std::vector<char>{}.max_size())
        return std::unexpected{Error{Code::size_unrepresentable}};
    try {
        return PreparedCanonicalWriter{std::nullopt, std::vector<char>(bytes, '\0')};
    } catch (...) { return std::unexpected{Error{Code::allocation_failure}}; }
}
auto PreparedCanonicalWriter::progress_slice(std::size_t begin) const noexcept
    -> std::expected<std::span<const char>, ActiveCodecFailure> {
    if (owner_ || !active_ || finished_ || failed_ || begin > cursor_)
        return std::unexpected{failure(ActivePipelineFailureKind::transport_state, stage_)};
    return std::span<const char>{storage_.data() + begin, cursor_ - begin};
}

auto PreparedCanonicalWriter::activate() noexcept -> std::expected<void, ActiveCodecFailure> {
    if (active_ || finished_ || failed_ || (owner_ && !owner_->activate()))
        return std::unexpected{failure(ActivePipelineFailureKind::transport_state, stage_)};
    active_ = true;
    stage_ = ActivePipelineStage::active;
    return {};
}
auto PreparedCanonicalWriter::fail(ActiveCodecFailure value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (active_)
        if (owner_) static_cast<void>(owner_->fail());
    active_ = false;
    failed_ = true;
    return std::unexpected{value};
}
auto PreparedCanonicalWriter::append_char(char value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (!active_ || finished_ || failed_)
        return std::unexpected{failure(ActivePipelineFailureKind::transport_state, stage_)};
    if (cursor_ == storage_.size())
        return fail(failure(ActivePipelineFailureKind::capacity_invariant,
                            ActivePipelineStage::active, storage_.size(),
                            cursor_ == std::numeric_limits<std::size_t>::max()
                                ? std::numeric_limits<std::uint64_t>::max()
                                : cursor_ + 1U));
    storage_[cursor_++] = value;
    return {};
}
auto PreparedCanonicalWriter::append_literal(std::string_view value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (!active_ || finished_ || failed_)
        return std::unexpected{failure(ActivePipelineFailureKind::transport_state, stage_)};
    if (value.size() > storage_.size() - cursor_)
        return fail(failure(ActivePipelineFailureKind::capacity_invariant,
                            ActivePipelineStage::active, storage_.size(),
                            value.size() > std::numeric_limits<std::size_t>::max() - cursor_
                                ? std::numeric_limits<std::uint64_t>::max()
                                : cursor_ + value.size()));
    for (const char byte : value)
        storage_[cursor_++] = byte;
    return {};
}
auto PreparedCanonicalWriter::append_unsigned(std::uint64_t value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    std::array<char, 20> text{};
    const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{})
        return fail(
            failure(ActivePipelineFailureKind::capacity_invariant, ActivePipelineStage::active));
    return append_literal({text.data(), static_cast<std::size_t>(end - text.data())});
}
auto PreparedCanonicalWriter::append_signed(std::int64_t value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    std::array<char, 20> text{};
    const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{})
        return fail(
            failure(ActivePipelineFailureKind::capacity_invariant, ActivePipelineStage::active));
    return append_literal({text.data(), static_cast<std::size_t>(end - text.data())});
}
auto PreparedCanonicalWriter::reject_domain() noexcept -> std::expected<void, ActiveCodecFailure> {
    return fail(failure(ActivePipelineFailureKind::registry_domain, stage_));
}
auto PreparedCanonicalWriter::append_binary64(std::uint64_t bits) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    constexpr std::string_view digits = "0123456789abcdef";
    std::array<char, 16> text{};
    for (std::size_t i{}; i < text.size(); ++i)
        text[i] = digits[(bits >> ((15U - i) * 4U)) & 0xfU];
    return append_literal({text.data(), text.size()});
}
auto PreparedCanonicalWriter::append_json_string(std::string_view value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (!active_ || finished_ || failed_)
        return std::unexpected{failure(ActivePipelineFailureKind::transport_state, stage_)};
    if (!valid_utf8(value))
        return fail(
            failure(ActivePipelineFailureKind::registry_domain, ActivePipelineStage::active));
    std::size_t required{2U};
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        const std::size_t extra =
            raw == '"' || raw == '\\'                                                           ? 2U
            : byte == 0x08U || byte == 0x09U || byte == 0x0aU || byte == 0x0cU || byte == 0x0dU ? 2U
            : byte < 0x20U                                                                      ? 6U
                           : 1U;
        if (extra > std::numeric_limits<std::size_t>::max() - required)
            return fail(failure(ActivePipelineFailureKind::capacity_invariant,
                                ActivePipelineStage::active, storage_.size(),
                                std::numeric_limits<std::uint64_t>::max()));
        required += extra;
    }
    if (required > storage_.size() - cursor_)
        return fail(failure(ActivePipelineFailureKind::capacity_invariant,
                            ActivePipelineStage::active, storage_.size(),
                            required > std::numeric_limits<std::size_t>::max() - cursor_
                                ? std::numeric_limits<std::uint64_t>::max()
                                : cursor_ + required));
    if (auto result = append_char('"'); !result)
        return result;
    constexpr std::string_view hex = "0123456789abcdef";
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        if (raw == '"' || raw == '\\') {
            if (auto result = append_char('\\'); !result)
                return result;
            if (auto result = append_char(raw); !result)
                return result;
        } else if (byte == 0x08U || byte == 0x09U || byte == 0x0aU || byte == 0x0cU ||
                   byte == 0x0dU) {
            constexpr std::array<char, 5> escaped{'b', 't', 'n', 'f', 'r'};
            constexpr std::array<unsigned char, 5> controls{0x08U, 0x09U, 0x0aU, 0x0cU, 0x0dU};
            std::size_t index{};
            while (controls[index] != byte)
                ++index;
            if (auto result = append_char('\\'); !result)
                return result;
            if (auto result = append_char(escaped[index]); !result)
                return result;
        } else if (byte < 0x20U) {
            const std::array<char, 6> escaped{
                '\\', 'u', '0', '0', hex[byte >> 4U], hex[byte & 0xfU]};
            if (auto result = append_literal({escaped.data(), escaped.size()}); !result)
                return result;
        } else if (auto result = append_char(raw); !result)
            return result;
    }
    return append_char('"');
}
auto PreparedCanonicalWriter::finish() noexcept
    -> std::expected<std::span<const char>, ActiveCodecFailure> {
    if (!active_ || finished_ || failed_ || (owner_ && !owner_->consume()))
        return std::unexpected{failure(ActivePipelineFailureKind::transport_state, stage_)};
    finished_ = true;
    active_ = false;
    stage_ = ActivePipelineStage::encoded;
    return std::span<const char>{storage_.data(), cursor_};
}

PreparedCanonicalCursor::PreparedCanonicalCursor(PreparedCanonicalCursor&& other) noexcept
    : owner_{std::move(other.owner_)}, stack_{std::move(other.stack_)},
      context_{std::move(other.context_)},
      active_{std::exchange(other.active_, false)}, consumed_{std::exchange(other.consumed_, true)},
      decoded_{std::exchange(other.decoded_, false)} {
    static_cast<void>(other.owner_.fail());
}

auto PreparedCanonicalCursor::prepare(prepared_resources_v1::PreparedResourceOwner owner) noexcept
    -> std::expected<PreparedCanonicalCursor, prepared_resources_v1::PreparedResourceError> {
    try {
        const auto depth = owner.resource().stocks.parser_depth;
        if (owner.resource().definitions.size() != wire_v1::detail::schema_nodes.size() ||
            depth == 0U || depth > static_cast<std::uint64_t>(std::vector<Frame>{}.max_size()) ||
            depth > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        for (std::size_t i{}; i < wire_v1::detail::schema_nodes.size(); ++i) {
            const auto kind = wire_v1::detail::schema_nodes[i].kind;
            const auto prepared = owner.resource().definitions[i].kind;
            const bool kind_matches =
                (kind == wire_v1::detail::SchemaKind::scalar &&
                 prepared == prepared_resources_v1::PreparedDefinitionKind::scalar) ||
                (kind == wire_v1::detail::SchemaKind::object &&
                 prepared == prepared_resources_v1::PreparedDefinitionKind::object) ||
                (kind == wire_v1::detail::SchemaKind::array &&
                 prepared == prepared_resources_v1::PreparedDefinitionKind::array) ||
                (kind == wire_v1::detail::SchemaKind::option &&
                 prepared == prepared_resources_v1::PreparedDefinitionKind::option) ||
                (kind == wire_v1::detail::SchemaKind::sum &&
                 prepared == prepared_resources_v1::PreparedDefinitionKind::sum);
            if (!kind_matches ||
                (kind == wire_v1::detail::SchemaKind::array) !=
                    owner.resource().definitions[i].local_array_cardinality.has_value())
                return std::unexpected{prepared_resources_v1::PreparedResourceError{
                    prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch,
                    static_cast<std::uint32_t>(i)}};
        }
        std::vector<Frame> stack(static_cast<std::size_t>(depth));
        for (auto& frame : stack)
            frame = {};
        return PreparedCanonicalCursor{std::move(owner), std::move(stack)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedCanonicalCursor::prepare(prepared_resources_v1::PreparedResourceOwner owner,
    const wire_v1::CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<PreparedCanonicalCursor, prepared_resources_v1::PreparedResourceError> {
    auto result = prepare(std::move(owner));
    if (!result) return result;
    try {
        const auto& p = capacity.prerequisite.parameters;
        if (p.first_total_pages.size() != p.workers.value ||
            p.second_total_pages.size() != p.workers.value ||
            p.first_target_counts.size() != p.workers.value ||
            p.second_target_counts.size() != p.workers.value ||
            p.domain_cpu_ids.size() != std::min<std::uint64_t>(p.workers.value,
                p.present_cpu_ids.size()))
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        Context context;
        context.nodes = p.numa_domain_maxnode.value;
        context.present_cpus = p.present_cpu_ids.size();
        context.maximum_domains = p.domain_cpu_ids.size();
        const std::array pages{&p.first_total_pages, &p.second_total_pages};
        const std::array targets{&p.first_target_counts, &p.second_target_counts};
        for (std::size_t side{}; side != pages.size(); ++side) {
            context.pages[side].reserve(pages[side]->size());
            context.targets[side].reserve(targets[side]->size());
            for (const auto value : *pages[side]) context.pages[side].push_back(value.value);
            for (const auto value : *targets[side]) context.targets[side].push_back(value.value);
        }
        result->context_.emplace(std::move(context));
        return result;
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedCanonicalCursor::activate() noexcept -> std::expected<void, ActiveCodecFailure> {
    if (active_ || consumed_ || !owner_.activate())
        return std::unexpected{
            failure(ActivePipelineFailureKind::transport_state, ActivePipelineStage::prepared)};
    active_ = true;
    return {};
}

auto PreparedTypedBuilderPlan::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedTypedBuilderPlan, prepared_resources_v1::PreparedResourceError> {
    try {
        if (resource.definitions.size() != wire_v1::detail::schema_nodes.size() ||
            resource.stocks.builder_slots >
                static_cast<std::uint64_t>(std::vector<PreparedParsedNode>{}.max_size()))
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        std::vector<PreparedParsedNode> slots(
            static_cast<std::size_t>(resource.stocks.builder_slots));
        std::vector<std::uint64_t> occurrences(resource.definitions.size());
        std::vector<std::uint64_t> limits;
        limits.reserve(resource.definitions.size());
        if (resource.stocks.builder_slots > std::numeric_limits<std::uint64_t>::max() - 7U)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::arithmetic_overflow}};
        const auto required_bits = (resource.stocks.builder_slots + 7U) / 8U;
        if (resource.stocks.builder_initialized_bits != required_bits ||
            required_bits > std::vector<std::uint8_t>{}.max_size())
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        std::vector<std::uint8_t> initialized_bits(static_cast<std::size_t>(required_bits));
        std::uint64_t total{};
        for (const auto& definition : resource.definitions) {
            if (definition.owner_occurrences > std::numeric_limits<std::uint64_t>::max() - total)
                return std::unexpected{prepared_resources_v1::PreparedResourceError{
                    prepared_resources_v1::PreparedResourceErrorCode::arithmetic_overflow}};
            total += definition.owner_occurrences;
            limits.push_back(definition.owner_occurrences);
        }
        if (total != resource.stocks.builder_slots)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        for (auto& slot : slots)
            slot = {};
        return PreparedTypedBuilderPlan{std::move(slots), std::move(occurrences), std::move(limits),
                                        std::move(initialized_bits)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

PreparedTypedBuilderPlan::PreparedTypedBuilderPlan(PreparedTypedBuilderPlan&& other) noexcept
    : slots_{std::move(other.slots_)}, occurrences_{std::move(other.occurrences_)},
      limits_{std::move(other.limits_)}, initialized_bits_{std::move(other.initialized_bits_)},
      initialized_{std::exchange(other.initialized_, 0U)},
      active_{std::exchange(other.active_, false)},
      finished_{std::exchange(other.finished_, false)},
      failed_{std::exchange(other.failed_, true)} {}

auto PreparedTypedBuilderPlan::activate() noexcept -> std::expected<void, ActiveCodecFailure> {
    if (active_ || finished_ || failed_)
        return std::unexpected{
            failure(ActivePipelineFailureKind::transport_state, ActivePipelineStage::prepared)};
    active_ = true;
    return {};
}

auto PreparedTypedBuilderPlan::fail(ActiveCodecFailure value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    active_ = false;
    failed_ = true;
    return std::unexpected{value};
}

auto PreparedTypedBuilderPlan::begin_node(std::uint32_t definition, PreparedParsedNodeKind kind,
                                          std::size_t lexical_begin) noexcept
    -> std::expected<std::size_t, ActiveCodecFailure> {
    if (!active_ || finished_ || failed_ || definition >= limits_.size())
        return std::unexpected{
            failure(ActivePipelineFailureKind::transport_state, ActivePipelineStage::active)};
    const auto occurrence = occurrences_[definition]++;
    if (occurrence >= limits_[definition] || initialized_ == slots_.size()) {
        auto value = failure(ActivePipelineFailureKind::capacity_invariant,
                             ActivePipelineStage::active, limits_[definition], occurrence + 1U);
        value.sequence_index = definition;
        static_cast<void>(fail(value));
        return std::unexpected{value};
    }
    const auto index = initialized_++;
    slots_[index] =
        PreparedParsedNode{definition, occurrence, kind, 0U, 0U, lexical_begin, 0U, true};
    initialized_bits_[index / 8U] |= static_cast<std::uint8_t>(1U << (index % 8U));
    return index;
}

auto PreparedTypedBuilderPlan::finish() noexcept
    -> std::expected<std::span<const PreparedParsedNode>, ActiveCodecFailure> {
    if (!active_ || finished_ || failed_)
        return std::unexpected{
            failure(ActivePipelineFailureKind::transport_state, ActivePipelineStage::active)};
    active_ = false;
    finished_ = true;
    return std::span<const PreparedParsedNode>{slots_.data(), initialized_};
}

namespace {
using wire_v1::detail::ScalarClass;
using wire_v1::detail::SchemaKind;

auto starts(std::span<const char> bytes, std::size_t cursor, std::string_view literal) noexcept
    -> bool {
    return cursor <= bytes.size() && literal.size() <= bytes.size() - cursor &&
           std::equal(literal.begin(), literal.end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
}
auto json_string_end(std::span<const char> bytes, std::size_t& cursor,
                     std::optional<std::uint64_t> maximum, bool& resource_exceeded) noexcept
    -> bool {
    if (cursor == bytes.size() || bytes[cursor++] != '"')
        return false;
    std::uint64_t decoded{};
    while (cursor < bytes.size()) {
        const auto byte = static_cast<unsigned char>(bytes[cursor++]);
        if (byte == '"') {
            resource_exceeded = maximum && decoded > *maximum;
            return !resource_exceeded;
        }
        if (byte < 0x20U)
            return false;
        if (byte == '\\') {
            if (cursor == bytes.size())
                return false;
            const char escaped = bytes[cursor++];
            if (escaped == '"' || escaped == '\\') {
                ++decoded;
                continue;
            }
            if (escaped == 'b' || escaped == 't' || escaped == 'n' || escaped == 'f' ||
                escaped == 'r') {
                ++decoded;
                continue;
            }
            if (escaped != 'u' || cursor + 4U > bytes.size() || bytes[cursor] != '0' ||
                bytes[cursor + 1U] != '0')
                return false;
            const auto hex = [](char value) noexcept -> int {
                if (value >= '0' && value <= '9')
                    return value - '0';
                if (value >= 'a' && value <= 'f')
                    return value - 'a' + 10;
                return -1;
            };
            const int high = hex(bytes[cursor + 2U]);
            const int low = hex(bytes[cursor + 3U]);
            if (high < 0 || low < 0)
                return false;
            const int value = high * 16 + low;
            if (value >= 0x20 || value == 0x08 || value == 0x09 || value == 0x0a || value == 0x0c ||
                value == 0x0d)
                return false;
            cursor += 4U;
            ++decoded;
            continue;
        }
        if (byte < 0x80U) {
            ++decoded;
            continue;
        }
        const std::size_t begin = cursor - 1U;
        std::size_t length{};
        if (byte >= 0xc2U && byte <= 0xdfU)
            length = 2U;
        else if (byte >= 0xe0U && byte <= 0xefU)
            length = 3U;
        else if (byte >= 0xf0U && byte <= 0xf4U)
            length = 4U;
        else
            return false;
        if (length - 1U > bytes.size() - cursor)
            return false;
        const auto second = static_cast<unsigned char>(bytes[begin + 1U]);
        if (!continuation(second) ||
            (length >= 3U && !continuation(static_cast<unsigned char>(bytes[begin + 2U]))) ||
            (length == 4U && !continuation(static_cast<unsigned char>(bytes[begin + 3U]))) ||
            (byte == 0xe0U && second < 0xa0U) || (byte == 0xedU && second >= 0xa0U) ||
            (byte == 0xf0U && second < 0x90U) || (byte == 0xf4U && second >= 0x90U))
            return false;
        cursor += length - 1U;
        decoded += length;
    }
    return false;
}
auto scalar(std::span<const char> bytes, std::size_t& cursor,
            const wire_v1::detail::SchemaNode& node, std::optional<std::uint64_t> decoded_maximum,
            bool& resource_exceeded) noexcept -> bool {
    if (node.scalar_class == ScalarClass::boolean) {
        if (starts(bytes, cursor, "true")) {
            cursor += 4U;
            return true;
        }
        if (starts(bytes, cursor, "false")) {
            cursor += 5U;
            return true;
        }
        return false;
    }
    if (node.scalar_class == ScalarClass::token) {
        for (std::uint32_t i{}; i < node.token_count; ++i) {
            const auto token = wire_v1::detail::scalar_tokens[node.token_begin + i];
            if (cursor < bytes.size() && bytes[cursor] == '"' &&
                starts(bytes, cursor + 1U, token) && cursor + token.size() + 1U < bytes.size() &&
                bytes[cursor + token.size() + 1U] == '"') {
                cursor += token.size() + 2U;
                return true;
            }
        }
        return false;
    }
    if (node.scalar_class == ScalarClass::string)
        return json_string_end(bytes, cursor, decoded_maximum, resource_exceeded);
    if (cursor == bytes.size() || bytes[cursor++] != '"')
        return false;
    const auto begin = cursor;
    while (cursor < bytes.size() && bytes[cursor] != '"')
        ++cursor;
    if (cursor == bytes.size() || cursor == begin)
        return false;
    const auto value = std::string_view{bytes.data() + begin, cursor - begin};
    ++cursor;
    if (node.scalar_class == ScalarClass::hex16 || node.scalar_class == ScalarClass::hex64) {
        const std::size_t width = node.scalar_class == ScalarClass::hex16 ? 16U : 64U;
        return value.size() == width && std::ranges::all_of(value, [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
               });
    }
    const bool signed_value = node.scalar_class == ScalarClass::signed_decimal32 ||
                              node.scalar_class == ScalarClass::signed_decimal64;
    std::size_t index{};
    if (signed_value && value[0] == '-') {
        if (value.size() == 1U)
            return false;
        index = 1U;
        if (value[index] == '0')
            return false;
    }
    if (value[index] == '0' && index + 1U != value.size())
        return false;
    if (!std::ranges::all_of(value.substr(index), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    if (signed_value) {
        std::int64_t parsed{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
            return false;
        if (node.scalar_class == ScalarClass::signed_decimal32 &&
            (parsed < std::numeric_limits<std::int32_t>::min() ||
             parsed > std::numeric_limits<std::int32_t>::max()))
            return false;
    } else {
        std::uint64_t parsed{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
            return false;
    }
    return true;
}
auto ordered_edge(const wire_v1::detail::SchemaNode& node, std::uint32_t ordinal) noexcept
    -> const wire_v1::detail::SchemaEdge& {
    for (std::uint32_t i{}; i < node.edge_count; ++i) {
        const auto candidate = node.edge_begin + i;
        std::uint32_t rank{};
        for (std::uint32_t j{}; j < node.edge_count; ++j)
            if (wire_v1::detail::schema_edges[node.edge_begin + j].key <
                wire_v1::detail::schema_edges[candidate].key)
                ++rank;
        if (rank == ordinal)
            return wire_v1::detail::schema_edges[candidate];
    }
    return wire_v1::detail::schema_edges[node.edge_begin];
}
auto root_schema_scalar() noexcept -> std::uint32_t {
    const auto& root = wire_v1::detail::schema_nodes[wire_v1::detail::evidence_root];
    for (std::uint32_t i{}; i < root.edge_count; ++i) {
        const auto& edge = wire_v1::detail::schema_edges[root.edge_begin + i];
        if (edge.key == "schema")
            return edge.child;
    }
    return std::numeric_limits<std::uint32_t>::max();
}
auto sum_branch(std::span<const char> bytes, std::size_t cursor,
                const wire_v1::detail::SchemaNode& node) noexcept -> std::optional<std::uint32_t> {
    if (cursor == bytes.size() || bytes[cursor] != '{')
        return std::nullopt;
    std::uint64_t nesting{1U};
    bool expect_key = true;
    for (std::size_t at = cursor + 1U; at < bytes.size();) {
        if (bytes[at] == '"') {
            const auto begin = ++at;
            bool escaped{};
            while (at < bytes.size()) {
                if (!escaped && bytes[at] == '"')
                    break;
                escaped = !escaped && bytes[at] == '\\';
                if (bytes[at] != '\\')
                    escaped = false;
                ++at;
            }
            if (at == bytes.size())
                return std::nullopt;
            const auto text = std::string_view{bytes.data() + begin, at - begin};
            ++at;
            if (nesting == 1U && expect_key && text == node.discriminator) {
                if (at + 1U >= bytes.size() || bytes[at++] != ':' || bytes[at++] != '"')
                    return std::nullopt;
                const auto token_begin = at;
                while (at < bytes.size() && bytes[at] != '"')
                    ++at;
                if (at == bytes.size())
                    return std::nullopt;
                const auto token = std::string_view{bytes.data() + token_begin, at - token_begin};
                for (std::uint32_t i{}; i < node.edge_count; ++i) {
                    const auto& edge = wire_v1::detail::schema_edges[node.edge_begin + i];
                    if (token == edge.token)
                        return edge.child;
                }
                return std::nullopt;
            }
            if (nesting == 1U && expect_key)
                expect_key = false;
            continue;
        }
        if (bytes[at] == '{' || bytes[at] == '[')
            ++nesting;
        else if (bytes[at] == '}' || bytes[at] == ']') {
            if (--nesting == 0U)
                break;
        } else if (nesting == 1U && bytes[at] == ',')
            expect_key = true;
        ++at;
    }
    return std::nullopt;
}
} // namespace

auto PreparedCanonicalCursor::walk(std::span<const char> bytes,
                                   PreparedTypedBuilderPlan* plan) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    std::size_t cursor{};
    const auto reject = [&](ActivePipelineFailureKind kind, std::uint64_t limit = 0U,
                            std::uint64_t observed =
                                0U) -> std::expected<void, ActiveCodecFailure> {
        static_cast<void>(owner_.fail());
        active_ = false;
        consumed_ = true;
        auto value = failure(kind, ActivePipelineStage::active, limit, observed);
        value.sequence_index = cursor;
        return std::unexpected{value};
    };
    if (!active_ || consumed_ || decoded_)
        return std::unexpected{
            failure(ActivePipelineFailureKind::transport_state, ActivePipelineStage::prepared)};
    const auto logical = owner_.resource().stocks.logical_bytes;
    if (bytes.size() > logical)
        return reject(ActivePipelineFailureKind::capacity_invariant, logical, bytes.size());
    std::size_t depth{1U};
    std::uint64_t nodes_seen{1U};
    const auto node_limit = owner_.resource().stocks.parser_nodes;
    if (node_limit == 0U)
        return reject(ActivePipelineFailureKind::capacity_invariant, 0U, 1U);
    constexpr auto no_event = std::numeric_limits<std::size_t>::max();
    const auto parsed_kind = [](SchemaKind kind) noexcept {
        switch (kind) {
        case SchemaKind::scalar:
            return PreparedParsedNodeKind::scalar;
        case SchemaKind::object:
            return PreparedParsedNodeKind::object;
        case SchemaKind::array:
            return PreparedParsedNodeKind::array;
        case SchemaKind::option:
            return PreparedParsedNodeKind::option;
        case SchemaKind::sum:
            return PreparedParsedNodeKind::sum;
        }
        return PreparedParsedNodeKind::scalar;
    };
    const auto start_event = [&](std::uint32_t node) noexcept -> std::optional<std::size_t> {
        if (plan == nullptr)
            return no_event;
        auto event =
            plan->begin_node(node, parsed_kind(wire_v1::detail::schema_nodes[node].kind), cursor);
        if (!event)
            return std::nullopt;
        return *event;
    };
    auto root_event = start_event(wire_v1::detail::evidence_root);
    if (!root_event)
        return reject(ActivePipelineFailureKind::capacity_invariant);
    stack_[0] = Frame{wire_v1::detail::evidence_root, 0U, 0U, *root_event, cursor, 0U};
    const auto push = [&](std::uint32_t node, std::uint8_t side = 0U) noexcept -> bool {
        if (depth == stack_.size() || nodes_seen == node_limit)
            return false;
        auto event = start_event(node);
        if (!event)
            return false;
        ++nodes_seen;
        const auto& parent = stack_[depth - 1U];
        Frame next{node, 0U, 0U, *event, cursor, 0U,
            node == wire_v1::detail::monitoring_result_definition
                ? std::uint8_t{0U} : parent.monitoring_presence,
            side != 0U ? side : parent.side,
            parent.owner_pages, parent.owner_targets};
        if (context_) {
            const auto special = wire_v1::detail::schema_nodes[parent.node].special;
            if (special == wire_v1::detail::SchemaSpecial::finalized_workers) {
                if (parent.side == 0U || parent.side > 2U) return false;
                const auto index = static_cast<std::size_t>(parent.side - 1U);
                if (parent.count >= context_->pages[index].size()) return false;
                next.owner_pages = context_->pages[index][static_cast<std::size_t>(parent.count)];
                next.owner_targets = context_->targets[index][static_cast<std::size_t>(parent.count)];
            }
        }
        stack_[depth++] = next;
        return true;
    };
    const auto cardinality = [&](const Frame& frame) -> std::optional<std::uint64_t> {
        const auto& node = wire_v1::detail::schema_nodes[frame.node];
        if (node.cardinality_presence != 0U &&
            (frame.monitoring_presence & node.cardinality_presence) == 0U)
            return 0U;
        if (context_) {
            if (frame.node == required_definition("array__frequency_domain_id__cpus"))
                return context_->present_cpus;
            if (node.special == wire_v1::detail::SchemaSpecial::frequency_domains)
                return context_->maximum_domains;
            if (frame.node == required_definition("array__region_request__numa_targets"))
                return frame.owner_targets;
            if (frame.node == required_definition("array__placement_observation__resident_page_counts"))
                return std::min(frame.owner_pages, context_->nodes);
        }
        return owner_.resource().definitions[frame.node].local_array_cardinality;
    };
    const auto exact = [&](const Frame& frame) noexcept {
        const auto& node = wire_v1::detail::schema_nodes[frame.node];
        return node.exact_cardinality && !(context_ &&
            (node.special == wire_v1::detail::SchemaSpecial::frequency_domains ||
             frame.node == required_definition("array__frequency_domain_id__cpus") ||
             frame.node == required_definition("array__raw_capture__context_acquisitions") ||
             frame.node == required_definition("array__raw_capture__temperature_acquisitions") ||
             frame.node == required_definition("array__frequency_domain_acquisitions__acquisitions")));
    };
    while (depth != 0U) {
        auto& frame = stack_[depth - 1U];
        const auto& node = wire_v1::detail::schema_nodes[frame.node];
        if (node.kind == SchemaKind::scalar) {
            if (frame.node == root_schema_scalar()) {
                if (!starts(bytes, cursor, "\"0\""))
                    return reject(ActivePipelineFailureKind::registry_domain);
                cursor += 3U;
                if (plan != nullptr) {
                    auto& event = plan->slots_[frame.event];
                    event.lexical_size = cursor - frame.lexical_begin;
                }
                --depth;
                continue;
            }
            bool resource_exceeded{};
            if (!scalar(bytes, cursor, node,
                        owner_.resource().definitions[frame.node].decoded_utf8_bytes,
                        resource_exceeded))
                return reject(resource_exceeded ? ActivePipelineFailureKind::registry_cardinality
                                                : ActivePipelineFailureKind::noncanonical);
            if (plan != nullptr) {
                auto& event = plan->slots_[frame.event];
                event.lexical_size = cursor - frame.lexical_begin;
            }
            --depth;
            continue;
        }
        if (node.kind == SchemaKind::option) {
            if (frame.phase == 0U) {
                if (starts(bytes, cursor, "null")) {
                    cursor += 4U;
                    if (plan != nullptr) {
                        auto& event = plan->slots_[frame.event];
                        event.present = false;
                        event.lexical_size = cursor - frame.lexical_begin;
                    }
                    --depth;
                    continue;
                }
                frame.phase = 1U;
                if (!push(node.child))
                    return reject(ActivePipelineFailureKind::capacity_invariant, stack_.size(),
                                  depth + 1U);
            } else {
                // Publish presence only after the complete classified child validates.
                for (std::size_t channel{}; channel < wire_v1::detail::classified_option_definitions.size(); ++channel) {
                    if (frame.node != wire_v1::detail::classified_option_definitions[channel]) continue;
                    for (std::size_t parent = depth - 1U; parent != 0U; --parent) {
                        if (stack_[parent - 1U].node != wire_v1::detail::monitoring_result_definition) continue;
                        stack_[parent - 1U].monitoring_presence |= static_cast<std::uint8_t>(1U << channel);
                        break;
                    }
                }
                if (plan != nullptr)
                    plan->slots_[frame.event].lexical_size = cursor - frame.lexical_begin;
                --depth;
            }
            continue;
        }
        if (node.kind == SchemaKind::sum) {
            if (frame.phase == 0U) {
                const auto branch = sum_branch(bytes, cursor, node);
                if (!branch)
                    return reject(ActivePipelineFailureKind::registry_domain);
                if (plan != nullptr)
                    plan->slots_[frame.event].selected_child = *branch;
                frame.phase = 1U;
                if (!push(*branch))
                    return reject(ActivePipelineFailureKind::capacity_invariant, stack_.size(),
                                  depth + 1U);
            } else {
                if (plan != nullptr)
                    plan->slots_[frame.event].lexical_size = cursor - frame.lexical_begin;
                --depth;
            }
            continue;
        }
        if (node.kind == SchemaKind::object) {
            if (frame.phase == 0U) {
                if (cursor == bytes.size() || bytes[cursor++] != '{')
                    return reject(ActivePipelineFailureKind::registry_shape);
                frame.phase = 1U;
            }
            if (frame.index == node.edge_count) {
                if (cursor == bytes.size() || bytes[cursor++] != '}')
                    return reject(ActivePipelineFailureKind::registry_shape);
                if (plan != nullptr)
                    plan->slots_[frame.event].lexical_size = cursor - frame.lexical_begin;
                --depth;
                continue;
            }
            if (frame.index != 0U && (cursor == bytes.size() || bytes[cursor++] != ','))
                return reject(ActivePipelineFailureKind::registry_shape);
            const auto& edge = ordered_edge(node, frame.index++);
            if (cursor == bytes.size() || bytes[cursor++] != '"' ||
                !starts(bytes, cursor, edge.key))
                return reject(ActivePipelineFailureKind::registry_shape);
            cursor += edge.key.size();
            if (cursor + 1U >= bytes.size() || bytes[cursor++] != '"' || bytes[cursor++] != ':')
                return reject(ActivePipelineFailureKind::registry_shape);
            const auto side = node.special == wire_v1::detail::SchemaSpecial::finalization
                ? static_cast<std::uint8_t>(edge.key == "first" ? 1U : 2U) : std::uint8_t{0U};
            if (!push(edge.child, side))
                return reject(ActivePipelineFailureKind::capacity_invariant, stack_.size(),
                              depth + 1U);
            continue;
        }
        if (frame.phase == 0U) {
            if (cursor == bytes.size() || bytes[cursor++] != '[')
                return reject(ActivePipelineFailureKind::registry_shape);
            frame.phase = 1U;
            if (cursor < bytes.size() && bytes[cursor] == ']') {
                if (exact(frame)) {
                    const auto maximum = cardinality(frame);
                    if (!maximum || frame.count != *maximum)
                        return reject(ActivePipelineFailureKind::registry_cardinality,
                                      maximum.value_or(0U), frame.count);
                }
                ++cursor;
                if (plan != nullptr) {
                    auto& event = plan->slots_[frame.event];
                    event.element_count = 0U;
                    event.lexical_size = cursor - frame.lexical_begin;
                }
                --depth;
                continue;
            }
        } else if (frame.phase == 2U) {
            ++frame.count;
            const auto maximum = cardinality(frame);
            if (!maximum || frame.count > *maximum)
                return reject(ActivePipelineFailureKind::registry_cardinality, maximum.value_or(0U),
                              frame.count);
            if (cursor < bytes.size() && bytes[cursor] == ']') {
                if (exact(frame) && frame.count != *maximum)
                    return reject(ActivePipelineFailureKind::registry_cardinality, *maximum,
                                  frame.count);
                ++cursor;
                if (plan != nullptr) {
                    auto& event = plan->slots_[frame.event];
                    event.element_count = frame.count;
                    event.lexical_size = cursor - frame.lexical_begin;
                }
                --depth;
                continue;
            }
            if (cursor == bytes.size() || bytes[cursor++] != ',')
                return reject(ActivePipelineFailureKind::registry_shape);
        }
        frame.phase = 2U;
        if (!push(node.child))
            return reject(ActivePipelineFailureKind::capacity_invariant, stack_.size(), depth + 1U);
    }
    if (cursor != bytes.size())
        return reject(ActivePipelineFailureKind::noncanonical, bytes.size(), cursor);
    if (plan != nullptr) {
        decoded_ = true;
        return {};
    }
    if (!owner_.consume())
        return reject(ActivePipelineFailureKind::transport_state);
    active_ = false;
    consumed_ = true;
    return {};
}

auto PreparedCanonicalCursor::validate(std::span<const char> bytes) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    return walk(bytes, nullptr);
}

auto PreparedCanonicalCursor::decode(std::span<const char> bytes,
                                     PreparedTypedBuilderPlan& plan) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (!plan.active_ || plan.finished_ || plan.failed_ || decoded_)
        return std::unexpected{
            failure(ActivePipelineFailureKind::transport_state, ActivePipelineStage::prepared)};
    auto result = walk(bytes, &plan);
    if (!result)
        static_cast<void>(plan.fail(result.error()));
    return result;
}

auto PreparedCanonicalCursor::complete(const PreparedTypedBuilderPlan& plan) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (!active_ || consumed_ || !decoded_ || !plan.finished_ || plan.failed_ || !owner_.consume())
        return std::unexpected{
            failure(ActivePipelineFailureKind::transport_state, ActivePipelineStage::active)};
    active_ = false;
    consumed_ = true;
    return {};
}

namespace {
auto parsed_failure(ActivePipelineFailureKind kind, std::uint32_t definition = 0U) noexcept
    -> std::unexpected<ActiveCodecFailure> {
    auto value = failure(kind, ActivePipelineStage::active);
    value.sequence_index = definition;
    return std::unexpected{value};
}

auto parsed_node(std::span<const PreparedParsedNode> nodes, std::uint32_t definition,
                 std::uint64_t occurrence = 0U) noexcept -> const PreparedParsedNode* {
    for (const auto& node : nodes)
        if (node.definition == definition && node.occurrence == occurrence)
            return &node;
    return nullptr;
}

auto lexical(std::span<const char> body, const PreparedParsedNode& node) noexcept
    -> std::optional<std::string_view> {
    if (node.lexical_begin > body.size() || node.lexical_size > body.size() - node.lexical_begin)
        return std::nullopt;
    return std::string_view{body.data() + node.lexical_begin, node.lexical_size};
}

auto quoted_payload(std::string_view value) noexcept -> std::optional<std::string_view> {
    if (value.size() < 2U || value.front() != '"' || value.back() != '"')
        return std::nullopt;
    return value.substr(1U, value.size() - 2U);
}

auto parsed_unsigned(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                     std::uint32_t definition, std::uint64_t occurrence = 0U) noexcept
    -> std::optional<std::uint64_t> {
    const auto* node = parsed_node(nodes, definition, occurrence);
    if (node == nullptr)
        return std::nullopt;
    const auto view = lexical(body, *node);
    if (!view)
        return std::nullopt;
    const auto payload = quoted_payload(*view);
    if (!payload)
        return std::nullopt;
    std::uint64_t result{};
    const auto converted =
        std::from_chars(payload->data(), payload->data() + payload->size(), result);
    if (converted.ec != std::errc{} || converted.ptr != payload->data() + payload->size())
        return std::nullopt;
    return result;
}

auto parsed_binary64(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                     std::uint32_t definition, std::uint64_t occurrence = 0U) noexcept
    -> std::optional<double> {
    const auto* node = parsed_node(nodes, definition, occurrence);
    if (node == nullptr)
        return std::nullopt;
    const auto view = lexical(body, *node);
    if (!view)
        return std::nullopt;
    const auto payload = quoted_payload(*view);
    if (!payload || payload->size() != 16U)
        return std::nullopt;
    std::uint64_t bits{};
    for (const char character : *payload) {
        const auto digit = character >= '0' && character <= '9'
                               ? static_cast<std::uint64_t>(character - '0')
                           : character >= 'a' && character <= 'f'
                               ? static_cast<std::uint64_t>(character - 'a' + 10)
                               : 16U;
        if (digit == 16U)
            return std::nullopt;
        bits = (bits << 4U) | digit;
    }
    const auto result = std::bit_cast<double>(bits);
    if (!std::isfinite(result))
        return std::nullopt;
    return result;
}

auto parsed_token(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                  std::uint32_t definition) noexcept -> std::optional<std::string_view> {
    const auto* node = parsed_node(nodes, definition);
    if (node == nullptr)
        return std::nullopt;
    const auto view = lexical(body, *node);
    return view ? quoted_payload(*view) : std::nullopt;
}

auto parsed_bool(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                 std::uint32_t definition) noexcept -> std::optional<bool> {
    const auto* node = parsed_node(nodes, definition);
    if (node == nullptr)
        return std::nullopt;
    const auto view = lexical(body, *node);
    if (view == std::optional<std::string_view>{"true"})
        return true;
    if (view == std::optional<std::string_view>{"false"})
        return false;
    return std::nullopt;
}

auto decode_string(std::string_view lexical_value, std::size_t exact_limit,
                   std::string& destination) noexcept -> bool {
    const auto payload = quoted_payload(lexical_value);
    if (!payload)
        return false;
    std::size_t decoded_size{};
    for (std::size_t index{}; index < payload->size();) {
        const char value = (*payload)[index++];
        if (value == '\\') {
            if (index == payload->size())
                return false;
            const char escaped = (*payload)[index++];
            if (escaped == 'u') {
                if (index + 4U > payload->size() || (*payload)[index] != '0' ||
                    (*payload)[index + 1U] != '0')
                    return false;
                const auto hex = [](char character) noexcept {
                    return (character >= '0' && character <= '9') ||
                           (character >= 'a' && character <= 'f');
                };
                if (!hex((*payload)[index + 2U]) || !hex((*payload)[index + 3U]))
                    return false;
                index += 4U;
            } else if (escaped != '"' && escaped != '\\' && escaped != 'b' && escaped != 't' &&
                       escaped != 'n' && escaped != 'f' && escaped != 'r')
                return false;
        }
        if (decoded_size == exact_limit)
            return false;
        ++decoded_size;
    }
    if (decoded_size > destination.capacity())
        return false;
    destination.clear();
    for (std::size_t index{}; index < payload->size();) {
        const char value = (*payload)[index++];
        if (value != '\\') {
            destination.push_back(value);
            continue;
        }
        if (index == payload->size())
            return false;
        const char escaped = (*payload)[index++];
        char decoded{};
        switch (escaped) {
        case '"':
            decoded = '"';
            break;
        case '\\':
            decoded = '\\';
            break;
        case 'b':
            decoded = '\b';
            break;
        case 't':
            decoded = '\t';
            break;
        case 'n':
            decoded = '\n';
            break;
        case 'f':
            decoded = '\f';
            break;
        case 'r':
            decoded = '\r';
            break;
        case 'u': {
            if (index + 4U > payload->size() || (*payload)[index] != '0' ||
                (*payload)[index + 1U] != '0')
                return false;
            const auto hex = [](char character) noexcept -> int {
                if (character >= '0' && character <= '9')
                    return character - '0';
                if (character >= 'a' && character <= 'f')
                    return character - 'a' + 10;
                return -1;
            };
            const int high = hex((*payload)[index + 2U]);
            const int low = hex((*payload)[index + 3U]);
            if (high < 0 || low < 0)
                return false;
            decoded = static_cast<char>(high * 16 + low);
            index += 4U;
            break;
        }
        default:
            return false;
        }
        destination.push_back(decoded);
    }
    return true;
}

auto definition_index(std::string_view id) noexcept -> std::optional<std::uint32_t> {
    for (std::uint32_t index{}; index < prepared_resources_v1::detail::definition_stocks.size();
         ++index)
        if (prepared_resources_v1::detail::definition_stocks[index].definition_id == id)
            return index;
    return std::nullopt;
}

auto parsed_node_inside(std::span<const PreparedParsedNode> nodes, std::uint32_t definition,
                        const PreparedParsedNode& parent, std::uint64_t ordinal = 0U) noexcept
    -> const PreparedParsedNode* {
    std::uint64_t found{};
    const auto end = parent.lexical_begin + parent.lexical_size;
    for (const auto& node : nodes) {
        if (node.definition != definition || node.lexical_begin < parent.lexical_begin ||
            node.lexical_begin >= end)
            continue;
        if (found++ == ordinal)
            return &node;
    }
    return nullptr;
}

auto unsigned_inside(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                     std::uint32_t definition, const PreparedParsedNode& parent,
                     std::uint64_t ordinal = 0U) noexcept -> std::optional<std::uint64_t> {
    const auto* node = parsed_node_inside(nodes, definition, parent, ordinal);
    if (node == nullptr)
        return std::nullopt;
    const auto view = lexical(body, *node);
    const auto payload = view ? quoted_payload(*view) : std::nullopt;
    if (!payload)
        return std::nullopt;
    std::uint64_t value{};
    const auto result = std::from_chars(payload->data(), payload->data() + payload->size(), value);
    return result.ec == std::errc{} && result.ptr == payload->data() + payload->size()
               ? std::optional{value}
               : std::nullopt;
}
struct HeaderDefinitions final {
    static constexpr auto experiment = required_definition("scalar__provenance__experiment_id");
    static constexpr std::array strings{
        required_definition("scalar__provenance__scenario_profile"),
        required_definition("scalar__provenance__scenario_digest"),
        required_definition("scalar__provenance__resolved_plan_digest"),
        required_definition("scalar__provenance__workload_id"),
        required_definition("scalar__provenance__first_variant_id"),
        required_definition("scalar__provenance__second_variant_id")};
    static constexpr auto bootstrap_count =
        required_definition("scalar__bootstrap_policy__replicate_count");
    static constexpr auto bootstrap_confidence =
        required_definition("scalar__bootstrap_policy__confidence");
    static constexpr auto bootstrap_seed = required_definition("scalar__bootstrap_policy__seed");
    static constexpr auto fraction =
        required_definition("scalar__statistics_policy__effective_practical_effect_fraction");
    static constexpr auto delta =
        required_definition("scalar__statistics_policy__practical_effect_delta");
    static constexpr auto target =
        required_definition("scalar__statistics_policy__relative_half_width_target");
    static constexpr auto page = required_definition("scalar__monitoring_policy__page_faults");
    static constexpr auto context =
        required_definition("scalar__monitoring_policy__context_switches");
    static constexpr auto temperature =
        required_definition("scalar__monitoring_policy__temperature");
    static constexpr auto frequency = required_definition("scalar__monitoring_policy__frequency");
    static constexpr auto before =
        required_definition("scalar__monitoring_policy__before_observation");
    static constexpr auto after =
        required_definition("scalar__monitoring_policy__after_observation");
    static constexpr auto during =
        required_definition("scalar__monitoring_policy__during_observation");
    static constexpr auto interval =
        required_definition("option__monitoring_policy__during_interval");
    static constexpr auto duration = required_definition("duration");
    static constexpr auto unsupported =
        required_definition("scalar__monitoring_policy__optional_unsupported");
    static constexpr auto monitoring = required_definition("monitoring_policy");
    static constexpr auto minimum =
        required_definition("scalar__repetition_policy__minimum_blocks");
    static constexpr auto maximum =
        required_definition("scalar__repetition_policy__maximum_blocks");
    static constexpr auto minimum_valid =
        required_definition("scalar__repetition_policy__minimum_valid_observations_per_variant");
    static constexpr auto stopping_enabled =
        required_definition("scalar__repetition_policy__stopping_enabled");
    static constexpr auto repetition = required_definition("repetition_policy");
    static constexpr auto order_strategy =
        required_definition("scalar__comparison_order__strategy");
    static constexpr auto order_seed = required_definition("scalar__comparison_order__seed");
    static constexpr auto order = required_definition("comparison_order");
};
} // namespace

auto PreparedEvidenceHeaderBuilder::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedEvidenceHeaderBuilder, prepared_resources_v1::PreparedResourceError> {
    try {
        constexpr std::array ids{std::string_view{"scalar__provenance__scenario_profile"},
                                 std::string_view{"scalar__provenance__scenario_digest"},
                                 std::string_view{"scalar__provenance__resolved_plan_digest"},
                                 std::string_view{"scalar__provenance__workload_id"},
                                 std::string_view{"scalar__provenance__first_variant_id"},
                                 std::string_view{"scalar__provenance__second_variant_id"}};
        if (resource.definitions.size() != wire_v1::detail::schema_nodes.size())
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        std::array<std::string, 6> strings;
        std::array<std::size_t, 6> limits{};
        for (std::size_t slot{}; slot < ids.size(); ++slot) {
            const auto index = definition_index(ids[slot]);
            if (!index || resource.definitions[*index].definition_id != ids[slot] ||
                resource.definitions[*index].owner_occurrences != 1U ||
                !resource.definitions[*index].decoded_utf8_bytes ||
                *resource.definitions[*index].decoded_utf8_bytes > strings[slot].max_size())
                return std::unexpected{prepared_resources_v1::PreparedResourceError{
                    prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch,
                    index.value_or(0U)}};
            strings[slot].reserve(
                static_cast<std::size_t>(*resource.definitions[*index].decoded_utf8_bytes));
            limits[slot] =
                static_cast<std::size_t>(*resource.definitions[*index].decoded_utf8_bytes);
        }
        return PreparedEvidenceHeaderBuilder{std::move(strings), limits};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedEvidenceHeaderBuilder::build(std::span<const char> body,
                                          std::span<const PreparedParsedNode> nodes) noexcept
    -> std::expected<wire_v1::detail::ReconstructedEvidenceHeader, ActiveCodecFailure> {
    if (consumed_)
        return parsed_failure(ActivePipelineFailureKind::transport_state);
    consumed_ = true;
    for (std::size_t index{}; index < strings_.size(); ++index) {
        const auto definition = HeaderDefinitions::strings[index];
        const auto* node = parsed_node(nodes, definition);
        const auto view =
            node == nullptr ? std::optional<std::string_view>{} : lexical(body, *node);
        if (!view || !decode_string(*view, limits_[index], strings_[index]))
            return parsed_failure(ActivePipelineFailureKind::registry_domain, definition);
    }
    const auto experiment = parsed_unsigned(body, nodes, HeaderDefinitions::experiment);
    const auto comparison_count = parsed_unsigned(body, nodes, HeaderDefinitions::bootstrap_count);
    const auto comparison_confidence =
        parsed_binary64(body, nodes, HeaderDefinitions::bootstrap_confidence);
    const auto comparison_seed = parsed_unsigned(body, nodes, HeaderDefinitions::bootstrap_seed);
    const auto stopping_count =
        parsed_unsigned(body, nodes, HeaderDefinitions::bootstrap_count, 1U);
    const auto stopping_confidence =
        parsed_binary64(body, nodes, HeaderDefinitions::bootstrap_confidence, 1U);
    const auto stopping_seed = parsed_unsigned(body, nodes, HeaderDefinitions::bootstrap_seed, 1U);
    const auto fraction = parsed_binary64(body, nodes, HeaderDefinitions::fraction);
    const auto stated_delta = parsed_binary64(body, nodes, HeaderDefinitions::delta);
    const auto target = parsed_binary64(body, nodes, HeaderDefinitions::target);
    if (!experiment || !comparison_count || !comparison_confidence || !comparison_seed ||
        !stopping_count || !stopping_confidence || !stopping_seed || !fraction || !stated_delta ||
        !target)
        return parsed_failure(ActivePipelineFailureKind::registry_domain);
    const auto valid_bootstrap = [](std::uint64_t count, double confidence) noexcept {
        return count != 0U && count <= std::numeric_limits<std::size_t>::max() &&
               confidence > 0.0 && confidence < 1.0;
    };
    if (!valid_bootstrap(*comparison_count, *comparison_confidence) ||
        !valid_bootstrap(*stopping_count, *stopping_confidence) || *fraction < 0.0 ||
        *fraction > 1.0 || *target < 0.0 || *target > 1.0)
        return parsed_failure(ActivePipelineFailureKind::typed_factory);
    const double delta = std::log1p(*fraction);
    if (!std::isfinite(delta) || std::bit_cast<std::uint64_t>(delta == 0.0 ? 0.0 : delta) !=
                                     std::bit_cast<std::uint64_t>(*stated_delta))
        return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                              HeaderDefinitions::delta);
    const auto comparison = statistics::detail::BootstrapAccess::policy(
        statistics::detail::BootstrapAccess::count(*comparison_count),
        statistics::detail::BootstrapAccess::confidence(*comparison_confidence),
        core::detail::SeedDerivationAccess::seed(*comparison_seed));
    const auto stopping = statistics::detail::BootstrapAccess::policy(
        statistics::detail::BootstrapAccess::count(*stopping_count),
        statistics::detail::BootstrapAccess::confidence(*stopping_confidence),
        core::detail::SeedDerivationAccess::seed(*stopping_seed));
    auto statistics_policy = runtime::detail::PreparedEvidenceHeaderAccess::policy(
        comparison, stopping, *fraction,
        statistics::detail::PreparedEvidenceHeaderAccess::threshold(delta == 0.0 ? 0.0 : delta),
        statistics::detail::AdaptiveStoppingAccess::target(*target == 0.0 ? 0.0 : *target));

    const auto channel =
        [&](std::uint32_t definition) -> std::optional<monitoring::MonitoringChannelDisposition> {
        const auto token = parsed_token(body, nodes, definition);
        if (token == "disabled")
            return monitoring::MonitoringChannelDisposition::disabled;
        if (token == "optional")
            return monitoring::MonitoringChannelDisposition::optional;
        if (token == "required")
            return monitoring::MonitoringChannelDisposition::required;
        return std::nullopt;
    };
    const auto page = channel(HeaderDefinitions::page);
    const auto context = channel(HeaderDefinitions::context);
    const auto temperature = channel(HeaderDefinitions::temperature);
    const auto frequency = channel(HeaderDefinitions::frequency);
    const auto before = parsed_bool(body, nodes, HeaderDefinitions::before);
    const auto after = parsed_bool(body, nodes, HeaderDefinitions::after);
    const auto during = parsed_bool(body, nodes, HeaderDefinitions::during);
    const auto optional_token = parsed_token(body, nodes, HeaderDefinitions::unsupported);
    const auto* interval_node = parsed_node(nodes, HeaderDefinitions::interval);
    if (!page || !context || !temperature || !frequency || !before || !after || !during ||
        !optional_token || interval_node == nullptr)
        return parsed_failure(ActivePipelineFailureKind::registry_domain);
    std::optional<core::Duration> interval;
    if (interval_node->present) {
        const auto amount = unsigned_inside(body, nodes, HeaderDefinitions::duration, *interval_node);
        if (!amount || *amount == 0U)
            return parsed_failure(ActivePipelineFailureKind::typed_factory,
                                  HeaderDefinitions::interval);
        interval = core::Duration::from_base_units(*amount);
    }
    std::optional<monitoring::OptionalUnavailableSourcePolicy> unsupported;
    if (*optional_token == "fail")
        unsupported = monitoring::OptionalUnavailableSourcePolicy::fail;
    else if (*optional_token == "warn")
        unsupported = monitoring::OptionalUnavailableSourcePolicy::warn;
    else if (*optional_token == "disable")
        unsupported = monitoring::OptionalUnavailableSourcePolicy::disable;
    if (!unsupported || *during != interval.has_value())
        return parsed_failure(ActivePipelineFailureKind::typed_factory,
                              HeaderDefinitions::monitoring);
    const auto enabled = [](auto value) noexcept {
        return value != monitoring::MonitoringChannelDisposition::disabled;
    };
    if ((enabled(*page) && (!*before || !*after)) ||
        ((enabled(*temperature) || enabled(*frequency)) && !*during))
        return parsed_failure(ActivePipelineFailureKind::typed_factory,
                              HeaderDefinitions::monitoring);
    auto monitoring_policy = monitoring::detail::PreparedEvidenceHeaderAccess::policy(
        {*page, *context, *temperature, *frequency}, *before, *after, *during, interval,
        *unsupported);

    const auto minimum = parsed_unsigned(body, nodes, HeaderDefinitions::minimum);
    const auto maximum = parsed_unsigned(body, nodes, HeaderDefinitions::maximum);
    const auto minimum_valid = parsed_unsigned(body, nodes, HeaderDefinitions::minimum_valid);
    const auto stopping_enabled = parsed_bool(body, nodes, HeaderDefinitions::stopping_enabled);
    constexpr auto observations_per_block = std::uint64_t{4U};
    constexpr auto largest_safe =
        std::numeric_limits<std::uint64_t>::max() / observations_per_block + 1U;
    if (!minimum || !maximum || !minimum_valid || !stopping_enabled || *minimum > *maximum ||
        *maximum > largest_safe)
        return parsed_failure(ActivePipelineFailureKind::typed_factory,
                              HeaderDefinitions::repetition);
    auto repetition_policy = measurement::detail::MeasurementLeafAccess::repetition_policy(
        *minimum, *maximum, *minimum_valid, *stopping_enabled);
    const auto order_token = parsed_token(body, nodes, HeaderDefinitions::order_strategy);
    const auto order_seed = parsed_unsigned(body, nodes, HeaderDefinitions::order_seed);
    std::optional<measurement::ComparisonOrderStrategy> strategy;
    if (order_token == "randomized_abba")
        strategy = measurement::ComparisonOrderStrategy::randomized_abba;
    else if (order_token == "randomized_baab")
        strategy = measurement::ComparisonOrderStrategy::randomized_baab;
    else if (order_token == "randomized_balanced")
        strategy = measurement::ComparisonOrderStrategy::randomized_balanced;
    if (!strategy || !order_seed)
        return parsed_failure(ActivePipelineFailureKind::registry_domain, HeaderDefinitions::order);

    return wire_v1::detail::ReconstructedEvidenceHeader{
        PairwiseRunProvenance{core::ExperimentId{*experiment}, std::move(strings_[0]),
                              std::move(strings_[1]), std::move(strings_[2]),
                              std::move(strings_[3]), std::move(strings_[4]),
                              std::move(strings_[5])},
        std::move(statistics_policy), std::move(monitoring_policy), std::move(repetition_policy),
        measurement::ComparisonOrderPolicy::create(*strategy, *order_seed)};
}

namespace {
struct CalibrationDefinitions final {
    static constexpr auto calibration = required_definition("calibration");
    static constexpr auto warmup = required_definition("warmup");
    static constexpr auto calibration_kind = required_definition("scalar__calibration__kind");
    static constexpr auto warmup_kind = required_definition("scalar__warmup__kind");
    static constexpr auto minimum_duration =
        required_definition("scalar__duration_policy__minimum_duration");
    static constexpr auto target_duration =
        required_definition("scalar__duration_policy__target_duration");
    static constexpr auto maximum_duration =
        required_definition("scalar__duration_policy__maximum_duration");
    static constexpr auto empty_array =
        required_definition("array__calibration__empty_harness_trials");
    static constexpr auto candidate_array =
        required_definition("array__calibration__candidate_trials");
    static constexpr auto warmup_array = required_definition("array__warmup__trials");
    static constexpr auto interval = required_definition("interval");
    static constexpr auto start = required_definition("scalar__interval__start");
    static constexpr auto end = required_definition("scalar__interval__end");
    static constexpr auto elapsed = required_definition("scalar__interval__elapsed_nanoseconds");
    static constexpr auto candidate_trial = required_definition("candidate_trial");
    static constexpr auto candidate = required_definition("scalar__candidate_trial__candidate");
    static constexpr auto duration = required_definition("duration");
    static constexpr auto calibration_disposition =
        required_definition("scalar__calibration__disposition");
    static constexpr auto selected = required_definition("option__calibration__selected_candidate");
    static constexpr auto work_count = required_definition("work_count");
    static constexpr auto warmup_minimum =
        required_definition("scalar__warmup_policy__minimum_trials");
    static constexpr auto warmup_maximum =
        required_definition("scalar__warmup_policy__maximum_trials");
    static constexpr auto warmup_window =
        required_definition("scalar__warmup_policy__stability_window");
    static constexpr auto u64 = required_definition("u64");
    static constexpr auto selected_work = required_definition("scalar__warmup__selected_work");
    static constexpr auto warmup_disposition = required_definition("scalar__warmup__disposition");
    static constexpr auto final_evaluation =
        required_definition("option__warmup__final_stability_evaluation");
    static constexpr auto stable = required_definition("scalar__stability_evaluation__stable");
};

auto stock_cardinality(const prepared_resources_v1::PreparedResourceValue& resource,
                       std::uint32_t definition) noexcept -> std::optional<std::size_t> {
    if (definition >= resource.definitions.size())
        return std::nullopt;
    const auto amount = resource.definitions[definition].local_array_cardinality;
    if (!amount || *amount > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    return static_cast<std::size_t>(*amount);
}

auto interval_inside(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                     const PreparedParsedNode& parent, std::uint64_t ordinal) noexcept
    -> std::optional<measurement::MeasurementInterval> {
    const auto* value =
        parsed_node_inside(nodes, CalibrationDefinitions::interval, parent, ordinal);
    if (value == nullptr)
        return std::nullopt;
    const auto start = unsigned_inside(body, nodes, CalibrationDefinitions::start, *value);
    const auto end = unsigned_inside(body, nodes, CalibrationDefinitions::end, *value);
    const auto elapsed = unsigned_inside(body, nodes, CalibrationDefinitions::elapsed, *value);
    if (!start || !end || !elapsed || *end < *start)
        return std::nullopt;
    auto result = measurement::MeasurementInterval::create(
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(*start),
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(*end));
    if (!result || result->elapsed().value() != *elapsed)
        return std::nullopt;
    return *result;
}

auto rational_le(std::uint64_t ln, std::uint64_t ld, std::uint64_t rn, std::uint64_t rd) noexcept
    -> bool {
    bool reversed{};
    for (;;) {
        const auto lq = ln / ld, rq = rn / rd;
        if (lq != rq)
            return reversed ? lq > rq : lq < rq;
        const auto lr = ln % ld, rr = rn % rd;
        if (lr == 0U || rr == 0U)
            return lr == 0U && rr == 0U ? true : (reversed ? rr == 0U : lr == 0U);
        ln = ld;
        ld = lr;
        rn = rd;
        rd = rr;
        reversed = !reversed;
    }
}
} // namespace

auto PreparedCalibrationWarmupBuilder::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedCalibrationWarmupBuilder,
                     prepared_resources_v1::PreparedResourceError> {
    try {
        const auto empty = stock_cardinality(resource, CalibrationDefinitions::empty_array);
        const auto candidates =
            stock_cardinality(resource, CalibrationDefinitions::candidate_array);
        const auto warmup = stock_cardinality(resource, CalibrationDefinitions::warmup_array);
        if (!empty || !candidates || !warmup)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::invalid_input}};
        std::vector<measurement::EmptyHarnessTrial> empty_values;
        empty_values.reserve(*empty);
        std::vector<measurement::SequentialReadBandwidthCalibrationTrial> bandwidth;
        bandwidth.reserve(*candidates);
        std::vector<measurement::DependentLoadLatencyCalibrationTrial> latency;
        latency.reserve(*candidates);
        std::vector<measurement::WarmupTrial> warmup_values;
        warmup_values.reserve(*warmup);
        std::vector<std::uint64_t> scratch;
        scratch.reserve(*warmup);
        return PreparedCalibrationWarmupBuilder{std::move(empty_values), std::move(bandwidth),
                                                std::move(latency), std::move(warmup_values),
                                                std::move(scratch)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedCalibrationWarmupBuilder::build(std::span<const char> body,
                                             std::span<const PreparedParsedNode> nodes) noexcept
    -> std::expected<wire_v1::detail::ReconstructedCalibrationWarmup, ActiveCodecFailure> {
    if (consumed_)
        return parsed_failure(ActivePipelineFailureKind::transport_state);
    consumed_ = true;
    wire_v1::detail::ReconstructedCalibrationWarmup result;
    const auto* calibration_node = parsed_node(nodes, CalibrationDefinitions::calibration);
    const auto* warmup_node = parsed_node(nodes, CalibrationDefinitions::warmup);
    if (calibration_node == nullptr || warmup_node == nullptr)
        return parsed_failure(ActivePipelineFailureKind::registry_shape);

    if (calibration_node->present) {
        const auto kind =
            parsed_node_inside(nodes, CalibrationDefinitions::calibration_kind, *calibration_node);
        const auto kind_view = kind ? lexical(body, *kind) : std::nullopt;
        const auto kind_token = kind_view ? quoted_payload(*kind_view) : std::nullopt;
        const bool bandwidth_kind = kind_token == std::optional<std::string_view>{"bandwidth"};
        if (!bandwidth_kind && kind_token != std::optional<std::string_view>{"latency"})
            return parsed_failure(ActivePipelineFailureKind::registry_domain,
                                  CalibrationDefinitions::calibration_kind);
        const auto minimum = unsigned_inside(body, nodes, CalibrationDefinitions::minimum_duration,
                                             *calibration_node);
        const auto target = unsigned_inside(body, nodes, CalibrationDefinitions::target_duration,
                                            *calibration_node);
        const auto maximum = unsigned_inside(body, nodes, CalibrationDefinitions::maximum_duration,
                                             *calibration_node);
        if (!minimum || !target || !maximum)
            return parsed_failure(ActivePipelineFailureKind::registry_shape,
                                  CalibrationDefinitions::calibration);
        if (*minimum > *target || *target > *maximum)
            return parsed_failure(ActivePipelineFailureKind::typed_factory,
                                  CalibrationDefinitions::calibration);
        auto policy = measurement::CalibrationDurationPolicy::create(
            core::Duration::from_base_units(*minimum), core::Duration::from_base_units(*target),
            core::Duration::from_base_units(*maximum));
        if (!policy)
            return parsed_failure(ActivePipelineFailureKind::typed_factory,
                                  CalibrationDefinitions::calibration);
        const auto* empty_array =
            parsed_node_inside(nodes, CalibrationDefinitions::empty_array, *calibration_node);
        const auto* candidate_array =
            parsed_node_inside(nodes, CalibrationDefinitions::candidate_array, *calibration_node);
        if (empty_array == nullptr || candidate_array == nullptr ||
            empty_array->element_count != 5U || candidate_array->element_count == 0U ||
            candidate_array->element_count > 128U ||
            empty_array->element_count > empty_.capacity() ||
            candidate_array->element_count >
                (bandwidth_kind ? bandwidth_.capacity() : latency_.capacity()))
            return parsed_failure(ActivePipelineFailureKind::capacity_invariant,
                                  CalibrationDefinitions::candidate_array);
        std::uint64_t overhead = std::numeric_limits<std::uint64_t>::max();
        for (std::uint64_t i{}; i < empty_array->element_count; ++i) {
            auto measured = interval_inside(body, nodes, *empty_array, i);
            if (!measured)
                return parsed_failure(ActivePipelineFailureKind::typed_factory,
                                      CalibrationDefinitions::interval);
            overhead = std::min(overhead, measured->elapsed().value());
            empty_.push_back(measurement::EmptyHarnessTrial::create(std::move(*measured)));
        }
        const auto stated_overhead =
            unsigned_inside(body, nodes, CalibrationDefinitions::duration, *calibration_node);
        if (!stated_overhead || *stated_overhead != overhead)
            return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                  CalibrationDefinitions::duration);
        std::optional<std::uint64_t> lower, upper, derived_selected;
        std::uint64_t expected = 1U, best_distance{};
        for (std::uint64_t i{}; i < candidate_array->element_count; ++i) {
            const auto* trial = parsed_node_inside(nodes, CalibrationDefinitions::candidate_trial,
                                                   *candidate_array, i);
            if (!trial)
                return parsed_failure(ActivePipelineFailureKind::registry_shape,
                                      CalibrationDefinitions::candidate_trial);
            const auto raw =
                unsigned_inside(body, nodes, CalibrationDefinitions::candidate, *trial);
            auto measured = interval_inside(body, nodes, *trial, 0U);
            const std::uint64_t quantum = bandwidth_kind ? 1U : 4096U;
            const std::uint64_t max_units = std::numeric_limits<std::uint64_t>::max() / quantum;
            if (!raw || !measured || *raw == 0U || *raw % quantum != 0U ||
                *raw / quantum != expected)
                return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                      CalibrationDefinitions::candidate_trial);
            const auto elapsed = measured->elapsed();
            if (bandwidth_kind) {
                auto count = kernel::SequentialReadBandwidthPassCount::create(*raw);
                if (!count)
                    return parsed_failure(ActivePipelineFailureKind::typed_factory);
                bandwidth_.push_back(measurement::SequentialReadBandwidthCalibrationTrial::create(
                    *count, std::move(*measured)));
            } else {
                auto count = kernel::DependentLoadLatencyStepCount::create(*raw);
                if (!count)
                    return parsed_failure(ActivePipelineFailureKind::typed_factory);
                latency_.push_back(measurement::DependentLoadLatencyCalibrationTrial::create(
                    *count, std::move(*measured)));
            }
            if (elapsed >= policy->minimum_duration() && elapsed <= policy->maximum_duration()) {
                const auto distance = elapsed.value() < target.value_or(0U)
                                          ? *target - elapsed.value()
                                          : elapsed.value() - *target;
                if (!derived_selected || distance < best_distance ||
                    (distance == best_distance && *raw < *derived_selected)) {
                    derived_selected = *raw;
                    best_distance = distance;
                }
            }
            if (elapsed < policy->target_duration())
                lower = expected;
            else if (elapsed > policy->target_duration())
                upper = expected;
            if (i + 1U < candidate_array->element_count) {
                if (elapsed == policy->target_duration())
                    return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                          CalibrationDefinitions::candidate_array);
                if (elapsed < policy->target_duration() && expected < max_units && !upper)
                    expected = expected > max_units - expected ? max_units : expected * 2U;
                else if (lower && upper && *upper - *lower > 1U)
                    expected = *lower + ((*upper - *lower) / 2U);
                else
                    return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                          CalibrationDefinitions::candidate_array);
            }
        }
        auto disposition =
            derived_selected ? measurement::CalibrationDisposition::selected
                             : measurement::CalibrationDisposition::no_candidate_in_acceptable_band;
        const auto& first =
            bandwidth_kind ? bandwidth_.front().interval() : latency_.front().interval();
        const auto& last =
            bandwidth_kind ? bandwidth_.back().interval() : latency_.back().interval();
        const auto quantum = bandwidth_kind ? 1U : 4096U;
        const auto maximum_units = std::numeric_limits<std::uint64_t>::max() / quantum;
        if (last.elapsed() != policy->target_duration() &&
            !(lower && upper && *upper - *lower <= 1U) &&
            !(candidate_array->element_count == 1U && last.elapsed() > policy->target_duration()) &&
            !(last.elapsed() < policy->target_duration() && expected == maximum_units))
            return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                  CalibrationDefinitions::candidate_array);
        if (!derived_selected && first.elapsed() > policy->maximum_duration())
            disposition =
                measurement::CalibrationDisposition::minimum_candidate_above_maximum_duration;
        else if (!derived_selected && expected == maximum_units &&
                 last.elapsed() < policy->minimum_duration())
            disposition =
                measurement::CalibrationDisposition::maximum_candidate_below_minimum_duration;
        const auto disposition_node = parsed_node_inside(
            nodes, CalibrationDefinitions::calibration_disposition, *calibration_node);
        const auto disposition_view =
            disposition_node ? lexical(body, *disposition_node) : std::nullopt;
        const auto disposition_token =
            disposition_view ? quoted_payload(*disposition_view) : std::nullopt;
        const auto expected_token =
            disposition == measurement::CalibrationDisposition::selected ? "selected"
            : disposition ==
                    measurement::CalibrationDisposition::minimum_candidate_above_maximum_duration
                ? "minimum_candidate_above_maximum_duration"
            : disposition ==
                    measurement::CalibrationDisposition::maximum_candidate_below_minimum_duration
                ? "maximum_candidate_below_minimum_duration"
                : "no_candidate_in_acceptable_band";
        if (disposition_token != std::optional<std::string_view>{expected_token})
            return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                  CalibrationDefinitions::calibration_disposition);
        const auto* selected_node =
            parsed_node_inside(nodes, CalibrationDefinitions::selected, *calibration_node);
        std::optional<std::uint64_t> stated_selected;
        if (selected_node && selected_node->present)
            stated_selected =
                unsigned_inside(body, nodes, CalibrationDefinitions::work_count, *selected_node);
        if (stated_selected != derived_selected)
            return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                  CalibrationDefinitions::selected);
        if (bandwidth_kind) {
            std::optional<kernel::SequentialReadBandwidthPassCount> selected;
            if (derived_selected)
                selected.emplace(
                    *kernel::SequentialReadBandwidthPassCount::create(*derived_selected));
            result.calibration.emplace(measurement::detail::CalibrationAccess::make_outcome(
                std::move(*policy), std::move(empty_),
                measurement::TimerOverheadEstimate::create(
                    core::Duration::from_base_units(overhead)),
                std::move(bandwidth_), disposition, selected));
        } else {
            std::optional<kernel::DependentLoadLatencyStepCount> selected;
            if (derived_selected)
                selected.emplace(*kernel::DependentLoadLatencyStepCount::create(*derived_selected));
            result.calibration.emplace(measurement::detail::CalibrationAccess::make_outcome(
                std::move(*policy), std::move(empty_),
                measurement::TimerOverheadEstimate::create(
                    core::Duration::from_base_units(overhead)),
                std::move(latency_), disposition, selected));
        }
    }

    if (warmup_node->present) {
        const auto* kind_node =
            parsed_node_inside(nodes, CalibrationDefinitions::warmup_kind, *warmup_node);
        const auto kind_view = kind_node ? lexical(body, *kind_node) : std::nullopt;
        const auto token = kind_view ? quoted_payload(*kind_view) : std::nullopt;
        const bool bandwidth_kind = token == std::optional<std::string_view>{"bandwidth"};
        if (!bandwidth_kind && token != std::optional<std::string_view>{"latency"})
            return parsed_failure(ActivePipelineFailureKind::registry_domain,
                                  CalibrationDefinitions::warmup_kind);
        const auto minimum =
            unsigned_inside(body, nodes, CalibrationDefinitions::warmup_minimum, *warmup_node);
        const auto maximum =
            unsigned_inside(body, nodes, CalibrationDefinitions::warmup_maximum, *warmup_node);
        const auto window =
            unsigned_inside(body, nodes, CalibrationDefinitions::warmup_window, *warmup_node);
        const auto denominator =
            unsigned_inside(body, nodes, CalibrationDefinitions::u64, *warmup_node, 0U);
        const auto numerator =
            unsigned_inside(body, nodes, CalibrationDefinitions::u64, *warmup_node, 1U);
        const auto selected_raw =
            unsigned_inside(body, nodes, CalibrationDefinitions::selected_work, *warmup_node);
        if (!minimum || !maximum || !window || !numerator || !denominator || !selected_raw)
            return parsed_failure(ActivePipelineFailureKind::registry_shape,
                                  CalibrationDefinitions::warmup);
        const auto disabled_invalid = *maximum == 0U && *minimum != 0U;
        const auto enabled_invalid = *maximum > 0U && (*window == 0U || *window > *maximum);
        if (*denominator == 0U || *minimum > *maximum || disabled_invalid || enabled_invalid ||
            *selected_raw == 0U)
            return parsed_failure(ActivePipelineFailureKind::typed_factory,
                                  CalibrationDefinitions::warmup);
        auto threshold = measurement::WarmupStabilityThreshold::create(*numerator, *denominator);
        if (!threshold)
            return parsed_failure(ActivePipelineFailureKind::typed_factory,
                                  CalibrationDefinitions::warmup);
        auto policy = measurement::WarmupPolicy::create(*minimum, *maximum, *window, *threshold);
        if (!policy)
            return parsed_failure(ActivePipelineFailureKind::typed_factory,
                                  CalibrationDefinitions::warmup);
        const auto* trials =
            parsed_node_inside(nodes, CalibrationDefinitions::warmup_array, *warmup_node);
        if (!trials || trials->element_count > *maximum ||
            trials->element_count > warmup_.capacity() ||
            (*maximum > 0U && *window > scratch_.capacity()))
            return parsed_failure(ActivePipelineFailureKind::capacity_invariant,
                                  CalibrationDefinitions::warmup_array);
        for (std::uint64_t i{}; i < trials->element_count; ++i) {
            auto measured = interval_inside(body, nodes, *trials, i);
            if (!measured)
                return parsed_failure(ActivePipelineFailureKind::typed_factory,
                                      CalibrationDefinitions::interval);
            warmup_.push_back(measurement::WarmupTrial::create(std::move(*measured)));
        }
        std::optional<measurement::WarmupStabilityEvaluation> evaluation;
        const auto evaluate_prefix = [&](std::size_t count) noexcept {
            scratch_.clear();
            for (const auto& trial :
                 std::span<const measurement::WarmupTrial>{warmup_}.first(count).last(
                     static_cast<std::size_t>(*window)))
                scratch_.push_back(trial.interval().elapsed().value());
            std::sort(scratch_.begin(), scratch_.end());
            const auto median = scratch_[(scratch_.size() - 1U) / 2U];
            std::uint64_t deviation{};
            bool stable = true;
            for (const auto sample : scratch_) {
                const auto current = sample < median ? median - sample : sample - median;
                deviation = std::max(deviation, current);
                stable = stable &&
                         (median == 0U ? sample == 0U
                                       : rational_le(current, median, *numerator, *denominator));
            }
            return measurement::WarmupStabilityEvaluation::create(
                core::Duration::from_base_units(median), core::Duration::from_base_units(deviation),
                stable);
        };
        for (std::size_t count = 1U; count <= warmup_.size(); ++count) {
            if (count < *minimum || count < *window)
                continue;
            auto current = evaluate_prefix(count);
            if (current.stable() && count != warmup_.size())
                return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                      CalibrationDefinitions::warmup_array);
            if (count == warmup_.size())
                evaluation.emplace(std::move(current));
        }
        const auto disposition =
            *maximum == 0U ? measurement::WarmupDisposition::skipped
            : evaluation && evaluation->stable()
                ? measurement::WarmupDisposition::stabilized
                : measurement::WarmupDisposition::maximum_trials_reached_without_stability;
        const auto* disposition_node =
            parsed_node_inside(nodes, CalibrationDefinitions::warmup_disposition, *warmup_node);
        const auto disposition_view =
            disposition_node ? lexical(body, *disposition_node) : std::nullopt;
        const auto disposition_token =
            disposition_view ? quoted_payload(*disposition_view) : std::nullopt;
        const auto expected_token = disposition == measurement::WarmupDisposition::skipped
                                        ? "skipped"
                                    : disposition == measurement::WarmupDisposition::stabilized
                                        ? "stabilized"
                                        : "maximum_trials_reached_without_stability";
        if (disposition_token != std::optional<std::string_view>{expected_token} ||
            (disposition == measurement::WarmupDisposition::skipped && !warmup_.empty()) ||
            (disposition ==
                 measurement::WarmupDisposition::maximum_trials_reached_without_stability &&
             warmup_.size() != *maximum))
            return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                  CalibrationDefinitions::warmup_disposition);
        const auto* stated_eval =
            parsed_node_inside(nodes, CalibrationDefinitions::final_evaluation, *warmup_node);
        if (!stated_eval || stated_eval->present != evaluation.has_value())
            return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                  CalibrationDefinitions::final_evaluation);
        if (evaluation) {
            const auto deviation =
                unsigned_inside(body, nodes, CalibrationDefinitions::duration, *stated_eval, 0U);
            const auto median =
                unsigned_inside(body, nodes, CalibrationDefinitions::duration, *stated_eval, 1U);
            const auto* stable_node =
                parsed_node_inside(nodes, CalibrationDefinitions::stable, *stated_eval);
            const auto stable_view = stable_node ? lexical(body, *stable_node) : std::nullopt;
            if (!median)
                return parsed_failure(ActivePipelineFailureKind::registry_shape,
                                      CalibrationDefinitions::duration);
            if (!deviation)
                return parsed_failure(ActivePipelineFailureKind::registry_shape,
                                      CalibrationDefinitions::duration);
            if (!stable_view)
                return parsed_failure(ActivePipelineFailureKind::registry_shape,
                                      CalibrationDefinitions::stable);
            if (*median != evaluation->median_duration().value() ||
                *deviation != evaluation->maximum_absolute_deviation().value() ||
                stable_view !=
                    std::optional<std::string_view>{evaluation->stable() ? "true" : "false"})
                return parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                      CalibrationDefinitions::final_evaluation);
        }
        if (bandwidth_kind) {
            auto selected = kernel::SequentialReadBandwidthPassCount::create(*selected_raw);
            if (!selected)
                return parsed_failure(ActivePipelineFailureKind::typed_factory);
            result.warmup.emplace(measurement::detail::WarmupAccess::make_outcome(
                std::move(*policy), *selected, std::move(warmup_), disposition, evaluation));
        } else {
            auto selected = kernel::DependentLoadLatencyStepCount::create(*selected_raw);
            if (!selected)
                return parsed_failure(ActivePipelineFailureKind::typed_factory);
            result.warmup.emplace(measurement::detail::WarmupAccess::make_outcome(
                std::move(*policy), *selected, std::move(warmup_), disposition, evaluation));
        }
    }
    return result;
}

namespace {
struct ObservationDefinitions final {
    static constexpr auto factual = required_definition("factual_observation");
    static constexpr auto factual_oid =
        required_definition("scalar__factual_observation__observation_id");
    static constexpr auto measurement = required_definition("measurement");
    static constexpr auto bandwidth = required_definition("bandwidth_measurement");
    static constexpr auto latency = required_definition("latency_measurement");
    static constexpr auto bw_array = required_definition("array__bandwidth_measurement__workers");
    static constexpr auto lat_array = required_definition("array__latency_measurement__workers");
    static constexpr auto bw_worker = required_definition("bandwidth_worker");
    static constexpr auto lat_worker = required_definition("latency_worker");
    static constexpr auto interval = required_definition("interval");
    static constexpr auto start = required_definition("scalar__interval__start");
    static constexpr auto end = required_definition("scalar__interval__end");
    static constexpr auto elapsed = required_definition("scalar__interval__elapsed_nanoseconds");
    static constexpr auto bw_oid =
        required_definition("scalar__bandwidth_measurement__observation_id");
    static constexpr auto bw_eid =
        required_definition("scalar__bandwidth_measurement__experiment_id");
    static constexpr auto bw_bid = required_definition("scalar__bandwidth_measurement__block_id");
    static constexpr auto bw_impl =
        required_definition("scalar__bandwidth_measurement__implementation_id");
    static constexpr auto bw_bytes =
        required_definition("scalar__bandwidth_configuration__bytes_per_worker");
    static constexpr auto bw_pass =
        required_definition("scalar__bandwidth_measurement__pass_count");
    static constexpr auto bw_total =
        required_definition("scalar__bandwidth_measurement__aggregate_work");
    static constexpr auto bw_wid = required_definition("scalar__bandwidth_worker__worker_id");
    static constexpr auto bw_work =
        required_definition("scalar__bandwidth_work__logical_bytes_read");
    static constexpr auto bw_expected =
        required_definition("scalar__bandwidth_verification__expected");
    static constexpr auto bw_actual = required_definition("scalar__bandwidth_verification__actual");
    static constexpr auto bw_matched =
        required_definition("scalar__bandwidth_verification__matched");
    static constexpr auto lat_oid =
        required_definition("scalar__latency_measurement__observation_id");
    static constexpr auto lat_eid =
        required_definition("scalar__latency_measurement__experiment_id");
    static constexpr auto lat_bid = required_definition("scalar__latency_measurement__block_id");
    static constexpr auto lat_impl =
        required_definition("scalar__latency_measurement__implementation_id");
    static constexpr auto lat_bytes =
        required_definition("scalar__latency_configuration__steps_per_worker");
    static constexpr auto seed_mode = required_definition("scalar__latency_seed__mode");
    static constexpr auto seed_option = required_definition("option__latency_seed__value");
    static constexpr auto seed_value = required_definition("scalar__latency_seed__value");
    static constexpr auto lat_total =
        required_definition("scalar__latency_measurement__aggregate_work");
    static constexpr auto lat_wid = required_definition("scalar__latency_worker__worker_id");
    static constexpr auto lat_completed =
        required_definition("scalar__latency_worker__completed_work");
    static constexpr auto lat_start = required_definition("scalar__latency_schedule__start_index");
    static constexpr auto lat_steps = required_definition("scalar__latency_schedule__step_count");
    static constexpr auto lat_expected =
        required_definition("scalar__latency_verification__expected");
    static constexpr auto lat_actual = required_definition("scalar__latency_verification__actual");
    static constexpr auto lat_matched =
        required_definition("scalar__latency_verification__matched");
    static constexpr auto validity = required_definition("validity");
    static constexpr auto validity_oid = required_definition("scalar__validity__observation_id");
    static constexpr auto disposition = required_definition("scalar__validity__disposition");
    static constexpr auto checks = required_definition("array__validity__checks");
    static constexpr auto check = required_definition("validity_check");
    static constexpr auto check_id = required_definition("scalar__validity_check__id");
    static constexpr auto outcome = required_definition("scalar__validity_check__outcome");
    static constexpr auto maximum_n =
        required_definition("scalar__validity_policy__maximum_harness_overhead_numerator");
    static constexpr auto maximum_d =
        required_definition("scalar__validity_policy__maximum_harness_overhead_denominator");
    static constexpr auto estimate = required_definition("scalar__timer_overhead__estimate");
    static constexpr auto fraction = required_definition("option__timer_overhead__fraction");
    static constexpr auto fraction_n =
        required_definition("scalar__timer_fraction__numerator_nanoseconds");
    static constexpr auto fraction_d =
        required_definition("scalar__timer_fraction__denominator_nanoseconds");
};

auto observation_token(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                       std::uint32_t definition, const PreparedParsedNode& parent) noexcept
    -> std::optional<std::string_view> {
    const auto* node = parsed_node_inside(nodes, definition, parent);
    const auto view = node ? lexical(body, *node) : std::nullopt;
    return view ? quoted_payload(*view) : std::nullopt;
}

auto observation_bool(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                      std::uint32_t definition, const PreparedParsedNode& parent) noexcept
    -> std::optional<bool> {
    const auto* node = parsed_node_inside(nodes, definition, parent);
    const auto view = node ? lexical(body, *node) : std::nullopt;
    if (view == "true")
        return true;
    if (view == "false")
        return false;
    return std::nullopt;
}

auto observation_interval(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                          const PreparedParsedNode& parent) noexcept
    -> std::optional<measurement::MeasurementInterval> {
    const auto* node = parsed_node_inside(nodes, ObservationDefinitions::interval, parent);
    if (!node)
        return std::nullopt;
    const auto start = unsigned_inside(body, nodes, ObservationDefinitions::start, *node);
    const auto end = unsigned_inside(body, nodes, ObservationDefinitions::end, *node);
    const auto elapsed = unsigned_inside(body, nodes, ObservationDefinitions::elapsed, *node);
    if (!start || !end || !elapsed || *end < *start || *elapsed != *end - *start)
        return std::nullopt;
    auto result = measurement::MeasurementInterval::create(
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(*start),
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(*end));
    return result ? std::optional<measurement::MeasurementInterval>{std::move(*result)}
                  : std::nullopt;
}
} // namespace

auto PreparedMeasurementValidityBuilder::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedMeasurementValidityBuilder,
                     prepared_resources_v1::PreparedResourceError> {
    try {
        if (resource.definitions.size() != wire_v1::detail::schema_nodes.size() ||
            resource.definitions[ObservationDefinitions::factual].owner_occurrences == 0U)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        const auto bandwidth = stock_cardinality(resource, ObservationDefinitions::bw_array);
        const auto latency = stock_cardinality(resource, ObservationDefinitions::lat_array);
        if (!bandwidth || !latency)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        std::vector<measurement::SequentialReadBandwidthWorkerObservation> bw;
        std::vector<measurement::DependentLoadLatencyWorkerObservation> lat;
        bw.reserve(*bandwidth);
        lat.reserve(*latency);
        return PreparedMeasurementValidityBuilder{std::move(bw), std::move(lat)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedMeasurementValidityBuilder::build(std::span<const char> body,
                                               std::span<const PreparedParsedNode> nodes,
                                               const PreparedParsedNode& factual) noexcept
    -> std::expected<ReconstructedMeasurementValidity, ActiveCodecFailure> {
    const auto fail = [](ActivePipelineFailureKind kind, std::uint32_t definition) {
        return std::expected<ReconstructedMeasurementValidity, ActiveCodecFailure>{
            std::unexpected{parsed_failure(kind, definition)}};
    };
    if (consumed_)
        return fail(ActivePipelineFailureKind::transport_state, 0U);
    consumed_ = true;
    const auto* sum = parsed_node_inside(nodes, ObservationDefinitions::measurement, factual);
    const auto* bw =
        sum ? parsed_node_inside(nodes, ObservationDefinitions::bandwidth, *sum) : nullptr;
    const auto* lat =
        sum ? parsed_node_inside(nodes, ObservationDefinitions::latency, *sum) : nullptr;
    if ((bw == nullptr) == (lat == nullptr))
        return fail(ActivePipelineFailureKind::registry_shape, ObservationDefinitions::measurement);

    std::optional<measurement::MeasurementObservation> measured;
    if (bw) {
        const auto oid = unsigned_inside(body, nodes, ObservationDefinitions::bw_oid, *bw);
        const auto eid = unsigned_inside(body, nodes, ObservationDefinitions::bw_eid, *bw);
        const auto bid = unsigned_inside(body, nodes, ObservationDefinitions::bw_bid, *bw);
        const auto implementation_token = observation_token(body, nodes, ObservationDefinitions::bw_impl, *bw);
        const auto impl = implementation_token ? kernel::parse_kernel_implementation_wire_id(
            *implementation_token, kernel::KernelFamily::bandwidth) : std::nullopt;
        const auto bytes = unsigned_inside(body, nodes, ObservationDefinitions::bw_bytes, *bw);
        const auto passes = unsigned_inside(body, nodes, ObservationDefinitions::bw_pass, *bw);
        const auto total = unsigned_inside(body, nodes, ObservationDefinitions::bw_total, *bw);
        auto enclosing = observation_interval(body, nodes, *bw);
        const auto* array = parsed_node_inside(nodes, ObservationDefinitions::bw_array, *bw);
        if (!oid || !eid || !bid || !impl || !bytes || !passes ||
            !total || !enclosing || !array || array->element_count > bandwidth_.capacity())
            return fail(ActivePipelineFailureKind::typed_factory,
                        ObservationDefinitions::bandwidth);
        auto config =
            kernel::SequentialReadBandwidthConfig::create(core::ByteCount::from_base_units(*bytes));
        auto pass = kernel::SequentialReadBandwidthPassCount::create(*passes);
        if (!config || !pass)
            return fail(ActivePipelineFailureKind::typed_factory,
                        ObservationDefinitions::bandwidth);
        for (std::uint64_t ordinal{}; ordinal < array->element_count; ++ordinal) {
            const auto* worker =
                parsed_node_inside(nodes, ObservationDefinitions::bw_worker, *array, ordinal);
            const auto id =
                worker ? unsigned_inside(body, nodes, ObservationDefinitions::bw_wid, *worker)
                       : std::nullopt;
            auto interval = worker ? observation_interval(body, nodes, *worker) : std::nullopt;
            const auto work =
                worker ? unsigned_inside(body, nodes, ObservationDefinitions::bw_work, *worker)
                       : std::nullopt;
            const auto expected =
                worker ? unsigned_inside(body, nodes, ObservationDefinitions::bw_expected, *worker)
                       : std::nullopt;
            const auto actual =
                worker ? unsigned_inside(body, nodes, ObservationDefinitions::bw_actual, *worker)
                       : std::nullopt;
            const auto matched =
                worker ? observation_bool(body, nodes, ObservationDefinitions::bw_matched, *worker)
                       : std::nullopt;
            const auto expected_work =
                config->working_set_per_worker().checked_multiply(pass->value());
            if (!id || *id > std::numeric_limits<std::uint32_t>::max() || !interval || !work ||
                !expected || !actual || !matched || *matched != (*expected == *actual) ||
                !expected_work || expected_work->value() != *work)
                return fail(ActivePipelineFailureKind::ledger_validation,
                            ObservationDefinitions::bw_worker);
            bandwidth_.push_back(measurement::SequentialReadBandwidthWorkerObservation::create(
                core::WorkerId{static_cast<std::uint32_t>(*id)}, std::move(*interval),
                kernel::detail::KernelLeafAccess::bandwidth_work(
                    core::ByteCount::from_base_units(*work)),
                kernel::detail::KernelLeafAccess::bandwidth_verification(*expected, *actual)));
        }
        auto value = measurement::SequentialReadBandwidthObservation::create(
            core::ObservationId{*oid}, core::ExperimentId{*eid},
            kernel::DependentLoadLatencyBlockId{*bid},
            *impl, *config, *pass,
            std::move(bandwidth_), std::move(*enclosing));
        if (!value || value->aggregate_work().value() != *total)
            return fail(ActivePipelineFailureKind::ledger_validation,
                        ObservationDefinitions::bandwidth);
        measured.emplace(std::move(*value));
    } else {
        const auto oid = unsigned_inside(body, nodes, ObservationDefinitions::lat_oid, *lat);
        const auto eid = unsigned_inside(body, nodes, ObservationDefinitions::lat_eid, *lat);
        const auto bid = unsigned_inside(body, nodes, ObservationDefinitions::lat_bid, *lat);
        const auto implementation_token = observation_token(body, nodes, ObservationDefinitions::lat_impl, *lat);
        const auto impl = implementation_token ? kernel::parse_kernel_implementation_wire_id(
            *implementation_token, kernel::KernelFamily::latency) : std::nullopt;
        const auto bytes = unsigned_inside(body, nodes, ObservationDefinitions::lat_bytes, *lat);
        const auto total = unsigned_inside(body, nodes, ObservationDefinitions::lat_total, *lat);
        auto enclosing = observation_interval(body, nodes, *lat);
        const auto mode = observation_token(body, nodes, ObservationDefinitions::seed_mode, *lat);
        const auto* option = parsed_node_inside(nodes, ObservationDefinitions::seed_option, *lat);
        const auto* array = parsed_node_inside(nodes, ObservationDefinitions::lat_array, *lat);
        if (!oid || !eid || !bid || !impl || !bytes || !total ||
            !enclosing || !mode || !option || !array || array->element_count > latency_.capacity())
            return fail(ActivePipelineFailureKind::typed_factory, ObservationDefinitions::latency);
        std::optional<kernel::PointerCycleSeedIntent> seed;
        if (*mode == "derive_per_experiment" && !option->present)
            seed = kernel::PointerCycleSeedIntent::derive_per_experiment();
        else if (*mode == "explicit_value" && option->present) {
            const auto value =
                unsigned_inside(body, nodes, ObservationDefinitions::seed_value, *option);
            if (value)
                seed = kernel::PointerCycleSeedIntent::explicit_value(*value);
        }
        if (!seed)
            return fail(ActivePipelineFailureKind::ledger_validation,
                        ObservationDefinitions::seed_option);
        auto config = kernel::DependentLoadLatencyConfig::create(
            core::ByteCount::from_base_units(*bytes), *seed);
        if (!config)
            return fail(ActivePipelineFailureKind::typed_factory, ObservationDefinitions::latency);
        for (std::uint64_t ordinal{}; ordinal < array->element_count; ++ordinal) {
            const auto* worker =
                parsed_node_inside(nodes, ObservationDefinitions::lat_worker, *array, ordinal);
            const auto id =
                worker ? unsigned_inside(body, nodes, ObservationDefinitions::lat_wid, *worker)
                       : std::nullopt;
            auto interval = worker ? observation_interval(body, nodes, *worker) : std::nullopt;
            const auto start =
                worker ? unsigned_inside(body, nodes, ObservationDefinitions::lat_start, *worker)
                       : std::nullopt;
            const auto steps =
                worker ? unsigned_inside(body, nodes, ObservationDefinitions::lat_steps, *worker)
                       : std::nullopt;
            const auto completed =
                worker
                    ? unsigned_inside(body, nodes, ObservationDefinitions::lat_completed, *worker)
                    : std::nullopt;
            const auto expected =
                worker ? unsigned_inside(body, nodes, ObservationDefinitions::lat_expected, *worker)
                       : std::nullopt;
            const auto actual =
                worker ? unsigned_inside(body, nodes, ObservationDefinitions::lat_actual, *worker)
                       : std::nullopt;
            const auto matched =
                worker ? observation_bool(body, nodes, ObservationDefinitions::lat_matched, *worker)
                       : std::nullopt;
            if (!steps)
                return fail(ActivePipelineFailureKind::typed_factory,
                            ObservationDefinitions::lat_worker);
            auto count = kernel::DependentLoadLatencyStepCount::create(*steps);
            if (!id || *id > std::numeric_limits<std::uint32_t>::max() || !interval || !start ||
                *start > std::numeric_limits<std::size_t>::max() || !count || !completed ||
                *completed != *steps || !expected || !actual || !matched ||
                *matched != (*expected == *actual) ||
                *expected > std::numeric_limits<std::size_t>::max() ||
                *actual > std::numeric_limits<std::size_t>::max())
                return fail(ActivePipelineFailureKind::ledger_validation,
                            ObservationDefinitions::lat_worker);
            latency_.push_back(measurement::DependentLoadLatencyWorkerObservation::create(
                core::WorkerId{static_cast<std::uint32_t>(*id)}, std::move(*interval),
                kernel::detail::KernelLeafAccess::schedule(static_cast<std::size_t>(*start),
                                                           *count),
                kernel::detail::KernelLeafAccess::latency_verification(
                    static_cast<std::size_t>(*expected), static_cast<std::size_t>(*actual))));
        }
        auto value = measurement::DependentLoadLatencyObservation::create(
            core::ObservationId{*oid}, core::ExperimentId{*eid},
            kernel::DependentLoadLatencyBlockId{*bid},
            *impl, *config,
            std::move(latency_), std::move(*enclosing));
        if (!value || value->aggregate_work().value() != *total)
            return fail(ActivePipelineFailureKind::ledger_validation,
                        ObservationDefinitions::latency);
        measured.emplace(std::move(*value));
    }

    const auto* validity = parsed_node_inside(nodes, ObservationDefinitions::validity, factual);
    const auto oid =
        validity ? unsigned_inside(body, nodes, ObservationDefinitions::validity_oid, *validity)
                 : std::nullopt;
    const auto disposition =
        validity ? observation_token(body, nodes, ObservationDefinitions::disposition, *validity)
                 : std::nullopt;
    const auto n = validity
                       ? unsigned_inside(body, nodes, ObservationDefinitions::maximum_n, *validity)
                       : std::nullopt;
    const auto d = validity
                       ? unsigned_inside(body, nodes, ObservationDefinitions::maximum_d, *validity)
                       : std::nullopt;
    const auto estimate =
        validity ? unsigned_inside(body, nodes, ObservationDefinitions::estimate, *validity)
                 : std::nullopt;
    const auto* checks_node =
        validity ? parsed_node_inside(nodes, ObservationDefinitions::checks, *validity) : nullptr;
    if (!oid || !disposition || !n || !d || !estimate || !checks_node ||
        checks_node->element_count != measurement::observation_validity_check_count)
        return fail(ActivePipelineFailureKind::registry_shape, ObservationDefinitions::validity);
    auto maximum = measurement::MaximumHarnessOverhead::create(*n, *d);
    if (!maximum)
        return fail(ActivePipelineFailureKind::typed_factory, ObservationDefinitions::validity);
    std::array<measurement::ObservationValidityCheckOutcome,
               measurement::observation_validity_check_count>
        outcomes{};
    for (std::uint64_t index{}; index < outcomes.size(); ++index) {
        const auto* item =
            parsed_node_inside(nodes, ObservationDefinitions::check, *checks_node, index);
        const auto id = item ? unsigned_inside(body, nodes, ObservationDefinitions::check_id, *item)
                             : std::nullopt;
        const auto token =
            item ? observation_token(body, nodes, ObservationDefinitions::outcome, *item)
                 : std::nullopt;
        if (!id || *id != index || !token)
            return fail(ActivePipelineFailureKind::ledger_validation,
                        ObservationDefinitions::checks);
        if (*token == "pass")
            outcomes[index] = measurement::ObservationValidityCheckOutcome::pass;
        else if (*token == "fail")
            outcomes[index] = measurement::ObservationValidityCheckOutcome::fail;
        else if (*token == "warning")
            outcomes[index] = measurement::ObservationValidityCheckOutcome::warning;
        else if (*token == "unavailable")
            outcomes[index] = measurement::ObservationValidityCheckOutcome::unavailable;
        else if (*token == "not_applicable")
            outcomes[index] = measurement::ObservationValidityCheckOutcome::not_applicable;
        else
            return fail(ActivePipelineFailureKind::registry_domain,
                        ObservationDefinitions::outcome);
    }
    const bool any_fail = std::ranges::any_of(outcomes, [](auto value) {
        return value == measurement::ObservationValidityCheckOutcome::fail;
    });
    if ((*disposition == "invalid") != any_fail ||
        (*disposition != "valid" && *disposition != "invalid"))
        return fail(ActivePipelineFailureKind::ledger_validation,
                    ObservationDefinitions::disposition);
    measurement::ObservationValidityChecks checks{
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::kernel_correctness, outcomes[0]),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::worker_interval_containment, outcomes[1]),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::timer_overhead, outcomes[2]),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::placement, outcomes[3]),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::migration, outcomes[4]),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::major_page_faults, outcomes[5]),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::throttling, outcomes[6]),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::context_switching, outcomes[7]),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::frequency_variation, outcomes[8]),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::thermal_variation, outcomes[9])};
    std::optional<measurement::TimerOverheadFraction> fraction;
    const auto* option = parsed_node_inside(nodes, ObservationDefinitions::fraction, *validity);
    if (!option)
        return fail(ActivePipelineFailureKind::registry_shape, ObservationDefinitions::fraction);
    if (option->present) {
        const auto fn = unsigned_inside(body, nodes, ObservationDefinitions::fraction_n, *option);
        const auto fd = unsigned_inside(body, nodes, ObservationDefinitions::fraction_d, *option);
        if (!fn || !fd || *fd == 0U)
            return fail(ActivePipelineFailureKind::typed_factory, ObservationDefinitions::fraction);
        fraction.emplace(measurement::TimerOverheadFraction::create(*fn, *fd));
    }
    auto evidence = measurement::ObservationValidityEvidence::create(
        core::ObservationId{*oid},
        any_fail ? measurement::ObservationValidityDisposition::invalid
                 : measurement::ObservationValidityDisposition::valid,
        std::move(checks), measurement::ObservationValidityPolicy::create(*maximum),
        measurement::TimerOverheadValidityEvidence::create(
            measurement::TimerOverheadEstimate::create(core::Duration::from_base_units(*estimate)),
            *maximum, fraction));
    const auto identity_matches = std::visit(
        [&](const auto& value) { return value.observation_id() == evidence.observation_id(); },
        *measured);
    const auto factual_oid =
        unsigned_inside(body, nodes, ObservationDefinitions::factual_oid, factual);
    const auto threshold_n =
        unsigned_inside(body, nodes, ObservationDefinitions::maximum_n, *validity, 1U);
    const auto threshold_d =
        unsigned_inside(body, nodes, ObservationDefinitions::maximum_d, *validity, 1U);
    if (!identity_matches || !factual_oid || *factual_oid != *oid || !threshold_n || !threshold_d ||
        *threshold_n != *n || *threshold_d != *d)
        return fail(ActivePipelineFailureKind::ledger_validation, ObservationDefinitions::validity);
    const auto projections_match = std::visit(
        [&](const auto& value) {
            const bool verification = std::ranges::all_of(value.workers(), [](const auto& worker) {
                return worker.verification().matched();
            });
            const bool containment = std::ranges::all_of(value.workers(), [&](const auto& worker) {
                return value.enclosing_interval().start() <= worker.interval().start() &&
                       worker.interval().end() <= value.enclosing_interval().end();
            });
            const auto elapsed = value.enclosing_interval().elapsed().value();
            const bool timer = elapsed != 0U && rational_le(*estimate, elapsed, *n, *d);
            const bool fraction_matches =
                elapsed == 0U ? !fraction.has_value()
                              : fraction && fraction->numerator_nanoseconds() == *estimate &&
                                    fraction->denominator_nanoseconds() == elapsed;
            const auto projected = [](bool passed) {
                return passed ? measurement::ObservationValidityCheckOutcome::pass
                              : measurement::ObservationValidityCheckOutcome::fail;
            };
            return outcomes[0] == projected(verification) &&
                   outcomes[1] == projected(containment) && outcomes[2] == projected(timer) &&
                   fraction_matches;
        },
        *measured);
    if (!projections_match)
        return fail(ActivePipelineFailureKind::ledger_validation, ObservationDefinitions::validity);
    return ReconstructedMeasurementValidity{std::move(*measured), std::move(evidence)};
}

namespace {
struct MonitoringDefinitions final {
    static constexpr auto option = required_definition("option__factual_observation__monitoring");
    static constexpr auto result = required_definition("monitoring_result");
    static constexpr auto evidence = required_definition("monitoring_evidence");
    static constexpr auto page_state =
        required_definition("scalar__monitoring_evidence__page_faults_state");
    static constexpr auto context_state =
        required_definition("scalar__monitoring_evidence__context_switches_state");
    static constexpr auto temperature_state =
        required_definition("scalar__monitoring_evidence__temperature_state");
    static constexpr auto frequency_state =
        required_definition("scalar__monitoring_evidence__frequency_state");
    static constexpr auto throttling_state =
        required_definition("scalar__monitoring_evidence__throttling_state");
    static constexpr auto page_option = required_definition("optional_page_evidence");
    static constexpr auto page = required_definition("page_evidence");
    static constexpr auto page_workers = required_definition("array__page_evidence__workers");
    static constexpr auto page_worker = required_definition("page_worker_evidence");
    static constexpr auto page_worker_id =
        required_definition("scalar__page_worker_evidence__worker_id");
    static constexpr auto page_worker_state =
        required_definition("scalar__page_worker_evidence__state");
    static constexpr auto page_minor =
        required_definition("option__page_worker_evidence__minor_delta");
    static constexpr auto page_major =
        required_definition("option__page_worker_evidence__major_delta");
    static constexpr auto context_option = required_definition("optional_context_evidence");
    static constexpr auto context = required_definition("context_evidence");
    static constexpr auto context_workers = required_definition("array__context_evidence__workers");
    static constexpr auto context_worker = required_definition("context_worker_evidence");
    static constexpr auto context_worker_id =
        required_definition("scalar__context_worker_evidence__worker_id");
    static constexpr auto context_worker_state =
        required_definition("scalar__context_worker_evidence__state");
    static constexpr auto voluntary =
        required_definition("option__context_worker_evidence__voluntary_count");
    static constexpr auto involuntary =
        required_definition("option__context_worker_evidence__involuntary_count");
    static constexpr auto temperature_option = required_definition("optional_temperature_evidence");
    static constexpr auto temperature = required_definition("temperature_evidence");
    static constexpr auto temperature_count =
        required_definition("scalar__temperature_evidence__sample_count");
    static constexpr auto temperature_min =
        required_definition("option__temperature_evidence__observed_minimum");
    static constexpr auto temperature_max =
        required_definition("option__temperature_evidence__observed_maximum");
    static constexpr auto temperature_range =
        required_definition("option__temperature_evidence__observed_range");
    static constexpr auto temperature_complete =
        required_definition("scalar__temperature_evidence__capture_completeness");
    static constexpr auto frequency_option = required_definition("optional_frequency_evidence");
    static constexpr auto frequency = required_definition("frequency_evidence");
    static constexpr auto frequency_domains =
        required_definition("array__frequency_evidence__domains");
    static constexpr auto frequency_domain = required_definition("frequency_domain_evidence");
    static constexpr auto frequency_count =
        required_definition("scalar__frequency_domain_evidence__sample_count");
    static constexpr auto frequency_kind =
        required_definition("scalar__frequency_domain_evidence__source_kind");
    static constexpr auto frequency_domain_state =
        required_definition("scalar__frequency_domain_evidence__state");
    static constexpr auto frequency_min =
        required_definition("option__frequency_domain_evidence__observed_minimum");
    static constexpr auto frequency_max =
        required_definition("option__frequency_domain_evidence__observed_maximum");
    static constexpr auto frequency_range =
        required_definition("option__frequency_domain_evidence__observed_range");
    static constexpr auto frequency_variation =
        required_definition("option__frequency_domain_evidence__observed_variation");
    static constexpr auto frequency_complete =
        required_definition("scalar__frequency_evidence__capture_completeness");
    static constexpr auto domain = required_definition("frequency_domain_id");
    static constexpr auto cpus = required_definition("array__frequency_domain_id__cpus");
    static constexpr auto cpu = required_definition("linux_cpu_id");
    static constexpr auto duration = required_definition("duration");
    static constexpr auto u64 = required_definition("u64");
    static constexpr auto timestamp = required_definition("timestamp");
    static constexpr auto raw = required_definition("raw_capture");
    static constexpr auto page_before = required_definition("array__raw_capture__page_before");
    static constexpr auto page_after = required_definition("array__raw_capture__page_after");
    static constexpr auto context_samples =
        required_definition("array__raw_capture__context_acquisitions");
    static constexpr auto temperature_samples =
        required_definition("array__raw_capture__temperature_acquisitions");
    static constexpr auto frequency_groups =
        required_definition("array__raw_capture__frequency_acquisitions");
    static constexpr auto frequency_metadata =
        required_definition("array__raw_capture__frequency_metadata");
    static constexpr auto raw_page = required_definition("raw_page_acquisition");
    static constexpr auto raw_context = required_definition("raw_context_acquisition");
    static constexpr auto raw_temperature = required_definition("raw_temperature_acquisition");
    static constexpr auto raw_frequency = required_definition("raw_frequency_acquisition");
    static constexpr auto raw_channel = required_definition("scalar__raw_acquisition__channel");
    static constexpr auto raw_implementation =
        required_definition("scalar__raw_acquisition__implementation_id");
    static constexpr auto raw_phase = required_definition("scalar__raw_acquisition__phase");
    static constexpr auto raw_status = required_definition("scalar__raw_acquisition__status");
    static constexpr auto page_payload_option =
        required_definition("option__page_acquisition_payload");
    static constexpr auto page_payload = required_definition("page_payload");
    static constexpr auto payload_worker = required_definition("scalar__page_payload__worker_id");
    static constexpr auto payload_minor = required_definition("scalar__page_payload__minor_faults");
    static constexpr auto payload_major = required_definition("scalar__page_payload__major_faults");
    static constexpr auto context_payload_option =
        required_definition("option__context_acquisition_payload");
    static constexpr auto context_payload = required_definition("context_payload");
    static constexpr auto context_payload_worker =
        required_definition("scalar__context_payload__worker_id");
    static constexpr auto context_payload_complete =
        required_definition("scalar__context_payload__completeness");
    static constexpr auto context_events = required_definition("array__context_payload__events");
    static constexpr auto context_event = required_definition("context_event");
    static constexpr auto event_worker = required_definition("scalar__context_event__worker_id");
    static constexpr auto event_timestamp = required_definition("scalar__context_event__timestamp");
    static constexpr auto event_kind = required_definition("scalar__context_event__kind");
    static constexpr auto temperature_payload_option =
        required_definition("option__temperature_acquisition_payload");
    static constexpr auto temperature_payload = required_definition("temperature_payload");
    static constexpr auto temperature_value =
        required_definition("scalar__temperature_payload__temperature");
    static constexpr auto frequency_group = required_definition("frequency_domain_acquisitions");
    static constexpr auto frequency_samples =
        required_definition("array__frequency_domain_acquisitions__acquisitions");
    static constexpr auto frequency_payload_option =
        required_definition("option__frequency_acquisition_payload");
    static constexpr auto frequency_payload = required_definition("frequency_snapshot");
    static constexpr auto frequency_value =
        required_definition("scalar__frequency_payload__frequency");
    static constexpr auto frequency_payload_kind =
        required_definition("scalar__frequency_payload__source_kind");
    static constexpr auto metadata = required_definition("frequency_metadata");
    static constexpr auto driver =
        required_definition("scalar__frequency_metadata__scaling_driver");
    static constexpr auto governor =
        required_definition("scalar__frequency_metadata__scaling_governor");
    static constexpr auto scaling_min =
        required_definition("scalar__frequency_metadata__scaling_minimum");
    static constexpr auto scaling_max =
        required_definition("scalar__frequency_metadata__scaling_maximum");
    static constexpr auto metadata_kind =
        required_definition("scalar__frequency_metadata__source_kind");
    static constexpr auto activity = required_definition("activity");
    static constexpr std::array activity_values{
        required_definition("scalar__activity__scheduled_rounds"),
        required_definition("scalar__activity__completed_rounds"),
        required_definition("scalar__activity__missed_rounds"),
        required_definition("scalar__activity__service_wakeups"),
        required_definition("scalar__activity__context_drains"),
        required_definition("scalar__activity__temperature_attempts"),
        required_definition("scalar__activity__frequency_attempts")};
};

auto monitoring_signal(std::optional<std::string_view> token)
    -> std::optional<monitoring::MonitoringSignalState> {
    if (token == "clear")
        return monitoring::MonitoringSignalState::clear;
    if (token == "detected")
        return monitoring::MonitoringSignalState::detected;
    if (token == "unavailable")
        return monitoring::MonitoringSignalState::unavailable;
    if (token == "not_applicable")
        return monitoring::MonitoringSignalState::not_applicable;
    return std::nullopt;
}
auto monitoring_frequency_kind(std::optional<std::string_view> token)
    -> std::optional<monitoring::FrequencySourceKind> {
    if (token == "hardware_current")
        return monitoring::FrequencySourceKind::hardware_current;
    if (token == "scaling_current")
        return monitoring::FrequencySourceKind::scaling_current;
    return std::nullopt;
}
auto signed_inside(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                   std::uint32_t definition, const PreparedParsedNode& parent) noexcept
    -> std::optional<std::int64_t> {
    const auto* node = parsed_node_inside(nodes, definition, parent);
    const auto view = node ? lexical(body, *node) : std::nullopt;
    const auto payload = view ? quoted_payload(*view) : std::nullopt;
    if (!payload)
        return std::nullopt;
    std::int64_t value{};
    const auto parsed = std::from_chars(payload->data(), payload->data() + payload->size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == payload->data() + payload->size()
               ? std::optional{value}
               : std::nullopt;
}

struct PageReplay final {
    std::span<const monitoring::WorkerPageFaultEvidence> expected;
    std::size_t index{};
    bool equal{true};
    static auto append(void* opaque, core::WorkerId worker, monitoring::MonitoringSignalState state,
                       std::optional<monitoring::PageFaultCount> minor,
                       std::optional<monitoring::PageFaultCount> major) noexcept -> bool {
        auto& self = *static_cast<PageReplay*>(opaque);
        if (self.index >= self.expected.size())
            return false;
        const auto& value = self.expected[self.index++];
        self.equal = self.equal && value.worker_id() == worker && value.state() == state &&
                     value.minor_delta() == minor && value.major_delta() == major;
        return true;
    }
};
struct ContextReplay final {
    std::span<const monitoring::WorkerContextSwitchEvidence> expected;
    std::size_t index{};
    bool equal{true};
    static auto append(void* opaque, core::WorkerId worker, monitoring::MonitoringSignalState state,
                       std::optional<monitoring::ContextSwitchCount> voluntary,
                       std::optional<monitoring::ContextSwitchCount> involuntary) noexcept -> bool {
        auto& self = *static_cast<ContextReplay*>(opaque);
        if (self.index >= self.expected.size())
            return false;
        const auto& value = self.expected[self.index++];
        self.equal = self.equal && value.worker_id() == worker && value.state() == state &&
                     value.voluntary_count() == voluntary &&
                     value.involuntary_count() == involuntary;
        return true;
    }
};
struct FrequencyReplay final {
    std::span<const monitoring::FrequencyDomainEvidence> expected;
    std::size_t index{};
    bool equal{true};
    static auto append(void* opaque,
                       const monitoring::detail::FrequencyDomainClassification& classified) noexcept
        -> bool {
        auto& self = *static_cast<FrequencyReplay*>(opaque);
        if (self.index >= self.expected.size() || classified.domain == nullptr)
            return false;
        const auto& value = self.expected[self.index++];
        self.equal = self.equal && value.domain_id() == *classified.domain &&
                     value.source_kind() == classified.kind && value.state() == classified.state &&
                     value.observed_minimum() == classified.minimum &&
                     value.observed_maximum() == classified.maximum &&
                     value.observed_range() == classified.range &&
                     value.observed_variation() == classified.variation &&
                     value.sample_count() == classified.count;
        return true;
    }
};
} // namespace

struct PreparedMonitoringBuilder::Storage final {
    bool actual_frequency_domains{};
    monitoring::MonitoringCaptureResult result;
    std::vector<std::vector<monitoring::ContextSwitchEvent>> context_event_pools;
    std::vector<std::vector<monitoring::FrequencyAcquisition>> frequency_sample_pools;
    std::vector<std::vector<core::LinuxCpuId>> cpu_pools;
    std::vector<std::string> implementation_ids;
    std::vector<std::string> drivers;
    std::vector<std::string> governors;
    std::vector<monitoring::PageFaultAcquisitionPair> page_pairs;
    std::vector<monitoring::ContextSwitchWorkerAcquisitionView> context_views;
    std::vector<monitoring::MonitoringImplementationId> context_view_ids;
    std::vector<const monitoring::MonitoringRawCaptureEvidence::TemperatureAcquisition*>
        temperature_scratch;
    std::vector<const monitoring::FrequencyAcquisition*> frequency_scratch;
    std::vector<std::vector<core::LinuxCpuId>> classifier_cpu_pools;
    std::vector<monitoring::FrequencyDomainAcquisitions> classifier_frequency_views;
    std::vector<std::optional<monitoring::detail::FrequencyDomainClassification>> frequency_results;
    std::vector<monitoring::WorkerPageFaultEvidence> page_evidence;
    std::vector<monitoring::WorkerContextSwitchEvidence> context_evidence;
    std::vector<monitoring::FrequencyDomainEvidence> frequency_evidence;
};

PreparedMonitoringBuilder::~PreparedMonitoringBuilder() noexcept = default;
PreparedMonitoringBuilder::PreparedMonitoringBuilder(std::unique_ptr<Storage> storage) noexcept
    : storage_{std::move(storage)} {}

auto PreparedMonitoringBuilder::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedMonitoringBuilder, prepared_resources_v1::PreparedResourceError> {
    return prepare_for_factual_owner(resource, 1U);
}

auto PreparedMonitoringBuilder::prepare_for_factual_owner(
    const prepared_resources_v1::PreparedResourceValue& resource, std::uint64_t owners,
    std::optional<std::size_t> domain_cpu_bound) noexcept
    -> std::expected<PreparedMonitoringBuilder, prepared_resources_v1::PreparedResourceError> {
    const auto mismatch = [] {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
    };
    try {
        if (owners == 0U || resource.definitions.size() != wire_v1::detail::schema_nodes.size())
            return mismatch();
        const auto card = [&](std::uint32_t id) { return stock_cardinality(resource, id); };
        const auto occurrences = [&](std::uint32_t id) -> std::optional<std::size_t> {
            const auto value = resource.definitions[id].owner_occurrences;
            // O is a whole-evidence stock. This builder owns one of the
            // enclosing factual observations, not a second copy of every O.
            return value % owners == 0U && value / owners <= std::numeric_limits<std::size_t>::max()
                       ? std::optional{static_cast<std::size_t>(value / owners)}
                       : std::nullopt;
        };
        const auto page_before = card(MonitoringDefinitions::page_before);
        const auto page_after = card(MonitoringDefinitions::page_after);
        const auto contexts = card(MonitoringDefinitions::context_samples);
        const auto temperatures = card(MonitoringDefinitions::temperature_samples);
        const auto groups = card(MonitoringDefinitions::frequency_groups);
        const auto metadata = card(MonitoringDefinitions::frequency_metadata);
        const auto page_workers = card(MonitoringDefinitions::page_workers);
        const auto context_workers = card(MonitoringDefinitions::context_workers);
        const auto frequency_domains = card(MonitoringDefinitions::frequency_domains);
        const auto event_pools = occurrences(MonitoringDefinitions::context_payload);
        const auto event_capacity = card(MonitoringDefinitions::context_events);
        const auto cpu_pools = occurrences(MonitoringDefinitions::domain);
        const auto cpu_capacity = domain_cpu_bound ? domain_cpu_bound : card(MonitoringDefinitions::cpus);
        const auto impl_count = occurrences(MonitoringDefinitions::raw_implementation);
        const auto driver_count = occurrences(MonitoringDefinitions::driver);
        const auto governor_count = occurrences(MonitoringDefinitions::governor);
        if (!page_before || !page_after || !contexts || !temperatures || !groups || !metadata ||
            !page_workers || !context_workers || !frequency_domains || !event_pools ||
            !event_capacity || !cpu_pools || !cpu_capacity || !impl_count || !driver_count ||
            !governor_count)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch, 1U}};
        auto storage = std::make_unique<Storage>();
        storage->actual_frequency_domains = domain_cpu_bound.has_value();
        storage->result.raw_.page_before_.reserve(*page_before);
        storage->result.raw_.page_after_.reserve(*page_after);
        storage->result.raw_.context_.reserve(*contexts);
        storage->result.raw_.temperature_.reserve(*temperatures);
        storage->result.raw_.frequency_storage_.reserve(*groups);
        storage->result.raw_.frequency_views_.reserve(*groups);
        storage->result.raw_.frequency_metadata_.reserve(*metadata);
        storage->context_event_pools.resize(*event_pools);
        for (auto& pool : storage->context_event_pools)
            pool.reserve(*event_capacity);
        storage->frequency_sample_pools.resize(*groups);
        const auto samples = card(MonitoringDefinitions::frequency_samples);
        if (!samples)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch, 2U}};
        for (auto& pool : storage->frequency_sample_pools)
            pool.reserve(*samples);
        storage->cpu_pools.resize(*cpu_pools);
        for (auto& pool : storage->cpu_pools)
            pool.reserve(*cpu_capacity);
        const auto string_limit = [&](std::uint32_t id) -> std::optional<std::size_t> {
            const auto value = resource.definitions[id].decoded_utf8_bytes;
            return value && *value <= std::numeric_limits<std::size_t>::max()
                       ? std::optional{static_cast<std::size_t>(*value)}
                       : std::nullopt;
        };
        const auto required_limit = [&](std::size_t count,
                                        std::uint32_t id) -> std::optional<std::size_t> {
            return count == 0U ? std::optional<std::size_t>{0U} : string_limit(id);
        };
        // Implementation IDs are closed registry tokens. The escaped__ names
        // are formula operands, with no actual schema owner or string stock.
        std::size_t implementation_bytes{};
        const auto& implementation = wire_v1::detail::schema_nodes[MonitoringDefinitions::raw_implementation];
        for (std::uint32_t index{}; index < implementation.token_count; ++index)
            implementation_bytes = std::max(implementation_bytes,
                wire_v1::detail::scalar_tokens[implementation.token_begin + index].size());
        const auto impl_bytes = std::optional{implementation_bytes};
        const auto driver_bytes =
            required_limit(*driver_count, MonitoringDefinitions::driver);
        const auto governor_bytes =
            required_limit(*governor_count, MonitoringDefinitions::governor);
        if (!impl_bytes || !driver_bytes || !governor_bytes)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch, 3U}};
        storage->implementation_ids.resize(*impl_count);
        for (auto& value : storage->implementation_ids)
            value.reserve(*impl_bytes);
        storage->drivers.resize(*driver_count);
        for (auto& value : storage->drivers)
            value.reserve(*driver_bytes);
        storage->governors.resize(*governor_count);
        for (auto& value : storage->governors)
            value.reserve(*governor_bytes);
        storage->page_pairs.reserve(*page_workers);
        storage->context_views.reserve(*context_workers);
        storage->context_view_ids.reserve(*context_workers);
        for (std::size_t index{}; index < *context_workers; ++index)
            storage->context_view_ids.push_back(monitoring::MonitoringImplementationId::create(
                "linux-perf-context-switch-records"));
        storage->page_evidence.reserve(*page_workers);
        storage->context_evidence.reserve(*context_workers);
        storage->frequency_evidence.reserve(*frequency_domains);
        storage->temperature_scratch.resize(*temperatures);
        storage->frequency_scratch.resize(*samples);
        storage->frequency_results.resize(*frequency_domains);
        storage->classifier_cpu_pools.resize(*frequency_domains);
        for (auto& pool : storage->classifier_cpu_pools) pool.reserve(*cpu_capacity);
        storage->classifier_frequency_views.reserve(*frequency_domains);
        return PreparedMonitoringBuilder{std::move(storage)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedMonitoringBuilder::build(
    std::span<const char> body, std::span<const PreparedParsedNode> nodes,
    const PreparedParsedNode& factual, std::span<const core::WorkerId> authenticated_workers,
    std::span<const core::LinuxCpuId> authenticated_cpus,
    platform_linux::RawMonotonicTimestamp observation_start,
    platform_linux::RawMonotonicTimestamp observation_end,
    std::span<const core::LinuxCpuId> selected_worker_cpus) noexcept
    -> std::expected<std::optional<monitoring::MonitoringCaptureResult>, ActiveCodecFailure> {
    using Result =
        std::expected<std::optional<monitoring::MonitoringCaptureResult>, ActiveCodecFailure>;
    const auto reject = [](ActivePipelineFailureKind kind, std::uint32_t definition) -> Result {
        return std::unexpected{parsed_failure(kind, definition)};
    };
    if (consumed_ || !storage_)
        return reject(ActivePipelineFailureKind::transport_state, 0U);
    consumed_ = true;
    const auto* option = parsed_node_inside(nodes, MonitoringDefinitions::option, factual);
    if (!option)
        return reject(ActivePipelineFailureKind::registry_shape, MonitoringDefinitions::option);
    if (!option->present)
        return std::optional<monitoring::MonitoringCaptureResult>{};
    const auto* root = parsed_node_inside(nodes, MonitoringDefinitions::result, *option);
    const auto* evidence =
        root ? parsed_node_inside(nodes, MonitoringDefinitions::evidence, *root) : nullptr;
    const auto* raw = root ? parsed_node_inside(nodes, MonitoringDefinitions::raw, *root) : nullptr;
    const auto* activity =
        root ? parsed_node_inside(nodes, MonitoringDefinitions::activity, *root) : nullptr;
    if (!root || !evidence || !raw || !activity)
        return reject(ActivePipelineFailureKind::registry_shape, MonitoringDefinitions::result);
    auto& result = storage_->result;
    const auto token = [&](std::uint32_t definition, const PreparedParsedNode& parent,
                           std::uint64_t ordinal = 0U) -> std::optional<std::string_view> {
        const auto* node = parsed_node_inside(nodes, definition, parent, ordinal);
        const auto value = node ? lexical(body, *node) : std::nullopt;
        return value ? quoted_payload(*value) : std::nullopt;
    };
    const auto option_u64 =
        [&](std::uint32_t definition,
            const PreparedParsedNode& parent) -> std::optional<std::optional<std::uint64_t>> {
        const auto* node = parsed_node_inside(nodes, definition, parent);
        if (!node)
            return std::nullopt;
        if (!node->present)
            return std::optional<std::uint64_t>{};
        const auto value = unsigned_inside(body, nodes, MonitoringDefinitions::u64, *node);
        return value ? std::optional<std::optional<std::uint64_t>>{*value} : std::nullopt;
    };
    const auto option_unsigned =
        [&](std::uint32_t definition, std::uint32_t child,
            const PreparedParsedNode& parent) -> std::optional<std::optional<std::uint64_t>> {
        const auto* node = parsed_node_inside(nodes, definition, parent);
        if (!node)
            return std::nullopt;
        if (!node->present)
            return std::optional<std::uint64_t>{};
        const auto value = unsigned_inside(body, nodes, child, *node);
        return value ? std::optional<std::optional<std::uint64_t>>{*value} : std::nullopt;
    };
    const auto states =
        std::array{monitoring_signal(token(MonitoringDefinitions::page_state, *evidence)),
                   monitoring_signal(token(MonitoringDefinitions::context_state, *evidence)),
                   monitoring_signal(token(MonitoringDefinitions::temperature_state, *evidence)),
                   monitoring_signal(token(MonitoringDefinitions::frequency_state, *evidence))};
    const auto throttling =
        monitoring_signal(token(MonitoringDefinitions::throttling_state, *evidence));
    if (std::ranges::any_of(states, [](const auto& value) { return !value; }) ||
        throttling != monitoring::MonitoringSignalState::not_applicable)
        return reject(ActivePipelineFailureKind::ledger_validation,
                      MonitoringDefinitions::evidence);
    result.evidence_.states_ = {*states[0], *states[1], *states[2], *states[3]};

    const auto coherent_presence = [&](const PreparedParsedNode* wrapper, const PreparedParsedNode* child,
                                       monitoring::MonitoringSignalState state) noexcept {
        return wrapper && (wrapper->present
            ? child && state != monitoring::MonitoringSignalState::not_applicable
            : state == monitoring::MonitoringSignalState::not_applicable ||
                state == monitoring::MonitoringSignalState::unavailable);
    };
    const auto* page_option = parsed_node_inside(nodes, MonitoringDefinitions::page_option, *evidence);
    const auto* page = page_option && page_option->present
        ? parsed_node_inside(nodes, MonitoringDefinitions::page, *page_option) : nullptr;
    if (!coherent_presence(page_option, page, *states[0]))
        return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::page);
    if (page) {
        const auto state =
            monitoring_signal(token(required_definition("scalar__page_evidence__state"), *page));
        const auto* workers = parsed_node_inside(nodes, MonitoringDefinitions::page_workers, *page);
        if (!state || *state != *states[0] || !workers ||
            workers->element_count > authenticated_workers.size())
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::page);
        auto& values = storage_->page_evidence;
        for (std::uint64_t index{}; index < workers->element_count; ++index) {
            const auto* item =
                parsed_node_inside(nodes, MonitoringDefinitions::page_worker, *workers, index);
            const auto worker =
                item ? unsigned_inside(body, nodes, MonitoringDefinitions::page_worker_id, *item)
                     : std::nullopt;
            const auto state_value =
                item ? monitoring_signal(token(MonitoringDefinitions::page_worker_state, *item))
                     : std::nullopt;
            const auto minor =
                item ? option_u64(MonitoringDefinitions::page_minor, *item) : std::nullopt;
            const auto major =
                item ? option_u64(MonitoringDefinitions::page_major, *item) : std::nullopt;
            if (!worker || *worker > std::numeric_limits<std::uint32_t>::max() || !state_value ||
                !minor || !major || index >= authenticated_workers.size() ||
                core::WorkerId{static_cast<std::uint32_t>(*worker)} != authenticated_workers[index])
                return reject(ActivePipelineFailureKind::ledger_validation,
                              MonitoringDefinitions::page_worker);
            values.push_back(monitoring::detail::PairwisePreparedCaptureStorageAccess::page_worker(
                authenticated_workers[index], *state_value,
                *minor ? std::optional{monitoring::PageFaultCount::from_events(**minor)}
                       : std::nullopt,
                *major ? std::optional{monitoring::PageFaultCount::from_events(**major)}
                       : std::nullopt));
        }
        result.evidence_.page_.emplace(
            monitoring::detail::PairwisePreparedCaptureStorageAccess::page_evidence(
                *state, std::move(values)));
    }

    const auto* context_option = parsed_node_inside(nodes, MonitoringDefinitions::context_option, *evidence);
    const auto* context = context_option && context_option->present
        ? parsed_node_inside(nodes, MonitoringDefinitions::context, *context_option) : nullptr;
    if (!coherent_presence(context_option, context, *states[1]))
        return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::context);
    if (context) {
        const auto state = monitoring_signal(
            token(required_definition("scalar__context_evidence__state"), *context));
        const auto* workers =
            parsed_node_inside(nodes, MonitoringDefinitions::context_workers, *context);
        if (!state || *state != *states[1] || !workers ||
            workers->element_count > authenticated_workers.size())
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::context);
        auto& values = storage_->context_evidence;
        for (std::uint64_t index{}; index < workers->element_count; ++index) {
            const auto* item =
                parsed_node_inside(nodes, MonitoringDefinitions::context_worker, *workers, index);
            const auto worker =
                item ? unsigned_inside(body, nodes, MonitoringDefinitions::context_worker_id, *item)
                     : std::nullopt;
            const auto state_value =
                item ? monitoring_signal(token(MonitoringDefinitions::context_worker_state, *item))
                     : std::nullopt;
            const auto voluntary =
                item ? option_u64(MonitoringDefinitions::voluntary, *item) : std::nullopt;
            const auto involuntary =
                item ? option_u64(MonitoringDefinitions::involuntary, *item) : std::nullopt;
            if (!worker || *worker > std::numeric_limits<std::uint32_t>::max() || !state_value ||
                !voluntary || !involuntary || index >= authenticated_workers.size() ||
                core::WorkerId{static_cast<std::uint32_t>(*worker)} != authenticated_workers[index])
                return reject(ActivePipelineFailureKind::ledger_validation,
                              MonitoringDefinitions::context_worker);
            values.push_back(
                monitoring::detail::PairwisePreparedCaptureStorageAccess::context_worker(
                    authenticated_workers[index], *state_value,
                    *voluntary
                        ? std::optional{monitoring::ContextSwitchCount::from_events(**voluntary)}
                        : std::nullopt,
                    *involuntary
                        ? std::optional{monitoring::ContextSwitchCount::from_events(**involuntary)}
                        : std::nullopt));
        }
        result.evidence_.context_.emplace(
            monitoring::detail::PairwisePreparedCaptureStorageAccess::context_evidence(
                *state, std::move(values)));
    }

    const auto* temperature_option = parsed_node_inside(nodes, MonitoringDefinitions::temperature_option, *evidence);
    const auto* temperature = temperature_option && temperature_option->present
        ? parsed_node_inside(nodes, MonitoringDefinitions::temperature, *temperature_option) : nullptr;
    if (!coherent_presence(temperature_option, temperature, *states[2]))
        return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::temperature);
    if (temperature) {
        const auto state = monitoring_signal(
            token(required_definition("scalar__temperature_evidence__state"), *temperature));
        const auto sensor =
            token(required_definition("scalar__temperature_evidence__sensor_kind"), *temperature);
        const auto count =
            unsigned_inside(body, nodes, MonitoringDefinitions::temperature_count, *temperature);
        const auto interval =
            unsigned_inside(body, nodes, MonitoringDefinitions::duration, *temperature);
        const auto complete = token(MonitoringDefinitions::temperature_complete, *temperature);
        const auto* min_option =
            parsed_node_inside(nodes, MonitoringDefinitions::temperature_min, *temperature);
        const auto* max_option =
            parsed_node_inside(nodes, MonitoringDefinitions::temperature_max, *temperature);
        const auto range = option_u64(MonitoringDefinitions::temperature_range, *temperature);
        std::optional<std::int64_t> minimum, maximum;
        if (min_option && min_option->present)
            minimum =
                signed_inside(body, nodes, MonitoringDefinitions::temperature_value, *min_option);
        if (max_option && max_option->present)
            maximum =
                signed_inside(body, nodes, MonitoringDefinitions::temperature_value, *max_option);
        if (!state || *state != *states[2] || sensor != "cpu_control" || !count || !interval ||
            !complete || !range || !min_option || !max_option ||
            min_option->present != minimum.has_value() ||
            max_option->present != maximum.has_value() ||
            minimum.has_value() != maximum.has_value() ||
            (minimum && (*maximum < *minimum || !*range ||
                         **range != static_cast<std::uint64_t>(*maximum - *minimum))) ||
            ((*count == 0U) != !minimum))
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::temperature);
        const auto completeness =
            *complete == "complete"
                ? std::optional{monitoring::TemperatureCaptureCompleteness::complete}
            : *complete == "incomplete"
                ? std::optional{monitoring::TemperatureCaptureCompleteness::incomplete}
                : std::nullopt;
        if (!completeness || *count > std::numeric_limits<std::size_t>::max())
            return reject(ActivePipelineFailureKind::typed_factory,
                          MonitoringDefinitions::temperature);
        result.evidence_.temperature_.emplace(
            monitoring::detail::PairwisePreparedCaptureStorageAccess::temperature_evidence(
                monitoring::TemperatureSensorKind::cpu_control, *state,
                minimum ? std::optional{monitoring::TemperatureMilliCelsius::from_millicelsius(
                              *minimum)}
                        : std::nullopt,
                maximum ? std::optional{monitoring::TemperatureMilliCelsius::from_millicelsius(
                              *maximum)}
                        : std::nullopt,
                *range ? std::optional{monitoring::TemperatureRangeMilliCelsius::from_millicelsius(
                             **range)}
                       : std::nullopt,
                static_cast<std::size_t>(*count), core::Duration::from_base_units(*interval),
                *completeness));
    }

    std::size_t cpu_pool_index{};
    const auto make_domain =
        [&](const PreparedParsedNode& parent) -> std::optional<monitoring::FrequencyDomainId> {
        const auto* domain = parsed_node_inside(nodes, MonitoringDefinitions::domain, parent);
        const auto* cpus =
            domain ? parsed_node_inside(nodes, MonitoringDefinitions::cpus, *domain) : nullptr;
        if (!cpus || cpu_pool_index >= storage_->cpu_pools.size())
            return std::nullopt;
        auto& values = storage_->cpu_pools[cpu_pool_index++];
        if (cpus->element_count > values.capacity())
            return std::nullopt;
        for (std::uint64_t index{}; index < cpus->element_count; ++index) {
            const auto cpu = unsigned_inside(body, nodes, MonitoringDefinitions::cpu, *cpus, index);
            if (!cpu || *cpu > std::numeric_limits<std::uint32_t>::max())
                return std::nullopt;
            values.emplace_back(static_cast<std::uint32_t>(*cpu));
        }
        if (values.empty() || !std::ranges::is_sorted(values) ||
            std::ranges::adjacent_find(values) != values.end())
            return std::nullopt;
        auto made = monitoring::FrequencyDomainId::create(std::move(values));
        return made ? std::optional{std::move(*made)} : std::nullopt;
    };
    const auto* frequency_option = parsed_node_inside(nodes, MonitoringDefinitions::frequency_option, *evidence);
    const auto* frequency = frequency_option && frequency_option->present
        ? parsed_node_inside(nodes, MonitoringDefinitions::frequency, *frequency_option) : nullptr;
    if (!coherent_presence(frequency_option, frequency, *states[3]))
        return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::frequency);
    if (frequency) {
        const auto state = monitoring_signal(
            token(required_definition("scalar__frequency_evidence__state"), *frequency));
        const auto interval =
            unsigned_inside(body, nodes, MonitoringDefinitions::duration, *frequency);
        const auto complete = token(MonitoringDefinitions::frequency_complete, *frequency);
        const auto* domains =
            parsed_node_inside(nodes, MonitoringDefinitions::frequency_domains, *frequency);
        const auto completeness =
            complete == "complete"
                ? std::optional{monitoring::FrequencyCaptureCompleteness::complete}
            : complete == "incomplete"
                ? std::optional{monitoring::FrequencyCaptureCompleteness::incomplete}
                : std::nullopt;
        if (!state || *state != *states[3] || !interval || !completeness || !domains ||
            domains->element_count > storage_->frequency_results.size())
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::frequency);
        auto& values = storage_->frequency_evidence;
        for (std::uint64_t index{}; index < domains->element_count; ++index) {
            const auto* item =
                parsed_node_inside(nodes, MonitoringDefinitions::frequency_domain, *domains, index);
            auto domain = item ? make_domain(*item) : std::nullopt;
            const auto kind =
                item
                    ? monitoring_frequency_kind(token(MonitoringDefinitions::frequency_kind, *item))
                    : std::nullopt;
            const auto domain_state =
                item
                    ? monitoring_signal(token(MonitoringDefinitions::frequency_domain_state, *item))
                    : std::nullopt;
            const auto count =
                item ? unsigned_inside(body, nodes, MonitoringDefinitions::frequency_count, *item)
                     : std::nullopt;
            const auto minimum =
                item ? option_unsigned(MonitoringDefinitions::frequency_min,
                                       MonitoringDefinitions::frequency_value, *item)
                     : std::nullopt;
            const auto maximum =
                item ? option_unsigned(MonitoringDefinitions::frequency_max,
                                       MonitoringDefinitions::frequency_value, *item)
                     : std::nullopt;
            const auto range = item ? option_unsigned(MonitoringDefinitions::frequency_range,
                                                      MonitoringDefinitions::frequency_value, *item)
                                    : std::nullopt;
            const auto* variation_option =
                item ? parsed_node_inside(nodes, MonitoringDefinitions::frequency_variation, *item)
                     : nullptr;
            std::optional<monitoring::FrequencyVariationRatio> variation;
            if (variation_option && variation_option->present) {
                const auto numerator = unsigned_inside(
                    body, nodes, required_definition("scalar__frequency_variation__numerator"),
                    *variation_option);
                const auto denominator = unsigned_inside(
                    body, nodes, required_definition("scalar__frequency_variation__denominator"),
                    *variation_option);
                if (!numerator || !denominator)
                    return reject(ActivePipelineFailureKind::typed_factory,
                                  MonitoringDefinitions::frequency_variation);
                auto made = monitoring::FrequencyVariationRatio::create(*numerator, *denominator);
                if (!made)
                    return reject(ActivePipelineFailureKind::typed_factory,
                                  MonitoringDefinitions::frequency_variation);
                variation = *made;
            }
            if (!item || !domain || !kind || !domain_state || !count ||
                *count > std::numeric_limits<std::size_t>::max() || !minimum || !maximum ||
                !range || !variation_option || variation_option->present != variation.has_value() ||
                minimum->has_value() != maximum->has_value() ||
                (*minimum &&
                 (**maximum < **minimum || !*range || **range != **maximum - **minimum)) ||
                ((*count == 0U) != !minimum->has_value()))
                return reject(ActivePipelineFailureKind::ledger_validation,
                              MonitoringDefinitions::frequency_domain);
            values.push_back(monitoring::FrequencyDomainEvidence::create(
                std::move(*domain), *kind, *domain_state,
                *minimum ? std::optional{monitoring::FrequencyHz::from_hz(**minimum)}
                         : std::nullopt,
                *maximum ? std::optional{monitoring::FrequencyHz::from_hz(**maximum)}
                         : std::nullopt,
                *range ? std::optional{monitoring::FrequencyRangeHz::from_hz(**range)}
                       : std::nullopt,
                variation, static_cast<std::size_t>(*count)));
        }
        result.evidence_.frequency_.emplace(
            monitoring::detail::PairwisePreparedCaptureStorageAccess::frequency_evidence(
                *state, std::move(values), core::Duration::from_base_units(*interval),
                *completeness));
    }

    std::size_t implementation_index{};
    std::uint32_t common_failure_definition{MonitoringDefinitions::raw};
    const auto common = [&](const PreparedParsedNode& item, std::string_view expected_channel,
                            std::string_view expected_phase)
        -> std::optional<std::tuple<std::string, platform_linux::RawMonotonicTimestamp,
                                    platform_linux::RawMonotonicTimestamp, bool>> {
        const auto channel = token(MonitoringDefinitions::raw_channel, item);
        const auto phase = token(MonitoringDefinitions::raw_phase, item);
        const auto status = token(MonitoringDefinitions::raw_status, item);
        // Canonical object member order places collection_end before
        // collection_start; both fields share the timestamp definition.
        const auto end = unsigned_inside(body, nodes, MonitoringDefinitions::timestamp, item, 0U);
        const auto start = unsigned_inside(body, nodes, MonitoringDefinitions::timestamp, item, 1U);
        const auto duration = unsigned_inside(body, nodes, MonitoringDefinitions::duration, item);
        const auto* impl_node =
            parsed_node_inside(nodes, MonitoringDefinitions::raw_implementation, item);
        if (channel != expected_channel) {
            common_failure_definition = MonitoringDefinitions::raw_channel;
            return std::nullopt;
        }
        if (phase != expected_phase) {
            common_failure_definition = MonitoringDefinitions::raw_phase;
            return std::nullopt;
        }
        if (status != "available" && status != "unavailable") {
            common_failure_definition = MonitoringDefinitions::raw_status;
            return std::nullopt;
        }
        if (!start || !end || *end < *start) {
            common_failure_definition = MonitoringDefinitions::timestamp;
            return std::nullopt;
        }
        if (!duration || *duration != *end - *start) {
            common_failure_definition = MonitoringDefinitions::duration;
            return std::nullopt;
        }
        if (!impl_node || implementation_index >= storage_->implementation_ids.size()) {
            common_failure_definition = MonitoringDefinitions::raw_implementation;
            return std::nullopt;
        }
        auto& implementation = storage_->implementation_ids[implementation_index++];
        const auto view = lexical(body, *impl_node);
        if (!view || !decode_string(*view, implementation.capacity(), implementation) ||
            implementation.empty()) {
            common_failure_definition = MonitoringDefinitions::raw_implementation;
            return std::nullopt;
        }
        return std::tuple{std::move(implementation),
                          platform_linux::RawMonotonicTimestamp::from_nanoseconds(*start),
                          platform_linux::RawMonotonicTimestamp::from_nanoseconds(*end),
                          status == "available"};
    };
    const auto make_page = [&](const PreparedParsedNode& item, std::string_view phase)
        -> std::optional<monitoring::MonitoringRawCaptureEvidence::PageAcquisition> {
        auto metadata = common(item, "page_faults", phase);
        const auto* payload_option =
            parsed_node_inside(nodes, MonitoringDefinitions::page_payload_option, item);
        if (!metadata || !payload_option || payload_option->present != std::get<3>(*metadata))
            return std::nullopt;
        std::optional<monitoring::MonitoringPayloadResult<monitoring::PageFaultSnapshot>> payload;
        if (payload_option->present) {
            const auto* value =
                parsed_node_inside(nodes, MonitoringDefinitions::page_payload, *payload_option);
            const auto worker =
                value ? unsigned_inside(body, nodes, MonitoringDefinitions::payload_worker, *value)
                      : std::nullopt;
            const auto minor =
                value ? unsigned_inside(body, nodes, MonitoringDefinitions::payload_minor, *value)
                      : std::nullopt;
            const auto major =
                value ? unsigned_inside(body, nodes, MonitoringDefinitions::payload_major, *value)
                      : std::nullopt;
            if (!worker || *worker > std::numeric_limits<std::uint32_t>::max() || !minor || !major)
                return std::nullopt;
            payload.emplace(
                monitoring::MonitoringPayloadResult<monitoring::PageFaultSnapshot>::available(
                    monitoring::PageFaultSnapshot::create(
                        core::WorkerId{static_cast<std::uint32_t>(*worker)},
                        monitoring::PageFaultCount::from_events(*minor),
                        monitoring::PageFaultCount::from_events(*major))));
        }
        if (!payload)
            payload.emplace(
                monitoring::MonitoringPayloadResult<monitoring::PageFaultSnapshot>::unavailable());
        return monitoring::detail::PairwisePreparedCaptureStorageAccess::acquisition(
            monitoring::MonitoringChannel::page_faults,
            monitoring::MonitoringImplementationId::create(std::move(std::get<0>(*metadata))),
            phase == "before_observation" ? monitoring::MonitoringSamplePhase::before_observation
                                          : monitoring::MonitoringSamplePhase::after_observation,
            std::get<1>(*metadata), std::get<2>(*metadata), std::move(*payload));
    };
    const auto fill_pages = [&](std::uint32_t definition, std::string_view phase,
                                auto& destination) -> bool {
        const auto* array = parsed_node_inside(nodes, definition, *raw);
        if (!array || array->element_count > destination.capacity())
            return false;
        for (std::uint64_t index{}; index < array->element_count; ++index) {
            const auto* item =
                parsed_node_inside(nodes, MonitoringDefinitions::raw_page, *array, index);
            auto made = item ? make_page(*item, phase) : std::nullopt;
            if (!made)
                return false;
            destination.push_back(std::move(*made));
        }
        return true;
    };
    if (!fill_pages(MonitoringDefinitions::page_before, "before_observation",
                    result.raw_.page_before_) ||
        !fill_pages(MonitoringDefinitions::page_after, "after_observation",
                    result.raw_.page_after_) ||
        result.raw_.page_before_.size() != result.raw_.page_after_.size())
        return reject(ActivePipelineFailureKind::ledger_validation,
                      MonitoringDefinitions::raw_page);

    const auto* context_array =
        parsed_node_inside(nodes, MonitoringDefinitions::context_samples, *raw);
    if (!context_array || context_array->element_count > result.raw_.context_.capacity() ||
        context_array->element_count > storage_->context_event_pools.size())
        return reject(ActivePipelineFailureKind::capacity_invariant,
                      MonitoringDefinitions::context_samples);
    for (std::uint64_t index{}; index < context_array->element_count; ++index) {
        const auto* item =
            parsed_node_inside(nodes, MonitoringDefinitions::raw_context, *context_array, index);
        auto metadata =
            item ? common(*item, "context_switches", "during_observation") : std::nullopt;
        const auto* payload_option =
            item ? parsed_node_inside(nodes, MonitoringDefinitions::context_payload_option, *item)
                 : nullptr;
        if (!metadata || !payload_option || payload_option->present != std::get<3>(*metadata))
            return reject(ActivePipelineFailureKind::ledger_validation,
                          metadata ? MonitoringDefinitions::context_payload_option
                                   : common_failure_definition);
        std::optional<monitoring::MonitoringPayloadResult<monitoring::ContextSwitchEventBatch>>
            payload;
        if (payload_option->present) {
            const auto* value =
                parsed_node_inside(nodes, MonitoringDefinitions::context_payload, *payload_option);
            const auto worker =
                value ? unsigned_inside(body, nodes, MonitoringDefinitions::context_payload_worker,
                                        *value)
                      : std::nullopt;
            const auto complete =
                value ? token(MonitoringDefinitions::context_payload_complete, *value)
                      : std::nullopt;
            const auto* events =
                value ? parsed_node_inside(nodes, MonitoringDefinitions::context_events, *value)
                      : nullptr;
            if (!worker || *worker > std::numeric_limits<std::uint32_t>::max() || !complete ||
                !events || events->element_count > storage_->context_event_pools[index].capacity())
                return reject(ActivePipelineFailureKind::typed_factory,
                              MonitoringDefinitions::context_payload);
            auto& event_values = storage_->context_event_pools[index];
            for (std::uint64_t event_index{}; event_index < events->element_count; ++event_index) {
                const auto* event = parsed_node_inside(nodes, MonitoringDefinitions::context_event,
                                                       *events, event_index);
                const auto event_worker =
                    event
                        ? unsigned_inside(body, nodes, MonitoringDefinitions::event_worker, *event)
                        : std::nullopt;
                const auto timestamp =
                    event ? unsigned_inside(body, nodes, MonitoringDefinitions::event_timestamp,
                                            *event)
                          : std::nullopt;
                const auto kind =
                    event ? token(MonitoringDefinitions::event_kind, *event) : std::nullopt;
                if (!event_worker || *event_worker != *worker || !timestamp ||
                    *timestamp > std::get<2>(*metadata).nanoseconds() ||
                    (kind != "voluntary" && kind != "involuntary"))
                    return reject(ActivePipelineFailureKind::ledger_validation,
                                  MonitoringDefinitions::context_event);
                // MON-003: collection times bracket the drain, not its events.
                // Preserve ring order; the classifier filters event times by Observation.
                event_values.push_back(monitoring::ContextSwitchEvent::create(
                    core::WorkerId{static_cast<std::uint32_t>(*worker)},
                    platform_linux::RawMonotonicTimestamp::from_nanoseconds(*timestamp),
                    kind == "voluntary" ? monitoring::ContextSwitchKind::voluntary
                                        : monitoring::ContextSwitchKind::involuntary));
            }
            const auto completeness =
                complete == "complete"
                    ? std::optional{monitoring::ContextSwitchCaptureCompleteness::complete}
                : complete == "incomplete"
                    ? std::optional{monitoring::ContextSwitchCaptureCompleteness::incomplete}
                    : std::nullopt;
            if (!completeness)
                return reject(ActivePipelineFailureKind::registry_domain,
                              MonitoringDefinitions::context_payload_complete);
            payload.emplace(
                monitoring::MonitoringPayloadResult<monitoring::ContextSwitchEventBatch>::available(
                    monitoring::ContextSwitchEventBatch::create(
                        core::WorkerId{static_cast<std::uint32_t>(*worker)}, *completeness,
                        std::move(event_values))));
        }
        if (!payload)
            payload.emplace(monitoring::MonitoringPayloadResult<
                            monitoring::ContextSwitchEventBatch>::unavailable());
        result.raw_.context_.push_back(
            monitoring::detail::PairwisePreparedCaptureStorageAccess::acquisition(
                monitoring::MonitoringChannel::context_switches,
                monitoring::MonitoringImplementationId::create(std::move(std::get<0>(*metadata))),
                monitoring::MonitoringSamplePhase::during_observation, std::get<1>(*metadata),
                std::get<2>(*metadata), std::move(*payload)));
    }
    result.raw_.context_count_ = result.raw_.context_.size();

    const auto* temperature_array =
        parsed_node_inside(nodes, MonitoringDefinitions::temperature_samples, *raw);
    if (!temperature_array ||
        temperature_array->element_count > result.raw_.temperature_.capacity())
        return reject(ActivePipelineFailureKind::capacity_invariant,
                      MonitoringDefinitions::temperature_samples);
    for (std::uint64_t index{}; index < temperature_array->element_count; ++index) {
        const auto* item = parsed_node_inside(nodes, MonitoringDefinitions::raw_temperature,
                                              *temperature_array, index);
        auto metadata = item ? common(*item, "temperature", "during_observation") : std::nullopt;
        const auto* payload_option =
            item ? parsed_node_inside(nodes, MonitoringDefinitions::temperature_payload_option,
                                      *item)
                 : nullptr;
        if (!metadata || !payload_option || payload_option->present != std::get<3>(*metadata))
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::raw_temperature);
        std::optional<monitoring::MonitoringPayloadResult<monitoring::TemperatureSnapshot>> payload;
        if (payload_option->present) {
            const auto* value = parsed_node_inside(
                nodes, MonitoringDefinitions::temperature_payload, *payload_option);
            const auto sensor =
                value
                    ? token(required_definition("scalar__temperature_payload__sensor_kind"), *value)
                    : std::nullopt;
            const auto amount =
                value ? signed_inside(body, nodes, MonitoringDefinitions::temperature_value, *value)
                      : std::nullopt;
            if (sensor != "cpu_control" || !amount)
                return reject(ActivePipelineFailureKind::typed_factory,
                              MonitoringDefinitions::temperature_payload);
            payload.emplace(
                monitoring::MonitoringPayloadResult<monitoring::TemperatureSnapshot>::available(
                    monitoring::TemperatureSnapshot::create(
                        monitoring::TemperatureSensorKind::cpu_control,
                        monitoring::TemperatureMilliCelsius::from_millicelsius(*amount))));
        }
        if (!payload)
            payload.emplace(monitoring::MonitoringPayloadResult<
                            monitoring::TemperatureSnapshot>::unavailable());
        result.raw_.temperature_.push_back(
            monitoring::detail::PairwisePreparedCaptureStorageAccess::acquisition(
                monitoring::MonitoringChannel::temperature,
                monitoring::MonitoringImplementationId::create(std::move(std::get<0>(*metadata))),
                monitoring::MonitoringSamplePhase::during_observation, std::get<1>(*metadata),
                std::get<2>(*metadata), std::move(*payload)));
    }

    const auto* groups = parsed_node_inside(nodes, MonitoringDefinitions::frequency_groups, *raw);
    if (!groups || groups->element_count > storage_->frequency_sample_pools.size())
        return reject(ActivePipelineFailureKind::capacity_invariant,
                      MonitoringDefinitions::frequency_groups);
    for (std::uint64_t group_index{}; group_index < groups->element_count; ++group_index) {
        const auto* group =
            parsed_node_inside(nodes, MonitoringDefinitions::frequency_group, *groups, group_index);
        auto group_domain = group ? make_domain(*group) : std::nullopt;
        const auto* samples =
            group ? parsed_node_inside(nodes, MonitoringDefinitions::frequency_samples, *group)
                  : nullptr;
        if (!group_domain || !samples ||
            samples->element_count > storage_->frequency_sample_pools[group_index].capacity())
            return reject(ActivePipelineFailureKind::typed_factory,
                          MonitoringDefinitions::frequency_group);
        auto& sample_values = storage_->frequency_sample_pools[group_index];
        for (std::uint64_t sample_index{}; sample_index < samples->element_count; ++sample_index) {
            const auto* item = parsed_node_inside(nodes, MonitoringDefinitions::raw_frequency,
                                                  *samples, sample_index);
            auto metadata = item ? common(*item, "frequency", "during_observation") : std::nullopt;
            const auto* payload_option =
                item ? parsed_node_inside(nodes, MonitoringDefinitions::frequency_payload_option,
                                          *item)
                     : nullptr;
            if (!metadata || !payload_option || payload_option->present != std::get<3>(*metadata))
                return reject(ActivePipelineFailureKind::ledger_validation,
                              MonitoringDefinitions::raw_frequency);
            std::optional<monitoring::MonitoringPayloadResult<monitoring::FrequencySnapshot>>
                payload;
            if (payload_option->present) {
                const auto* value = parsed_node_inside(
                    nodes, MonitoringDefinitions::frequency_payload, *payload_option);
                auto domain = value ? make_domain(*value) : std::nullopt;
                const auto kind =
                    value ? monitoring_frequency_kind(
                                token(MonitoringDefinitions::frequency_payload_kind, *value))
                          : std::nullopt;
                const auto hz =
                    value ? unsigned_inside(body, nodes, MonitoringDefinitions::frequency_value,
                                            *value)
                          : std::nullopt;
                if (!domain || !(*domain == *group_domain) || !kind || !hz)
                    return reject(ActivePipelineFailureKind::ledger_validation,
                                  MonitoringDefinitions::frequency_payload);
                payload.emplace(
                    monitoring::MonitoringPayloadResult<monitoring::FrequencySnapshot>::available(
                        monitoring::FrequencySnapshot::create(
                            std::move(*domain), *kind, monitoring::FrequencyHz::from_hz(*hz))));
            }
            if (!payload)
                payload.emplace(monitoring::MonitoringPayloadResult<
                                monitoring::FrequencySnapshot>::unavailable());
            sample_values.push_back(
                monitoring::detail::PairwisePreparedCaptureStorageAccess::acquisition(
                    monitoring::MonitoringChannel::frequency,
                    monitoring::MonitoringImplementationId::create(
                        std::move(std::get<0>(*metadata))),
                    monitoring::MonitoringSamplePhase::during_observation, std::get<1>(*metadata),
                    std::get<2>(*metadata), std::move(*payload)));
        }
        result.raw_.frequency_storage_.push_back(std::move(sample_values));
        result.raw_.frequency_views_.push_back(monitoring::FrequencyDomainAcquisitions{
            std::move(*group_domain), std::span<const monitoring::FrequencyAcquisition>{
                                          result.raw_.frequency_storage_.back()}});
    }

    const auto* metadata_array =
        parsed_node_inside(nodes, MonitoringDefinitions::frequency_metadata, *raw);
    if (!metadata_array ||
        metadata_array->element_count > result.raw_.frequency_metadata_.capacity() ||
        metadata_array->element_count > storage_->drivers.size() ||
        metadata_array->element_count > storage_->governors.size())
        return reject(ActivePipelineFailureKind::capacity_invariant,
                      MonitoringDefinitions::frequency_metadata);
    for (std::uint64_t index{}; index < metadata_array->element_count; ++index) {
        const auto* item =
            parsed_node_inside(nodes, MonitoringDefinitions::metadata, *metadata_array, index);
        auto domain = item ? make_domain(*item) : std::nullopt;
        const auto kind =
            item ? monitoring_frequency_kind(token(MonitoringDefinitions::metadata_kind, *item))
                 : std::nullopt;
        const auto minimum =
            item ? unsigned_inside(body, nodes, MonitoringDefinitions::scaling_min, *item)
                 : std::nullopt;
        const auto maximum =
            item ? unsigned_inside(body, nodes, MonitoringDefinitions::scaling_max, *item)
                 : std::nullopt;
        const auto* driver_node =
            item ? parsed_node_inside(nodes, MonitoringDefinitions::driver, *item) : nullptr;
        const auto* governor_node =
            item ? parsed_node_inside(nodes, MonitoringDefinitions::governor, *item) : nullptr;
        auto& driver = storage_->drivers[index];
        auto& governor = storage_->governors[index];
        const auto driver_view = driver_node ? lexical(body, *driver_node) : std::nullopt;
        const auto governor_view = governor_node ? lexical(body, *governor_node) : std::nullopt;
        if (!domain || !kind || !minimum || !maximum || *minimum > *maximum || !driver_view ||
            !governor_view || !decode_string(*driver_view, driver.capacity(), driver) ||
            !decode_string(*governor_view, governor.capacity(), governor))
            return reject(ActivePipelineFailureKind::typed_factory,
                          MonitoringDefinitions::metadata);
        result.raw_.frequency_metadata_.push_back(
            monitoring::detail::PairwisePreparedCaptureStorageAccess::frequency_metadata(
                std::move(*domain), *kind, monitoring::FrequencyDriverId::create(std::move(driver)),
                monitoring::FrequencyGovernorId::create(std::move(governor)),
                monitoring::FrequencyHz::from_hz(*minimum),
                monitoring::FrequencyHz::from_hz(*maximum)));
    }
    if (result.raw_.frequency_views_.size() != result.raw_.frequency_metadata_.size())
        return reject(ActivePipelineFailureKind::ledger_validation,
                      MonitoringDefinitions::frequency_metadata);
    for (std::size_t index{}; index < result.raw_.frequency_views_.size(); ++index)
        if (!(result.raw_.frequency_views_[index].domain_id ==
              result.raw_.frequency_metadata_[index].domain_id()))
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::frequency_metadata);

    std::array<std::uint64_t*, 7> activity_destinations{
        &result.activity_.scheduled_, &result.activity_.completed_, &result.activity_.missed_,
        &result.activity_.wakeups_,   &result.activity_.drains_,    &result.activity_.temperature_,
        &result.activity_.frequency_};
    for (std::size_t index{}; index < activity_destinations.size(); ++index) {
        const auto value =
            unsigned_inside(body, nodes, MonitoringDefinitions::activity_values[index], *activity);
        if (!value)
            return reject(ActivePipelineFailureKind::registry_domain,
                          MonitoringDefinitions::activity_values[index]);
        *activity_destinations[index] = *value;
    }
    if (result.activity_.completed_ > result.activity_.scheduled_ ||
        result.activity_.missed_ > result.activity_.scheduled_ - result.activity_.completed_)
        return reject(ActivePipelineFailureKind::ledger_validation,
                      MonitoringDefinitions::activity);
    if (storage_->actual_frequency_domains) {
        std::uint64_t frequency_attempts{};
        std::optional<std::size_t> frequency_group_size;
        for (const auto& group : result.raw_.frequency_views_) {
            if ((frequency_group_size && *frequency_group_size != group.acquisitions.size()) ||
                group.acquisitions.size() > std::numeric_limits<std::uint64_t>::max() - frequency_attempts)
                return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::activity);
            frequency_group_size = group.acquisitions.size();
            frequency_attempts += group.acquisitions.size();
        }
        if (result.activity_.temperature_ != result.raw_.temperature_.size() ||
            result.activity_.frequency_ != frequency_attempts ||
            result.activity_.drains_ < result.raw_.context_.size() ||
            result.raw_.temperature_.size() > result.activity_.scheduled_)
            return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::activity);
    }

    if ((result.evidence_.page_ &&
         (result.raw_.page_before_.size() != authenticated_workers.size() ||
          result.raw_.page_after_.size() != authenticated_workers.size())) ||
        (!result.evidence_.page_ &&
         (!result.raw_.page_before_.empty() || !result.raw_.page_after_.empty())) ||
        (!result.evidence_.context_ && !result.raw_.context_.empty()) ||
        (!result.evidence_.temperature_ && !result.raw_.temperature_.empty()) ||
        (!result.evidence_.frequency_ && (!result.raw_.frequency_views_.empty() || !result.raw_.frequency_metadata_.empty())))
        return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::raw);

    if ((!result.evidence_.context_ && result.activity_.drains_ != 0U) ||
        (!result.evidence_.temperature_ && result.activity_.temperature_ != 0U) ||
        (!result.evidence_.frequency_ && result.activity_.frequency_ != 0U))
        return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::activity);

    if (result.evidence_.page_) {
        storage_->page_pairs.clear();
        for (std::size_t index{}; index < authenticated_workers.size(); ++index)
            storage_->page_pairs.emplace_back(authenticated_workers[index],
                                              result.raw_.page_before_[index],
                                              result.raw_.page_after_[index]);
        PageReplay replay{result.evidence_.page_->workers()};
        const auto sink = monitoring::detail::PageFaultEvidenceSink{&replay, &PageReplay::append};
        const auto classified = monitoring::detail::classify_page_faults_bounded(
            observation_start, observation_end, storage_->page_pairs, sink);
        if (!classified || *classified != result.evidence_.page_->state() || !replay.equal ||
            replay.index != result.evidence_.page_->workers().size())
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::page);
    }
    if (result.evidence_.context_) {
        if (authenticated_workers.empty() ||
            result.raw_.context_.size() % authenticated_workers.size() != 0U ||
            authenticated_workers.size() > storage_->context_view_ids.size())
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::context);
        const auto stride = result.raw_.context_.size() / authenticated_workers.size();
        storage_->context_views.clear();
        for (std::size_t index{}; index < authenticated_workers.size(); ++index)
            storage_->context_views.emplace_back(
                authenticated_workers[index], std::move(storage_->context_view_ids[index]),
                observation_start, observation_end,
                monitoring::ContextSwitchCaptureCompleteness::complete,
                std::span<const monitoring::MonitoringRawCaptureEvidence::ContextAcquisition>{
                    result.raw_.context_}
                    .subspan(index * stride, stride));
        ContextReplay replay{result.evidence_.context_->workers()};
        const auto classified = monitoring::detail::classify_context_switches_bounded(
            observation_start, observation_end, authenticated_workers, storage_->context_views,
            monitoring::detail::ContextSwitchEvidenceSink{&replay, &ContextReplay::append});
        if (!classified || *classified != result.evidence_.context_->state() || !replay.equal ||
            replay.index != result.evidence_.context_->workers().size())
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::context);
    }
    const auto inside_points = [&](auto acquisitions, auto association)
        -> std::optional<decltype(acquisitions)> {
        std::size_t first{}, count{};
        std::optional<platform_linux::RawMonotonicTimestamp> previous;
        for (std::size_t index{}; index < acquisitions.size(); ++index) {
            const auto& item = acquisitions[index];
            if (item.collection_end() < item.collection_start() ||
                (previous && item.collection_start() < *previous) || !association(item))
                return std::nullopt;
            previous = item.collection_start();
            if (item.collection_start() >= observation_start &&
                item.collection_end() <= observation_end) {
                if (count == 0U) first = index;
                else if (index - first != count) return std::nullopt;
                ++count;
            }
        }
        return acquisitions.subspan(first, count);
    };
    if (result.evidence_.temperature_) {
        const auto selected = inside_points(
            std::span<const monitoring::MonitoringRawCaptureEvidence::TemperatureAcquisition>{result.raw_.temperature_},
            [](const auto& item) {
                return item.implementation_id().value() == "linux-hwmon-k10temp-tctl" &&
                    (!item.payload() || item.payload()->sensor_kind() == monitoring::TemperatureSensorKind::cpu_control);
            });
        if (!selected) return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::temperature);
        std::ranges::fill(storage_->temperature_scratch, nullptr);
        const auto replay = monitoring::detail::classify_temperature_bounded(
            observation_start, observation_end, result.evidence_.temperature_->sampling_interval(),
            result.evidence_.temperature_->capture_completeness(), *selected,
            storage_->temperature_scratch);
        if (!replay || replay->state != result.evidence_.temperature_->state() ||
            replay->minimum != result.evidence_.temperature_->observed_minimum() ||
            replay->maximum != result.evidence_.temperature_->observed_maximum() ||
            replay->range != result.evidence_.temperature_->observed_range() ||
            replay->count != result.evidence_.temperature_->sample_count())
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::temperature);
    }
    if (result.evidence_.frequency_) {
        if (result.raw_.frequency_views_.size() != result.evidence_.frequency_->domains().size() ||
            authenticated_cpus.size() == 0U || result.raw_.frequency_views_.empty() ||
            result.raw_.frequency_views_.size() > authenticated_workers.size())
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::frequency);
        std::size_t total_cpus{};
        for (std::size_t index{}; index < result.raw_.frequency_views_.size(); ++index) {
            const auto& view = result.raw_.frequency_views_[index];
            const auto& classified = result.evidence_.frequency_->domains()[index];
            if (!(view.domain_id == classified.domain_id()) ||
                classified.source_kind() != result.raw_.frequency_metadata_[index].source_kind())
                return reject(ActivePipelineFailureKind::ledger_validation,
                              MonitoringDefinitions::frequency);
            if (storage_->actual_frequency_domains && index != 0U &&
                !(result.raw_.frequency_views_[index - 1U].domain_id < view.domain_id))
                return reject(ActivePipelineFailureKind::ledger_validation,
                              MonitoringDefinitions::frequency);
            for (const auto cpu : view.domain_id.cpus()) {
                bool repeated{};
                if (storage_->actual_frequency_domains)
                    for (std::size_t previous{}; previous < index; ++previous)
                        repeated = repeated || std::ranges::find(
                            result.raw_.frequency_views_[previous].domain_id.cpus(), cpu) !=
                            result.raw_.frequency_views_[previous].domain_id.cpus().end();
                if (total_cpus >= authenticated_cpus.size() || repeated ||
                    (storage_->actual_frequency_domains
                        ? std::ranges::find(authenticated_cpus, cpu) == authenticated_cpus.end()
                        : cpu != authenticated_cpus[total_cpus]))
                    return reject(ActivePipelineFailureKind::ledger_validation,
                                  MonitoringDefinitions::frequency);
                ++total_cpus;
            }
        }
        if ((!storage_->actual_frequency_domains && total_cpus != authenticated_cpus.size()) ||
            (storage_->actual_frequency_domains && selected_worker_cpus.size() != authenticated_workers.size()))
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::frequency);
        for (const auto cpu : selected_worker_cpus) {
            const auto covered = std::ranges::any_of(result.raw_.frequency_views_, [&](const auto& group) {
                return std::ranges::find(group.domain_id.cpus(), cpu) != group.domain_id.cpus().end();
            });
            if (!covered) return reject(ActivePipelineFailureKind::ledger_validation,
                                       MonitoringDefinitions::frequency);
        }
        for (std::size_t index{}; index < result.raw_.frequency_views_.size(); ++index) {
            const auto& group = result.raw_.frequency_views_[index];
            const auto kind = result.raw_.frequency_metadata_[index].source_kind();
            const auto selected = inside_points(group.acquisitions, [&](const auto& item) {
                const auto implementation = kind == monitoring::FrequencySourceKind::hardware_current
                    ? "linux-cpufreq-cpuinfo-cur-freq" : "linux-cpufreq-scaling-cur-freq";
                return item.implementation_id().value() == implementation &&
                    (!item.payload() || (item.payload()->domain_id() == group.domain_id &&
                                         item.payload()->source_kind() == kind));
            });
            if (!selected || index >= storage_->classifier_cpu_pools.size() ||
                storage_->classifier_frequency_views.size() == storage_->classifier_frequency_views.capacity())
                return reject(ActivePipelineFailureKind::ledger_validation, MonitoringDefinitions::frequency);
            auto& cpus = storage_->classifier_cpu_pools[index];
            if (group.domain_id.cpus().size() > cpus.capacity())
                return reject(ActivePipelineFailureKind::capacity_invariant, MonitoringDefinitions::frequency);
            for (const auto cpu : group.domain_id.cpus()) cpus.push_back(cpu);
            auto domain = monitoring::FrequencyDomainId::create(std::move(cpus));
            if (!domain) return reject(ActivePipelineFailureKind::typed_factory, MonitoringDefinitions::frequency);
            storage_->classifier_frequency_views.push_back({std::move(*domain), *selected});
        }
        std::ranges::fill(storage_->frequency_scratch, nullptr);
        std::ranges::fill(storage_->frequency_results, std::nullopt);
        FrequencyReplay replay{result.evidence_.frequency_->domains()};
        const auto classified = monitoring::detail::classify_frequency_bounded(
            observation_start, observation_end, result.evidence_.frequency_->sampling_interval(),
            result.evidence_.frequency_->capture_completeness(), storage_->classifier_frequency_views,
            storage_->frequency_scratch, storage_->frequency_results,
            monitoring::detail::FrequencyEvidenceSink{&replay, &FrequencyReplay::append});
        if (!classified || *classified != result.evidence_.frequency_->state() || !replay.equal ||
            replay.index != result.evidence_.frequency_->domains().size())
            return reject(ActivePipelineFailureKind::ledger_validation,
                          MonitoringDefinitions::frequency);
    }
    return std::optional<monitoring::MonitoringCaptureResult>{std::move(result)};
}

namespace {
struct ErrorDefinitions final {
    static constexpr auto error = required_definition("finalized_error");
    static constexpr auto code = required_definition("scalar__finalized_error__code");
    static constexpr auto operation = required_definition("scalar__finalized_error__operation");
    static constexpr auto reason = required_definition("scalar__finalized_error__reason");
    static constexpr auto subsystem = required_definition("scalar__finalized_error__subsystem");
    static constexpr auto category = required_definition("scalar__finalized_error__category");
    static constexpr auto stable = required_definition("scalar__finalized_error__stable_code");
    static constexpr auto severity = required_definition("scalar__finalized_error__severity");
    static constexpr auto continuation = required_definition("scalar__finalized_error__continuation");
    static constexpr auto renderer = required_definition("scalar__finalized_error__renderer");
    static constexpr auto context = required_definition("error_context");
    static constexpr auto kind = required_definition("scalar__error_context__kind");
    static constexpr auto phase = required_definition("scalar__error_context__phase");
    static constexpr auto expected = required_definition("scalar__error_context__expected");
    static constexpr auto observed = required_definition("scalar__error_context__observed");
    static constexpr auto role = required_definition("scalar__error_context__role");
    static constexpr auto ordinal = required_definition("scalar__error_context__worker_ordinal");
    static constexpr auto family = required_definition("scalar__error_context__family");
    static constexpr auto implementation = required_definition("scalar__error_context__implementation");
    static constexpr auto kernel_phase = required_definition("scalar__error_context__kernel_phase");
    static constexpr auto verification = required_definition("scalar__error_context__verification_phase");
    static constexpr auto channel = required_definition("scalar__error_context__channel");
    static constexpr auto missing = required_definition("scalar__error_context__missing_operation");
    static constexpr auto protection = required_definition("scalar__error_context__protection");
    static constexpr auto range = required_definition("scalar__error_context__range");
    static constexpr auto qualification = required_definition("scalar__error_context__qualification");
    static constexpr auto context_reason = required_definition("scalar__error_context__reason");
    static constexpr auto mapping_protection =
        required_definition("scalar__error_context__mapping_protection");
    static constexpr auto advice = required_definition("scalar__error_context__advice");
    static constexpr auto leg = required_definition("scalar__error_context__leg");
    static constexpr auto uncertain = required_definition("scalar__error_context__uncertain");
    static constexpr auto nested = required_definition("nested_failure_value");
    static constexpr auto nested_code = required_definition("scalar__nested_failure_value__code");
    static constexpr auto nested_operation =
        required_definition("scalar__nested_failure_value__operation");
    static constexpr auto nested_reason = required_definition("scalar__nested_failure_value__reason");
    static constexpr auto flat_context = required_definition("error_flat_context");
    static constexpr auto nested_phase =
        required_definition("option__error_context__nested_phase");
    static constexpr auto nested_platform =
        required_definition("option__error_context__nested_platform_failure");
    static constexpr auto nested_errno = required_definition("option__nested_posix_errno");
    static constexpr auto native = required_definition("signed_native_error");
    static constexpr auto os_option = required_definition("option__finalized_error__os_error");
    static constexpr auto os = required_definition("os_error");
    static constexpr auto os_domain = required_definition("scalar__os_error__domain");
    static constexpr auto os_value = required_definition("scalar__os_error__value");
};

auto error_token(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                 std::uint32_t definition, const PreparedParsedNode& parent) noexcept
    -> std::optional<std::string_view> {
    const auto* node = parsed_node_inside(nodes, definition, parent);
    const auto value = node ? lexical(body, *node) : std::nullopt;
    return value ? quoted_payload(*value) : std::nullopt;
}
auto error_token_last(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                      std::uint32_t definition, const PreparedParsedNode& parent) noexcept
    -> std::optional<std::string_view> {
    const PreparedParsedNode* found{};
    const auto end = parent.lexical_begin + parent.lexical_size;
    for (const auto& node : nodes)
        if (node.definition == definition && node.lexical_begin >= parent.lexical_begin &&
            node.lexical_begin < end)
            found = &node;
    const auto value = found ? lexical(body, *found) : std::nullopt;
    return value ? quoted_payload(*value) : std::nullopt;
}

template <class Enum, std::size_t N>
auto error_enum(std::string_view token, const std::array<std::string_view, N>& tokens) noexcept
    -> std::optional<Enum> {
    for (std::size_t index{}; index < tokens.size(); ++index)
        if (tokens[index] == token)
            return static_cast<Enum>(index);
    return std::nullopt;
}

constexpr auto phase_tokens = std::to_array<std::string_view>({
    "preparation", "active", "finalization", "cleanup", "create", "service_exception",
    "begin_capture", "abort_capture", "complete_capture", "stop_and_join", "prepare",
    "page_read", "context_read", "temperature_read", "frequency_read",
    "retained_region_release_mapping", "retained_region_release_bound",
    "retained_region_release_initialized", "retained_region_release_placement_observed",
    "execution_input_validation", "execution_validation", "start_state_schedule",
    "terminal_verification_preparation", "worker_launch", "worker_startup", "worker_callback",
    "worker_command", "worker_execute", "worker_generation", "worker_projection",
    "worker_result_publication", "worker_join"});
constexpr auto kernel_phase_tokens = std::to_array<std::string_view>({
    "none", "pass_count_preparation", "work_accounting", "execution_input_validation",
    "execution_validation", "start_state_schedule", "terminal_verification_preparation"});

auto parse_error_code(std::string_view token) noexcept -> std::optional<PairwiseFailureCode> {
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(code, subsystem, stable, renderer)                                             \
    if (token == #code)                                                                         \
        return PairwiseFailureCode::code;
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    return std::nullopt;
}

auto parse_error_operation(std::string_view token) noexcept
    -> std::optional<PairwiseFailureOperation> {
    for (std::size_t index{}; index < static_cast<std::size_t>(PairwiseFailureOperation::count);
         ++index) {
        const auto value = static_cast<PairwiseFailureOperation>(index);
        if (pairwise_operation_token(value) == token)
            return value;
    }
    return std::nullopt;
}
auto parse_error_reason(std::string_view token) noexcept -> std::optional<PairwiseFailureReason> {
    for (std::size_t index{}; index < static_cast<std::size_t>(PairwiseFailureReason::count);
         ++index) {
        const auto value = static_cast<PairwiseFailureReason>(index);
        if (pairwise_reason_token(value) == token)
            return value;
    }
    return std::nullopt;
}

auto signed_error_inside(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                         std::uint32_t definition, const PreparedParsedNode& parent) noexcept
    -> std::optional<int> {
    const auto token = error_token(body, nodes, definition, parent);
    if (!token)
        return std::nullopt;
    int value{};
    const auto parsed = std::from_chars(token->data(), token->data() + token->size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == token->data() + token->size()
               ? std::optional{value}
               : std::nullopt;
}
auto boolean_inside(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                    std::uint32_t definition, const PreparedParsedNode& parent) noexcept
    -> std::optional<bool> {
    const auto* node = parsed_node_inside(nodes, definition, parent);
    const auto value = node ? lexical(body, *node) : std::nullopt;
    if (value == "true")
        return true;
    if (value == "false")
        return false;
    return std::nullopt;
}

using FlatResult = std::expected<PairwiseFlatFailureContext, ActiveCodecFailure>;
auto parse_flat_error_context(std::span<const char> body,
                              std::span<const PreparedParsedNode> nodes,
                              const PreparedParsedNode& parent) noexcept -> FlatResult {
    const auto reject = [&] { return FlatResult{std::unexpected{parsed_failure(
        ActivePipelineFailureKind::typed_factory, ErrorDefinitions::context)}}; };
    const auto kind = error_token(body, nodes, ErrorDefinitions::kind, parent);
    if (!kind)
        return reject();
    if (*kind == "none")
        return NoPairwiseFailureContext{};
    if (*kind == "lifecycle_phase") {
        const auto token = error_token(body, nodes, ErrorDefinitions::phase, parent);
        const auto value = token ? error_enum<PairwiseLifecyclePhase>(*token, phase_tokens)
                                 : std::nullopt;
        return value ? FlatResult{LifecyclePhaseContext{*value}} : reject();
    }
    if (*kind == "expected_observed_count" || *kind == "expected_observed_ordinal") {
        const auto expected = unsigned_inside(body, nodes, ErrorDefinitions::expected, parent);
        const auto observed = unsigned_inside(body, nodes, ErrorDefinitions::observed, parent);
        if (!expected || !observed)
            return reject();
        if (*kind == "expected_observed_count")
            return ExpectedObservedCountContext{{*expected}, {*observed}};
        return ExpectedObservedOrdinalContext{{*expected}, {*observed}};
    }
    if (*kind == "worker_association") {
        const auto role = error_token(body, nodes, ErrorDefinitions::role, parent);
        const auto ordinal = unsigned_inside(body, nodes, ErrorDefinitions::ordinal, parent);
        if (!role || !ordinal || (*role != "first" && *role != "second"))
            return reject();
        return WorkerAssociationContext{*role == "first" ? PairwiseRole::first : PairwiseRole::second,
                                        {*ordinal}};
    }
    if (*kind == "kernel_identity") {
        const auto family = error_token(body, nodes, ErrorDefinitions::family, parent);
        const auto implementation = error_token(body, nodes, ErrorDefinitions::implementation, parent);
        const auto phase = error_token(body, nodes, ErrorDefinitions::kernel_phase, parent);
        const auto parsed_phase = phase ? error_enum<PairwiseKernelPhase>(*phase, kernel_phase_tokens)
                                        : std::nullopt;
        if (!family || !implementation || !parsed_phase ||
            (*family != "bandwidth" && *family != "latency") ||
            (*implementation != "scalar_u64_sequential_v1" &&
             *implementation != "scalar_u64_random_cycle_v1"))
            return reject();
        return KernelIdentityContext{
            *family == "bandwidth" ? PairwiseKernelFamily::bandwidth : PairwiseKernelFamily::latency,
            *implementation == "scalar_u64_sequential_v1"
                ? PairwiseKernelImplementation::scalar_u64_sequential_v1
                : PairwiseKernelImplementation::scalar_u64_random_cycle_v1,
            *parsed_phase};
    }
#define SIMPLE_CONTEXT(kind_token, definition, tokens_name, enum_type, result_type)              \
    if (*kind == kind_token) {                                                                    \
        const auto token = error_token(body, nodes, definition, parent);                          \
        const auto value = token ? error_enum<enum_type>(*token, tokens_name) : std::nullopt;     \
        return value ? FlatResult{result_type{*value}} : reject();                                \
    }
    constexpr auto verification_tokens = std::to_array<std::string_view>({"before", "after"});
    constexpr auto channel_tokens = std::to_array<std::string_view>(
        {"page_faults", "context_switches", "temperature", "frequency"});
    constexpr auto missing_tokens =
        std::to_array<std::string_view>({"now", "bind_service_cpu", "support", "read", "clock_now"});
    SIMPLE_CONTEXT("verification_phase", ErrorDefinitions::verification, verification_tokens,
                   PairwiseVerificationPhase, VerificationPhaseContext)
    SIMPLE_CONTEXT("monitoring_channel", ErrorDefinitions::channel, channel_tokens,
                   PairwiseMonitoringChannel, MonitoringChannelContext)
    SIMPLE_CONTEXT("missing_operation", ErrorDefinitions::missing, missing_tokens,
                   PairwiseMissingOperation, MissingOperationContext)
#undef SIMPLE_CONTEXT
    if (*kind == "memory_protection_state") {
        const auto protection = error_token(body, nodes, ErrorDefinitions::protection, parent);
        const auto range = error_token(body, nodes, ErrorDefinitions::range, parent);
        const auto qualification = error_token(body, nodes, ErrorDefinitions::qualification, parent);
        const auto reason = error_token(body, nodes, ErrorDefinitions::context_reason, parent);
        constexpr auto qualifications =
            std::to_array<std::string_view>({"qualified", "skipped", "not_qualified"});
        const auto q = qualification
                           ? error_enum<PairwiseQualificationState>(*qualification, qualifications)
                           : std::nullopt;
        const auto r = reason ? parse_error_reason(*reason) : std::nullopt;
        if (protection != "read_only" || range != "full_mapping" || !q || !r)
            return reject();
        return MemoryProtectionStateContext{PairwiseProtectionKind::read_only,
                                            PairwiseRangeKind::full_mapping, *q, *r};
    }
    if (*kind == "requested_protection_advice") {
        constexpr auto protections =
            std::to_array<std::string_view>({"unspecified", "read_write", "read_only"});
        constexpr auto advice = std::to_array<std::string_view>(
            {"none", "base_pages", "huge_pages", "no_huge_pages"});
        const auto protection = error_token(body, nodes, ErrorDefinitions::mapping_protection, parent);
        const auto requested = error_token(body, nodes, ErrorDefinitions::advice, parent);
        const auto p = protection
                           ? error_enum<PairwiseMappingProtection>(*protection, protections)
                           : std::nullopt;
        const auto a = requested ? error_enum<PairwiseMappingAdvice>(*requested, advice) : std::nullopt;
        return p && a ? FlatResult{RequestedProtectionAdviceContext{*p, *a}} : reject();
    }
    if (*kind == "capacity_acquisition") {
        const auto leg = error_token(body, nodes, ErrorDefinitions::leg, parent);
        const auto uncertain = boolean_inside(body, nodes, ErrorDefinitions::uncertain, parent);
        if (!leg || !uncertain || (*leg != "first_controller" && *leg != "second_runner"))
            return reject();
        return PairwiseMonitoringCapacityAcquisitionContext{
            *leg == "first_controller"
                ? PairwiseMonitoringCapacityAcquisitionLeg::first_controller
                : PairwiseMonitoringCapacityAcquisitionLeg::second_runner,
            *uncertain};
    }
    if (*kind == "perf_loss") {
        constexpr auto record_id = required_definition("perf_loss_record");
        constexpr auto ring_id = required_definition("perf_loss_ring");
        constexpr auto drain_id = required_definition("perf_loss_drain");
        const auto* record = parsed_node_inside(nodes, record_id, parent);
        const auto* ring = parsed_node_inside(nodes, ring_id, parent);
        const auto* drain = parsed_node_inside(nodes, drain_id, parent);
        if (!record || !ring || !drain) return reject();
        const auto type = error_token(body, nodes, required_definition("scalar__perf_loss__loss_type"), *record);
        const auto role = error_token(body, nodes, required_definition("scalar__perf_loss__role"), *drain);
        constexpr auto types = std::to_array<std::string_view>({"lost", "lost_samples"});
        constexpr auto roles = std::to_array<std::string_view>({"unscoped", "initial", "scheduled", "final"});
        const auto parsed_type = type ? error_enum<PairwisePerfLossType>(*type, types) : std::nullopt;
        const auto parsed_role = role ? error_enum<PairwisePerfDrainRole>(*role, roles) : std::nullopt;
        const auto header = unsigned_inside(body, nodes, required_definition("scalar__perf_loss__header_size"), *record);
        const auto head = unsigned_inside(body, nodes, required_definition("scalar__perf_loss__captured_head"), *ring);
        const auto tail = unsigned_inside(body, nodes, required_definition("scalar__perf_loss__captured_tail"), *ring);
        const auto bytes = unsigned_inside(body, nodes, required_definition("scalar__perf_loss__ring_bytes"), *ring);
        const auto optional = [&](std::uint32_t option_id, std::uint32_t scalar_id,
                                  const PreparedParsedNode& scope)
            -> std::optional<std::optional<PairwiseCount>> {
            const auto* node = parsed_node_inside(nodes, option_id, scope);
            if (!node) return std::nullopt;
            if (!node->present) return std::optional<std::optional<PairwiseCount>>{std::in_place, std::nullopt};
            const auto count = unsigned_inside(body, nodes, scalar_id, *node);
            if (!count) return std::nullopt;
            return std::optional<std::optional<PairwiseCount>>{std::in_place, PairwiseCount{*count}};
        };
        const auto lost = optional(required_definition("option__perf_loss__lost_count"), required_definition("scalar__perf_loss__lost_count"), *record);
        const auto ordinal = optional(required_definition("option__perf_loss__ordinal"), required_definition("scalar__perf_loss__ordinal"), *drain);
        const auto started = optional(required_definition("option__perf_loss__started_ns"), required_definition("scalar__perf_loss__started_ns"), *drain);
        const auto previous = optional(required_definition("option__perf_loss__last_successful_drain_completed_ns"), required_definition("scalar__perf_loss__last_successful_drain_completed_ns"), parent);
        if (!parsed_type || !parsed_role || !header || !head || !tail || !bytes ||
            !lost || !ordinal || !started || !previous) return reject();
        return PairwisePerfLossContext{
            {*parsed_type, {*header}, *lost}, {{*head}, {*tail}, {*bytes}},
            {*parsed_role, *ordinal ? std::optional<PairwiseOrdinal>{{(**ordinal).value}} : std::nullopt,
             *started}, *previous};
    }
    return reject();
}

using NestedResult = std::expected<PairwiseNestedFailureValue, ActiveCodecFailure>;
auto parse_nested_error(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                        const PreparedParsedNode& parent) noexcept -> NestedResult {
    const auto reject = [&] { return NestedResult{std::unexpected{parsed_failure(
        ActivePipelineFailureKind::typed_factory, ErrorDefinitions::nested)}}; };
    const auto code = error_token(body, nodes, ErrorDefinitions::nested_code, parent);
    const auto operation = error_token(body, nodes, ErrorDefinitions::nested_operation, parent);
    const auto reason = error_token(body, nodes, ErrorDefinitions::nested_reason, parent);
    const auto* context = parsed_node_inside(nodes, ErrorDefinitions::flat_context, parent);
    const auto* native = parsed_node_inside(nodes, ErrorDefinitions::nested_errno, parent);
    if (!code || !operation || !reason || !context || !native)
        return reject();
    const auto parsed_code = parse_error_code(*code);
    const auto parsed_operation = parse_error_operation(*operation);
    const auto parsed_reason = parse_error_reason(*reason);
    const auto parsed_context = parse_flat_error_context(body, nodes, *context);
    std::optional<int> os_error;
    if (native->present) {
        const auto value = signed_error_inside(body, nodes, ErrorDefinitions::native, *native);
        if (!value || *value <= 0)
            return reject();
        os_error = *value;
    }
    if (!parsed_code || !parsed_operation || !parsed_reason || !parsed_context)
        return reject();
    return PairwiseNestedFailureValue{*parsed_code, *parsed_operation, *parsed_reason,
                                      *parsed_context, os_error};
}

auto parse_error_context(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                         const PreparedParsedNode& parent) noexcept
    -> std::expected<PairwiseFailureContext, ActiveCodecFailure> {
    const auto kind = error_token_last(body, nodes, ErrorDefinitions::kind, parent);
    if (!kind)
        return std::unexpected{parsed_failure(ActivePipelineFailureKind::typed_factory,
                                              ErrorDefinitions::context)};
    if (*kind != "nested_failure") {
        auto flat = parse_flat_error_context(body, nodes, parent);
        if (!flat)
            return std::unexpected{flat.error()};
        return std::visit([](const auto& value) -> PairwiseFailureContext { return value; }, *flat);
    }
    const auto* failure = parsed_node_inside(nodes, ErrorDefinitions::nested, parent);
    const auto* phase = parsed_node_inside(nodes, ErrorDefinitions::nested_phase, parent);
    const auto* platform = parsed_node_inside(nodes, ErrorDefinitions::nested_platform, parent);
    if (!failure || !phase || !platform)
        return std::unexpected{parsed_failure(ActivePipelineFailureKind::typed_factory,
                                              ErrorDefinitions::context)};
    auto nested = parse_nested_error(body, nodes, *failure);
    if (!nested)
        return std::unexpected{nested.error()};
    std::optional<PairwiseLifecyclePhase> nested_phase;
    if (phase->present) {
        const auto token = error_token(body, nodes, ErrorDefinitions::phase, *phase);
        const auto value = token ? error_enum<PairwiseLifecyclePhase>(*token, phase_tokens)
                                 : std::nullopt;
        if (!value)
            return std::unexpected{parsed_failure(ActivePipelineFailureKind::typed_factory,
                                                  ErrorDefinitions::nested_phase)};
        nested_phase = *value;
    }
    std::optional<PairwiseNestedFailureValue> nested_platform;
    if (platform->present) {
        const auto* value = parsed_node_inside(nodes, ErrorDefinitions::nested, *platform);
        if (!value)
            return std::unexpected{parsed_failure(ActivePipelineFailureKind::typed_factory,
                                                  ErrorDefinitions::nested_platform)};
        auto parsed = parse_nested_error(body, nodes, *value);
        if (!parsed)
            return std::unexpected{parsed.error()};
        nested_platform = *parsed;
    }
    return PairwiseFailureContext{NestedFailureContext{*nested, nested_phase, nested_platform}};
}
} // namespace

auto PreparedFinalizedErrorBuilder::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedFinalizedErrorBuilder,
                     prepared_resources_v1::PreparedResourceError> {
    if (resource.definitions.size() != wire_v1::detail::schema_nodes.size() ||
        resource.definitions[ErrorDefinitions::error].owner_occurrences == 0U)
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
    return PreparedFinalizedErrorBuilder{
        resource.definitions[ErrorDefinitions::error].owner_occurrences};
}

auto PreparedFinalizedErrorBuilder::build(
    std::span<const char> body, std::span<const PreparedParsedNode> nodes,
    const PreparedParsedNode& error) noexcept
    -> std::expected<FinalizedEvidenceError, ActiveCodecFailure> {
    const auto reject = [&](ActivePipelineFailureKind kind, std::uint32_t definition) {
        return std::expected<FinalizedEvidenceError, ActiveCodecFailure>{
            std::unexpected{parsed_failure(kind, definition)}};
    };
    if (consumed_ || admitted_ == 0U || error.definition != ErrorDefinitions::error)
        return reject(ActivePipelineFailureKind::transport_state, ErrorDefinitions::error);
    consumed_ = true;
    const auto code = error_token(body, nodes, ErrorDefinitions::code, error);
    const auto operation = error_token(body, nodes, ErrorDefinitions::operation, error);
    const auto reason = error_token(body, nodes, ErrorDefinitions::reason, error);
    const auto* context_node = parsed_node_inside(nodes, ErrorDefinitions::context, error);
    const auto* os_option = parsed_node_inside(nodes, ErrorDefinitions::os_option, error);
    if (!code || !operation || !reason || !context_node || !os_option)
        return reject(ActivePipelineFailureKind::registry_shape, ErrorDefinitions::error);
    const auto parsed_code = parse_error_code(*code);
    const auto parsed_operation = parse_error_operation(*operation);
    const auto parsed_reason = parse_error_reason(*reason);
    const auto context = parse_error_context(body, nodes, *context_node);
    if (!parsed_code || !parsed_operation || !parsed_reason || !context)
        return reject(ActivePipelineFailureKind::typed_factory, ErrorDefinitions::error);
    std::optional<int> native;
    if (os_option->present) {
        const auto* os = parsed_node_inside(nodes, ErrorDefinitions::os, *os_option);
        const auto domain = os ? error_token(body, nodes, ErrorDefinitions::os_domain, *os)
                               : std::nullopt;
        const auto value = os ? signed_error_inside(body, nodes, ErrorDefinitions::os_value, *os)
                              : std::nullopt;
        if (domain != "posix_errno" || !value || *value <= 0)
            return reject(ActivePipelineFailureKind::typed_factory, ErrorDefinitions::os);
        native = *value;
    }
    const auto subsystem = pairwise_failure_subsystem(*parsed_code);
    const auto make = [&]<PairwiseFailureSubsystem Subsystem>()
        -> std::optional<PairwiseEvidenceFailure> {
        auto value = BasicPairwiseFailure<Subsystem>::create(
            *parsed_code, *parsed_operation, *parsed_reason, *context, native);
        return value ? std::optional<PairwiseEvidenceFailure>{*value} : std::nullopt;
    };
    std::optional<PairwiseEvidenceFailure> failure;
    switch (subsystem) {
    case PairwiseFailureSubsystem::platform:
        failure = make.template operator()<PairwiseFailureSubsystem::platform>();
        break;
    case PairwiseFailureSubsystem::memory:
        failure = make.template operator()<PairwiseFailureSubsystem::memory>();
        break;
    case PairwiseFailureSubsystem::monitoring:
        failure = make.template operator()<PairwiseFailureSubsystem::monitoring>();
        break;
    case PairwiseFailureSubsystem::kernel:
        failure = make.template operator()<PairwiseFailureSubsystem::kernel>();
        break;
    case PairwiseFailureSubsystem::measurement:
        failure = make.template operator()<PairwiseFailureSubsystem::measurement>();
        break;
    case PairwiseFailureSubsystem::statistics:
        failure = make.template operator()<PairwiseFailureSubsystem::statistics>();
        break;
    case PairwiseFailureSubsystem::worker:
        failure = make.template operator()<PairwiseFailureSubsystem::worker>();
        break;
    case PairwiseFailureSubsystem::runtime:
        failure = make.template operator()<PairwiseFailureSubsystem::runtime>();
        break;
    case PairwiseFailureSubsystem::count:
        break;
    }
    if (!failure)
        return reject(ActivePipelineFailureKind::typed_factory, ErrorDefinitions::error);
    auto result = FinalizedEvidenceError::from_failure(*failure);
    const auto profile = pairwise_finalized_error_profiles()[static_cast<std::size_t>(*parsed_code)];
    const auto subsystem_token = std::to_array<std::string_view>(
        {"platform", "memory", "monitoring", "kernel", "measurement", "statistics", "worker", "runtime"});
    const auto category_token = std::to_array<std::string_view>(
        {"core", "build", "platform_linux", "topology", "scenario", "runtime", "memory", "kernel",
         "measurement", "monitoring", "statistics", "results", "reporting", "validation", "usage"});
    const auto severity_token =
        std::to_array<std::string_view>({"information", "warning", "error", "fatal"});
    const auto continuation_token = std::to_array<std::string_view>(
        {"continue_operation", "skip_item", "invalidate_observation", "abort_experiment", "abort_command"});
    const auto projected = [&](std::uint32_t definition) { return error_token(body, nodes, definition, error); };
    if (projected(ErrorDefinitions::subsystem) !=
        subsystem_token[static_cast<std::size_t>(profile.subsystem)])
        return reject(ActivePipelineFailureKind::ledger_validation, ErrorDefinitions::subsystem);
    if (projected(ErrorDefinitions::category) !=
        category_token[static_cast<std::size_t>(profile.category)])
        return reject(ActivePipelineFailureKind::ledger_validation, ErrorDefinitions::category);
    if (projected(ErrorDefinitions::stable) != result.stable_code())
        return reject(ActivePipelineFailureKind::ledger_validation, ErrorDefinitions::stable);
    if (projected(ErrorDefinitions::severity) !=
        severity_token[static_cast<std::size_t>(profile.severity)])
        return reject(ActivePipelineFailureKind::ledger_validation, ErrorDefinitions::severity);
    if (projected(ErrorDefinitions::continuation) !=
        continuation_token[static_cast<std::size_t>(profile.continuation)])
        return reject(ActivePipelineFailureKind::ledger_validation, ErrorDefinitions::continuation);
    if (projected(ErrorDefinitions::renderer) != profile.renderer_token)
        return reject(ActivePipelineFailureKind::ledger_validation, ErrorDefinitions::renderer);
    if (result.operation_token() != *operation)
        return reject(ActivePipelineFailureKind::ledger_validation, ErrorDefinitions::operation);
    if (result.reason_token() != *reason)
        return reject(ActivePipelineFailureKind::ledger_validation, ErrorDefinitions::reason);
    return result;
}

auto PreparedFinalizedErrorPool::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedFinalizedErrorPool,
                     prepared_resources_v1::PreparedResourceError> {
    try {
        if (resource.definitions.size() != wire_v1::detail::schema_nodes.size())
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        const auto count = resource.definitions[ErrorDefinitions::error].owner_occurrences;
        if (count > std::numeric_limits<std::size_t>::max())
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::size_unrepresentable}};
        std::vector<PreparedFinalizedErrorBuilder> builders;
        builders.reserve(static_cast<std::size_t>(count));
        std::vector<std::uint8_t> consumed(static_cast<std::size_t>(count), 0U);
        for (std::uint64_t ordinal{}; ordinal < count; ++ordinal) {
            auto builder = PreparedFinalizedErrorBuilder::prepare(resource);
            if (!builder) return std::unexpected{builder.error()};
            builders.push_back(std::move(*builder));
        }
        return PreparedFinalizedErrorPool{std::move(builders), std::move(consumed)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedFinalizedErrorPool::build(
    std::size_t ordinal, std::span<const char> body,
    std::span<const PreparedParsedNode> nodes,
    const PreparedParsedNode& error) noexcept
    -> std::expected<FinalizedEvidenceError, ActiveCodecFailure> {
    if (ordinal >= builders_.size())
        return std::unexpected{ActiveCodecFailure{
            ActivePipelineFailureKind::capacity_invariant, ActivePipelineStage::active,
            ErrorDefinitions::error, builders_.size(), ordinal + 1U, std::nullopt}};
    if (consumed_[ordinal] != 0U)
        return std::unexpected{ActiveCodecFailure{
            ActivePipelineFailureKind::transport_state, ActivePipelineStage::active,
            ErrorDefinitions::error, ordinal, ordinal, std::nullopt}};
    consumed_[ordinal] = 1U;
    if (error.definition != ErrorDefinitions::error || error.occurrence != ordinal)
        return std::unexpected{ActiveCodecFailure{
            ActivePipelineFailureKind::ledger_validation, ActivePipelineStage::active,
            ErrorDefinitions::error, ordinal, error.occurrence, std::nullopt}};
    return builders_[ordinal].build(body, nodes, error);
}

namespace {
struct MemoryDefinitions final {
    static constexpr auto region = required_definition("region_diagnostic");
    static constexpr auto kind = required_definition("scalar__region_diagnostic__kind");
    static constexpr auto unavailable = required_definition("scalar__region_diagnostic__unavailable_reason");
    static constexpr auto request = required_definition("region_request");
    static constexpr auto worker = required_definition("scalar__region_request__worker_id");
    static constexpr auto bytes = required_definition("scalar__region_request__requested_bytes");
    static constexpr auto page = required_definition("scalar__region_request__page_size");
    static constexpr auto page_mode = required_definition("scalar__region_request__page_mode");
    static constexpr auto policy = required_definition("scalar__region_request__placement_policy");
    static constexpr auto targets = required_definition("array__region_request__numa_targets");
    static constexpr auto target = required_definition("region_numa_target");
    static constexpr auto stable = required_definition("scalar__region_numa_target__stable_node");
    static constexpr auto linux_node = required_definition("scalar__region_numa_target__linux_node");
    static constexpr auto topology_node = required_definition("scalar__region_numa_target__topology_node");
    static constexpr auto ratio = required_definition("exact_ratio");
    static constexpr auto numerator = required_definition("scalar__exact_ratio__numerator");
    static constexpr auto denominator = required_definition("scalar__exact_ratio__denominator");
    static constexpr auto prefault = required_definition("scalar__region_request__prefault");
    static constexpr auto verify_before = required_definition("scalar__region_request__verify_before");
    static constexpr auto verify_after = required_definition("scalar__region_request__verify_after");
    static constexpr auto binding = required_definition("region_binding");
    static constexpr auto bind_policy = required_definition("scalar__region_binding__policy");
    static constexpr auto accepted = required_definition("scalar__region_binding__policy_call_accepted");
    static constexpr auto migration = required_definition("scalar__region_binding__migration_requested");
    static constexpr auto strict = required_definition("scalar__region_binding__strict_verification_requested");
    static constexpr auto initialization = required_definition("region_initialization");
    static constexpr auto init_prefault = required_definition("scalar__region_initialization__prefault_requested");
    static constexpr auto init_pass = required_definition("scalar__region_initialization__prefault_pass_performed");
    static constexpr auto init_complete = required_definition("scalar__region_initialization__initialization_completed");
    static constexpr auto padding = required_definition("scalar__region_initialization__padding_touched");
    static constexpr auto pre = required_definition("pre_run_placement");
    static constexpr auto pre_state = required_definition("scalar__pre_run_placement__state");
    static constexpr auto pre_observation = required_definition("option__pre_run_placement__observation");
    static constexpr auto pre_evaluation = required_definition("option__pre_run_placement__evaluation");
    static constexpr auto pre_qualification = required_definition("option__pre_run_placement__qualification_state");
    static constexpr auto post_option = required_definition("option__region_diagnostic__post_run");
    static constexpr auto post = required_definition("post_run_placement");
    static constexpr auto post_state = required_definition("scalar__post_run_placement__state");
    static constexpr auto post_observation = required_definition("option__post_run_placement__observation");
    static constexpr auto post_evaluation = required_definition("option__post_run_placement__evaluation");
    static constexpr auto post_qualification = required_definition("option__post_run_placement__qualification_state");
    static constexpr auto qualification = required_definition("qualification_state");
    static constexpr auto observation = required_definition("placement_observation");
    static constexpr auto total = required_definition("scalar__placement_observation__total_pages");
    static constexpr auto resident = required_definition("scalar__placement_observation__resident_pages");
    static constexpr auto unresolved = required_definition("scalar__placement_observation__unresolved_pages");
    static constexpr auto counts = required_definition("array__placement_observation__resident_page_counts");
    static constexpr auto count = required_definition("node_page_count");
    static constexpr auto count_node = required_definition("scalar__node_page_count__linux_node");
    static constexpr auto count_pages = required_definition("scalar__node_page_count__page_count");
    static constexpr auto evaluation = required_definition("placement_evaluation");
    static constexpr auto requested = required_definition("scalar__placement_evaluation__requested_node_pages");
    static constexpr auto unexpected = required_definition("scalar__placement_evaluation__unexpected_node_pages");
    static constexpr auto evaluation_unresolved = required_definition("scalar__placement_evaluation__unresolved_pages");
    static constexpr auto protection_option = required_definition("option__region_diagnostic__protection");
    static constexpr auto protection = required_definition("region_protection");
    static constexpr auto protection_kind = required_definition("scalar__region_protection__protection_kind");
    static constexpr auto protection_accepted = required_definition("scalar__region_protection__protection_accepted");
    static constexpr auto writable = required_definition("scalar__region_protection__writable");
    static constexpr auto executable = required_definition("scalar__region_protection__executable");
    static constexpr auto cleanup = required_definition("region_cleanup");
    static constexpr auto release_attempted = required_definition("scalar__region_cleanup__release_attempted");
    static constexpr auto release_succeeded = required_definition("scalar__region_cleanup__release_succeeded");
    static constexpr auto active_after = required_definition("scalar__region_cleanup__mapping_active_after");
    static constexpr auto path = required_definition("scalar__region_diagnostic__finalization_path");
    static constexpr auto disposition = required_definition("scalar__region_diagnostic__disposition");
};

auto memory_token(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                  std::uint32_t definition, const PreparedParsedNode& parent) noexcept
    -> std::optional<std::string_view> {
    return error_token(body, nodes, definition, parent);
}
auto memory_ratio(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                  const PreparedParsedNode& parent) noexcept -> std::optional<memory::ExactRatio> {
    const auto numerator = unsigned_inside(body, nodes, MemoryDefinitions::numerator, parent);
    const auto denominator = unsigned_inside(body, nodes, MemoryDefinitions::denominator, parent);
    if (!numerator || !denominator || *denominator == 0U || *numerator > *denominator ||
        std::gcd(*numerator, *denominator) != 1U)
        return std::nullopt;
    return memory::detail::PairwiseMemoryAccess::ratio(*numerator, *denominator);
}
auto memory_qualification(std::string_view token) noexcept
    -> std::optional<memory::PlacementQualificationState> {
    if (token == "skipped") return memory::PlacementQualificationState::skipped;
    if (token == "qualified") return memory::PlacementQualificationState::qualified;
    if (token == "not_qualified") return memory::PlacementQualificationState::not_qualified;
    return std::nullopt;
}
auto memory_observation_valid(const memory::RegionRequest& request,
                              const memory::RegionPlacementObservation& observation) noexcept
    -> bool {
    if (observation.total_pages() == 0U ||
        observation.resident_pages() > observation.total_pages() ||
        observation.unresolved_pages() !=
            observation.total_pages() - observation.resident_pages())
        return false;
    std::uint64_t sum{};
    std::optional<core::LinuxNumaNodeId> prior;
    for (const auto& item : observation.resident_page_counts()) {
        if (item.page_count() == 0U || (prior && item.linux_node() <= *prior) ||
            item.page_count() > std::numeric_limits<std::uint64_t>::max() - sum)
            return false;
        prior = item.linux_node();
        sum += item.page_count();
    }
    return sum == observation.resident_pages() &&
           observation.total_pages() ==
               request.requested_bytes().value() / request.page_size().value();
}
} // namespace

struct PreparedMemoryDiagnosticBuilder::Storage final {
    std::vector<memory::RegionNumaTarget> targets;
    std::vector<std::string> stable_ids;
    std::vector<memory::LinuxNodePageCount> pre_counts;
    std::vector<memory::LinuxNodePageCount> post_counts;
};

PreparedMemoryDiagnosticBuilder::~PreparedMemoryDiagnosticBuilder() noexcept = default;
PreparedMemoryDiagnosticBuilder::PreparedMemoryDiagnosticBuilder(
    std::unique_ptr<Storage> storage) noexcept : storage_{std::move(storage)} {}

auto PreparedMemoryDiagnosticBuilder::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedMemoryDiagnosticBuilder,
                     prepared_resources_v1::PreparedResourceError> {
    const auto mismatch = [] {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
    };
    if (resource.definitions.size() != wire_v1::detail::schema_nodes.size() ||
        resource.definitions[MemoryDefinitions::region].owner_occurrences == 0U)
        return mismatch();
    const auto target_capacity = stock_cardinality(resource, MemoryDefinitions::targets);
    const auto count_capacity = stock_cardinality(resource, MemoryDefinitions::counts);
    const auto stable_capacity = resource.definitions[MemoryDefinitions::stable].decoded_utf8_bytes;
    if (!target_capacity || !count_capacity || !stable_capacity ||
        *target_capacity > std::numeric_limits<std::size_t>::max() ||
        *count_capacity > std::numeric_limits<std::size_t>::max() ||
        *stable_capacity > std::numeric_limits<std::size_t>::max())
        return mismatch();
    try {
        auto storage = std::make_unique<Storage>();
        storage->targets.reserve(static_cast<std::size_t>(*target_capacity));
        storage->stable_ids.resize(static_cast<std::size_t>(*target_capacity));
        for (auto& value : storage->stable_ids)
            value.reserve(static_cast<std::size_t>(*stable_capacity));
        storage->pre_counts.reserve(static_cast<std::size_t>(*count_capacity));
        storage->post_counts.reserve(static_cast<std::size_t>(*count_capacity));
        return PreparedMemoryDiagnosticBuilder{std::move(storage)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedMemoryDiagnosticBuilder::build(
    std::span<const char> body, std::span<const PreparedParsedNode> nodes,
    const PreparedParsedNode& region, core::WorkerId authenticated_worker) noexcept
    -> std::expected<memory::RegionFinalizationDiagnostic, ActiveCodecFailure> {
    using Result = std::expected<memory::RegionFinalizationDiagnostic, ActiveCodecFailure>;
    const auto reject = [&](std::uint32_t definition) -> Result {
        return std::unexpected{parsed_failure(ActivePipelineFailureKind::ledger_validation,
                                              definition)};
    };
    if (consumed_ || !storage_ || region.definition != MemoryDefinitions::region)
        return std::unexpected{parsed_failure(ActivePipelineFailureKind::transport_state,
                                              MemoryDefinitions::region)};
    consumed_ = true;
    const auto kind = memory_token(body, nodes, MemoryDefinitions::kind, region);
    if (!kind)
        return reject(MemoryDefinitions::region);
    if (*kind == "unavailable") {
        const auto reason = unsigned_inside(body, nodes, MemoryDefinitions::unavailable, region);
        if (!reason || *reason > 1U)
            return reject(MemoryDefinitions::unavailable);
        return memory::detail::PairwiseMemoryAccess::unavailable_diagnostic(
            *reason == 0U ? memory::RegionFinalizationUnavailableReason::contained_exception
                          : memory::RegionFinalizationUnavailableReason::contained_unknown_failure);
    }
    if (*kind != "available")
        return reject(MemoryDefinitions::region);
    const auto* request_node = parsed_node_inside(nodes, MemoryDefinitions::request, region);
    const auto* targets_node = request_node
                                   ? parsed_node_inside(nodes, MemoryDefinitions::targets, *request_node)
                                   : nullptr;
    const auto* ratio_node = request_node
                                 ? parsed_node_inside(nodes, MemoryDefinitions::ratio, *request_node)
                                 : nullptr;
    const auto worker = request_node
                            ? unsigned_inside(body, nodes, MemoryDefinitions::worker, *request_node)
                            : std::nullopt;
    const auto bytes = request_node
                           ? unsigned_inside(body, nodes, MemoryDefinitions::bytes, *request_node)
                           : std::nullopt;
    const auto page = request_node
                          ? unsigned_inside(body, nodes, MemoryDefinitions::page, *request_node)
                          : std::nullopt;
    const auto prefault = request_node
                              ? boolean_inside(body, nodes, MemoryDefinitions::prefault, *request_node)
                              : std::nullopt;
    const auto verify_before = request_node
                                   ? boolean_inside(body, nodes, MemoryDefinitions::verify_before,
                                                    *request_node)
                                   : std::nullopt;
    const auto verify_after = request_node
                                  ? boolean_inside(body, nodes, MemoryDefinitions::verify_after,
                                                   *request_node)
                                  : std::nullopt;
    const auto page_mode = request_node
                               ? memory_token(body, nodes, MemoryDefinitions::page_mode, *request_node)
                               : std::nullopt;
    const auto policy = request_node
                            ? memory_token(body, nodes, MemoryDefinitions::policy, *request_node)
                            : std::nullopt;
    const auto threshold = ratio_node ? memory_ratio(body, nodes, *ratio_node) : std::nullopt;
    if (!request_node || !targets_node || !worker || !bytes || !page || !prefault ||
        !verify_before || !verify_after || page_mode != "base" || policy != "bind" ||
        !threshold || *worker != authenticated_worker.value() || targets_node->element_count == 0U ||
        targets_node->element_count > storage_->targets.capacity())
        return reject(MemoryDefinitions::request);
    for (std::uint64_t index{}; index < targets_node->element_count; ++index) {
        const auto* target = parsed_node_inside(nodes, MemoryDefinitions::target, *targets_node, index);
        const auto* stable_node =
            target ? parsed_node_inside(nodes, MemoryDefinitions::stable, *target) : nullptr;
        const auto stable = stable_node ? lexical(body, *stable_node) : std::nullopt;
        const auto linux_node = target
                                    ? unsigned_inside(body, nodes, MemoryDefinitions::linux_node, *target)
                                    : std::nullopt;
        const auto topology_node = target
                                       ? unsigned_inside(body, nodes, MemoryDefinitions::topology_node,
                                                         *target)
                                       : std::nullopt;
        if (!target || !stable || !linux_node || !topology_node ||
            *linux_node > std::numeric_limits<std::uint32_t>::max() ||
            *topology_node > std::numeric_limits<std::uint32_t>::max() ||
            index >= storage_->stable_ids.size() ||
            !decode_string(*stable, storage_->stable_ids[index].capacity(),
                           storage_->stable_ids[index]))
            return reject(MemoryDefinitions::target);
        const auto stable_value = std::string_view{storage_->stable_ids[index]};
        if (!stable_value.starts_with("numa/") || stable_value.size() == 5U ||
            (stable_value.size() > 6U && stable_value[5] == '0'))
            return reject(MemoryDefinitions::stable);
        std::uint32_t stable_ordinal{};
        const auto stable_parsed = std::from_chars(stable_value.data() + 5U,
                                                   stable_value.data() + stable_value.size(),
                                                   stable_ordinal);
        if (stable_parsed.ec != std::errc{} ||
            stable_parsed.ptr != stable_value.data() + stable_value.size())
            return reject(MemoryDefinitions::stable);
        storage_->targets.push_back({topology::detail::PreparedStableTopologyIdAccess::adopt_numa(
                                         std::move(storage_->stable_ids[index])),
                                     core::LinuxNumaNodeId{static_cast<std::uint32_t>(*linux_node)},
                                     core::NumaNodeId{static_cast<std::uint32_t>(*topology_node)}});
    }
    if (*bytes == 0U || *page == 0U || *bytes % *page != 0U)
        return reject(MemoryDefinitions::request);
    for (std::size_t left{}; left < storage_->targets.size(); ++left)
        for (std::size_t right = left + 1U; right < storage_->targets.size(); ++right)
            if (storage_->targets[left].stable_node == storage_->targets[right].stable_node ||
                storage_->targets[left].linux_node == storage_->targets[right].linux_node ||
                storage_->targets[left].topology_node == storage_->targets[right].topology_node)
                return reject(MemoryDefinitions::targets);
    auto request = memory::detail::PairwiseMemoryAccess::request(
        {authenticated_worker, core::ByteCount::from_base_units(*bytes),
         core::ByteCount::from_base_units(*page), memory::PlacementPolicy::bind,
         memory::PageMode::base, std::move(storage_->targets), *threshold, *prefault,
         *verify_before, *verify_after});
    const auto* binding = parsed_node_inside(nodes, MemoryDefinitions::binding, region);
    const auto* initialization = parsed_node_inside(nodes, MemoryDefinitions::initialization, region);
    if (!binding || !initialization ||
        memory_token(body, nodes, MemoryDefinitions::bind_policy, *binding) != "bind" ||
        boolean_inside(body, nodes, MemoryDefinitions::accepted, *binding) != true ||
        boolean_inside(body, nodes, MemoryDefinitions::migration, *binding) != false ||
        boolean_inside(body, nodes, MemoryDefinitions::strict, *binding) != false ||
        boolean_inside(body, nodes, MemoryDefinitions::init_prefault, *initialization) != *prefault ||
        boolean_inside(body, nodes, MemoryDefinitions::init_pass, *initialization) != *prefault ||
        boolean_inside(body, nodes, MemoryDefinitions::init_complete, *initialization) != true ||
        boolean_inside(body, nodes, MemoryDefinitions::padding, *initialization) != false)
        return reject(MemoryDefinitions::binding);

    const auto parse_placement = [&](const PreparedParsedNode& placement, bool pre,
                                     std::vector<memory::LinuxNodePageCount>& scratch)
        -> std::optional<std::tuple<std::string_view,
                                   std::optional<memory::RegionPlacementObservation>,
                                   std::optional<memory::RegionPlacementEvaluation>,
                                   std::optional<memory::PlacementQualificationState>>> {
        const auto state = memory_token(body, nodes,
                                        pre ? MemoryDefinitions::pre_state
                                            : MemoryDefinitions::post_state,
                                        placement);
        const auto* observation_option = parsed_node_inside(
            nodes, pre ? MemoryDefinitions::pre_observation : MemoryDefinitions::post_observation,
            placement);
        const auto* evaluation_option = parsed_node_inside(
            nodes, pre ? MemoryDefinitions::pre_evaluation : MemoryDefinitions::post_evaluation,
            placement);
        const auto* qualification_option = parsed_node_inside(
            nodes, pre ? MemoryDefinitions::pre_qualification : MemoryDefinitions::post_qualification,
            placement);
        if (!state || !observation_option || !evaluation_option || !qualification_option)
            return std::nullopt;
        std::optional<memory::RegionPlacementObservation> observation;
        if (observation_option->present) {
            const auto* value = parsed_node_inside(nodes, MemoryDefinitions::observation,
                                                   *observation_option);
            const auto* counts = value ? parsed_node_inside(nodes, MemoryDefinitions::counts, *value)
                                       : nullptr;
            const auto total = value ? unsigned_inside(body, nodes, MemoryDefinitions::total, *value)
                                     : std::nullopt;
            const auto resident = value
                                      ? unsigned_inside(body, nodes, MemoryDefinitions::resident, *value)
                                      : std::nullopt;
            const auto unresolved = value
                                        ? unsigned_inside(body, nodes, MemoryDefinitions::unresolved,
                                                          *value)
                                        : std::nullopt;
            if (!value || !counts || !total || !resident || !unresolved ||
                counts->element_count > scratch.capacity())
                return std::nullopt;
            for (std::uint64_t index{}; index < counts->element_count; ++index) {
                const auto* item = parsed_node_inside(nodes, MemoryDefinitions::count, *counts, index);
                const auto node = item
                                      ? unsigned_inside(body, nodes, MemoryDefinitions::count_node, *item)
                                      : std::nullopt;
                const auto pages = item
                                       ? unsigned_inside(body, nodes, MemoryDefinitions::count_pages, *item)
                                       : std::nullopt;
                if (!node || !pages || *node > std::numeric_limits<std::uint32_t>::max())
                    return std::nullopt;
                scratch.push_back(memory::detail::PairwiseMemoryAccess::node_count(
                    core::LinuxNumaNodeId{static_cast<std::uint32_t>(*node)}, *pages));
            }
            observation = memory::detail::PairwiseMemoryAccess::observation(
                *total, *resident, *unresolved, std::move(scratch));
        }
        std::optional<memory::RegionPlacementEvaluation> evaluation;
        if (evaluation_option->present) {
            const auto* value = parsed_node_inside(nodes, MemoryDefinitions::evaluation,
                                                   *evaluation_option);
            const auto* ratio = value ? parsed_node_inside(nodes, MemoryDefinitions::ratio, *value)
                                      : nullptr;
            const auto requested = value
                                       ? unsigned_inside(body, nodes, MemoryDefinitions::requested, *value)
                                       : std::nullopt;
            const auto unexpected = value
                                        ? unsigned_inside(body, nodes, MemoryDefinitions::unexpected,
                                                          *value)
                                        : std::nullopt;
            const auto unresolved = value
                                        ? unsigned_inside(body, nodes,
                                                          MemoryDefinitions::evaluation_unresolved,
                                                          *value)
                                        : std::nullopt;
            const auto observed = ratio ? memory_ratio(body, nodes, *ratio) : std::nullopt;
            if (!value || !requested || !unexpected || !unresolved || !observed)
                return std::nullopt;
            evaluation = memory::detail::PairwiseMemoryAccess::placement_evaluation(
                *requested, *unexpected, *unresolved, *observed);
        }
        std::optional<memory::PlacementQualificationState> qualification;
        if (qualification_option->present) {
            const auto token = memory_token(body, nodes, MemoryDefinitions::qualification,
                                            *qualification_option);
            qualification = token ? memory_qualification(*token) : std::nullopt;
            if (!qualification)
                return std::nullopt;
        }
        return std::tuple{*state, std::move(observation), std::move(evaluation), qualification};
    };
    const auto* pre_node = parsed_node_inside(nodes, MemoryDefinitions::pre, region);
    auto pre = pre_node ? parse_placement(*pre_node, true, storage_->pre_counts) : std::nullopt;
    if (!pre)
        return reject(MemoryDefinitions::pre);
    auto& [pre_state, pre_observation, pre_evaluation, pre_qualification] = *pre;
    const bool pre_observed = pre_state == "observed";
    if ((!pre_observed && pre_state != "skipped") ||
        pre_observed != static_cast<bool>(pre_observation && pre_evaluation && pre_qualification) ||
        pre_observed != *verify_before)
        return reject(MemoryDefinitions::pre);
    if (pre_observed) {
        if (!memory_observation_valid(request, *pre_observation))
            return reject(MemoryDefinitions::pre);
        const auto derived = memory::detail::PlacementEvaluatedRegionAccess::evaluate_region_placement(
            request, *pre_observation);
        if (!derived || derived->qualification_state != *pre_qualification ||
            derived->evaluation.requested_node_pages() != pre_evaluation->requested_node_pages() ||
            derived->evaluation.unexpected_node_pages() != pre_evaluation->unexpected_node_pages() ||
            derived->evaluation.unresolved_pages() != pre_evaluation->unresolved_pages() ||
            derived->evaluation.observed_ratio() != pre_evaluation->observed_ratio())
            return reject(MemoryDefinitions::pre);
    }
    auto pre_value = memory::detail::PairwiseMemoryAccess::pre_run(
        pre_observed ? memory::PreRunDiagnosticState::observed
                     : memory::PreRunDiagnosticState::skipped,
        std::move(pre_observation), std::move(pre_evaluation), pre_qualification);
    const auto* protection_option = parsed_node_inside(nodes, MemoryDefinitions::protection_option, region);
    std::optional<memory::RegionProtectionEvidence> protection;
    if (!protection_option)
        return reject(MemoryDefinitions::protection_option);
    if (protection_option->present) {
        const auto* value = parsed_node_inside(nodes, MemoryDefinitions::protection, *protection_option);
        if (!value || memory_token(body, nodes, MemoryDefinitions::protection_kind, *value) != "read_only" ||
            boolean_inside(body, nodes, MemoryDefinitions::protection_accepted, *value) != true ||
            boolean_inside(body, nodes, MemoryDefinitions::writable, *value) != false ||
            boolean_inside(body, nodes, MemoryDefinitions::executable, *value) != false)
            return reject(MemoryDefinitions::protection);
        protection = memory::detail::PairwiseMemoryAccess::protection_evidence();
    }
    const auto* post_option = parsed_node_inside(nodes, MemoryDefinitions::post_option, region);
    std::optional<memory::PostRunPlacementEvidence> post;
    if (!post_option)
        return reject(MemoryDefinitions::post_option);
    if (post_option->present) {
        const auto* post_node = parsed_node_inside(nodes, MemoryDefinitions::post, *post_option);
        auto parsed = post_node ? parse_placement(*post_node, false, storage_->post_counts)
                                : std::nullopt;
        if (!parsed)
            return reject(MemoryDefinitions::post);
        auto& [state, observation, evaluation, qualification] = *parsed;
        const bool observed = state == "observed";
        if ((!observed && state != "skipped" && state != "not_applicable") ||
            observed != static_cast<bool>(observation && evaluation && qualification))
            return reject(MemoryDefinitions::post);
        if (observed) {
            if (!memory_observation_valid(request, *observation))
                return reject(MemoryDefinitions::post);
            const auto derived = memory::detail::PlacementEvaluatedRegionAccess::evaluate_region_placement(
                request, *observation);
            if (!derived || derived->qualification_state != *qualification ||
                derived->evaluation.requested_node_pages() != evaluation->requested_node_pages() ||
                derived->evaluation.unexpected_node_pages() != evaluation->unexpected_node_pages() ||
                derived->evaluation.unresolved_pages() != evaluation->unresolved_pages() ||
                derived->evaluation.observed_ratio() != evaluation->observed_ratio())
                return reject(MemoryDefinitions::post);
        }
        post = memory::detail::PairwiseMemoryAccess::post_run(
            observed ? memory::PostRunVerificationState::observed
                     : state == "skipped" ? memory::PostRunVerificationState::skipped
                                           : memory::PostRunVerificationState::not_applicable,
            std::move(observation), std::move(evaluation), qualification);
    }
    const auto* cleanup = parsed_node_inside(nodes, MemoryDefinitions::cleanup, region);
    const auto attempted = cleanup
                               ? boolean_inside(body, nodes, MemoryDefinitions::release_attempted, *cleanup)
                               : std::nullopt;
    const auto succeeded = cleanup
                               ? boolean_inside(body, nodes, MemoryDefinitions::release_succeeded, *cleanup)
                               : std::nullopt;
    const auto active = cleanup
                            ? boolean_inside(body, nodes, MemoryDefinitions::active_after, *cleanup)
                            : std::nullopt;
    const auto path = memory_token(body, nodes, MemoryDefinitions::path, region);
    const auto disposition = memory_token(body, nodes, MemoryDefinitions::disposition, region);
    if (!attempted || !succeeded || !active || !path || !disposition ||
        (*attempted ? (*succeeded == *active) : (*succeeded || !*active)))
        return reject(MemoryDefinitions::cleanup);
    std::optional<memory::RegionFinalizationPath> final_path;
    if (*path == "protected_execution") final_path = memory::RegionFinalizationPath::protected_execution;
    else if (*path == "unprotected_execution_ineligible")
        final_path = memory::RegionFinalizationPath::unprotected_execution_ineligible;
    else if (*path == "unprotected_protection_failed")
        final_path = memory::RegionFinalizationPath::unprotected_protection_failed;
    if (!final_path || (*disposition != "succeeded" && *disposition != "failed") ||
        ((*final_path == memory::RegionFinalizationPath::protected_execution) != protection.has_value()) ||
        (*final_path == memory::RegionFinalizationPath::protected_execution &&
         ((*disposition == "succeeded" && !post) ||
          (post && (*verify_after
                       ? post->state() != memory::PostRunVerificationState::observed
                       : post->state() != memory::PostRunVerificationState::skipped)))) ||
        (*final_path != memory::RegionFinalizationPath::protected_execution &&
         (!post || post->state() != memory::PostRunVerificationState::not_applicable)) ||
        (*disposition == "succeeded" && (!*attempted || !*succeeded || *active)))
        return reject(MemoryDefinitions::region);
    return memory::detail::PairwiseMemoryAccess::diagnostic(
        std::move(request), memory::detail::PairwiseMemoryAccess::binding_evidence(),
        memory::detail::PairwiseMemoryAccess::initialization_evidence(*prefault),
        std::move(pre_value), std::move(protection), std::move(post),
        memory::detail::PairwiseMemoryAccess::cleanup(*attempted, *succeeded, *active), *final_path,
        *disposition == "succeeded" ? memory::RegionFinalizationDisposition::succeeded
                                    : memory::RegionFinalizationDisposition::failed);
}

namespace {
struct OuterObservationDefinitions final {
    static constexpr auto factual = required_definition("factual_observation");
    static constexpr auto observation_id =
        required_definition("scalar__factual_observation__observation_id");
    static constexpr auto slot = required_definition("scheduled_slot");
    static constexpr auto slot_block = required_definition("scalar__scheduled_slot__block_id");
    static constexpr auto slot_index = required_definition("scalar__scheduled_slot__slot_index");
    static constexpr auto slot_variant = required_definition("scalar__scheduled_slot__variant_slot");
    static constexpr auto placements =
        required_definition("array__factual_observation__placements");
    static constexpr auto placement = required_definition("placement_worker");
    static constexpr auto placement_worker =
        required_definition("scalar__placement_worker__worker_id");
    static constexpr auto placement_state = required_definition("scalar__placement_worker__state");
    static constexpr auto migrations =
        required_definition("array__factual_observation__migrations");
    static constexpr auto migration = required_definition("migration_worker");
    static constexpr auto migration_worker =
        required_definition("scalar__migration_worker__worker_id");
    static constexpr auto migration_state = required_definition("scalar__migration_worker__state");
};

auto placement_state(std::string_view token) noexcept
    -> std::optional<measurement::WorkerPlacementValidityState> {
    using State = measurement::WorkerPlacementValidityState;
    if (token == "qualified") return State::qualified;
    if (token == "not_qualified") return State::not_qualified;
    if (token == "unavailable") return State::unavailable;
    if (token == "not_applicable") return State::not_applicable;
    return std::nullopt;
}

auto migration_state(std::string_view token) noexcept
    -> std::optional<measurement::WorkerMigrationValidityState> {
    using State = measurement::WorkerMigrationValidityState;
    if (token == "not_migrated") return State::not_migrated;
    if (token == "migrated") return State::migrated;
    if (token == "unavailable") return State::unavailable;
    if (token == "not_applicable") return State::not_applicable;
    return std::nullopt;
}

auto projected_worker_outcome(
    std::span<const measurement::WorkerPlacementValidityEvidence> values) noexcept
    -> measurement::ObservationValidityCheckOutcome {
    using State = measurement::WorkerPlacementValidityState;
    if (std::ranges::any_of(values, [](const auto& item) { return item.state() == State::not_qualified; }))
        return measurement::ObservationValidityCheckOutcome::fail;
    if (std::ranges::any_of(values, [](const auto& item) { return item.state() == State::unavailable; }))
        return measurement::ObservationValidityCheckOutcome::unavailable;
    if (std::ranges::all_of(values, [](const auto& item) { return item.state() == State::not_applicable; }))
        return measurement::ObservationValidityCheckOutcome::not_applicable;
    return measurement::ObservationValidityCheckOutcome::pass;
}

auto projected_worker_outcome(
    std::span<const measurement::WorkerMigrationValidityEvidence> values) noexcept
    -> measurement::ObservationValidityCheckOutcome {
    using State = measurement::WorkerMigrationValidityState;
    if (std::ranges::any_of(values, [](const auto& item) { return item.state() == State::migrated; }))
        return measurement::ObservationValidityCheckOutcome::fail;
    if (std::ranges::any_of(values, [](const auto& item) { return item.state() == State::unavailable; }))
        return measurement::ObservationValidityCheckOutcome::unavailable;
    if (std::ranges::all_of(values, [](const auto& item) { return item.state() == State::not_applicable; }))
        return measurement::ObservationValidityCheckOutcome::not_applicable;
    return measurement::ObservationValidityCheckOutcome::pass;
}
} // namespace

struct PreparedFactualObservationPool::Storage final {
    std::array<std::vector<core::LinuxCpuId>, 2> selected_cpus;
    struct Entry final {
        PreparedMeasurementValidityBuilder measurement;
        PreparedMonitoringBuilder monitoring;
        std::vector<core::WorkerId> worker_ids;
        std::vector<measurement::WorkerPlacementValidityEvidence> placements;
        std::vector<measurement::WorkerMigrationValidityEvidence> migrations;
        bool consumed{};
    };
    std::vector<Entry> entries;
};

PreparedFactualObservationPool::~PreparedFactualObservationPool() noexcept = default;
PreparedFactualObservationPool::PreparedFactualObservationPool(
    PreparedFactualObservationPool&&) noexcept = default;
PreparedFactualObservationPool::PreparedFactualObservationPool(
    std::unique_ptr<Storage> storage) noexcept : storage_{std::move(storage)} {}

auto PreparedFactualObservationPool::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedFactualObservationPool,
                     prepared_resources_v1::PreparedResourceError> {
    return prepare_impl(resource, nullptr, nullptr);
}

auto PreparedFactualObservationPool::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource,
    const wire_v1::CalculatedEvidenceCapacity& capacity,
    const detail::PairwiseHostCapacityRequestTranscript& request) noexcept
    -> std::expected<PreparedFactualObservationPool, prepared_resources_v1::PreparedResourceError> {
    return prepare_impl(resource, &capacity, &request);
}

auto PreparedFactualObservationPool::prepare_impl(
    const prepared_resources_v1::PreparedResourceValue& resource,
    const wire_v1::CalculatedEvidenceCapacity* capacity,
    const detail::PairwiseHostCapacityRequestTranscript* request) noexcept
    -> std::expected<PreparedFactualObservationPool, prepared_resources_v1::PreparedResourceError> {
    try {
        if (resource.definitions.size() != wire_v1::detail::schema_nodes.size())
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        const auto owners = resource.definitions[OuterObservationDefinitions::factual].owner_occurrences;
        const auto placements = stock_cardinality(resource, OuterObservationDefinitions::placements);
        const auto migrations = stock_cardinality(resource, OuterObservationDefinitions::migrations);
        const auto bandwidth_workers = stock_cardinality(resource, ObservationDefinitions::bw_array);
        const auto latency_workers = stock_cardinality(resource, ObservationDefinitions::lat_array);
        if (!placements || !migrations || !bandwidth_workers || !latency_workers ||
            *placements != *migrations)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        auto storage = std::make_unique<Storage>();
        if (request && capacity) {
            const std::array sides{&request->first(), &request->second()};
            for (std::size_t side{}; side != sides.size(); ++side) {
                if (sides[side]->workers().size() != capacity->prerequisite.parameters.workers.value)
                    return std::unexpected{prepared_resources_v1::PreparedResourceError{
                        prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
                storage->selected_cpus[side].reserve(sides[side]->workers().size());
                for (const auto& worker : sides[side]->workers())
                    storage->selected_cpus[side].push_back(worker.cpu().linux_cpu());
            }
        }
        if (owners > std::numeric_limits<std::size_t>::max())
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::arithmetic_overflow}};
        storage->entries.reserve(static_cast<std::size_t>(owners));
        for (std::uint64_t ordinal{}; ordinal < owners; ++ordinal) {
            auto measurement = PreparedMeasurementValidityBuilder::prepare(resource);
            auto monitoring = PreparedMonitoringBuilder::prepare_for_factual_owner(resource, owners,
                capacity ? std::optional{capacity->prerequisite.parameters.present_cpu_ids.size()}
                         : std::nullopt);
            if (!measurement || !monitoring)
                return std::unexpected{!measurement ? measurement.error() : monitoring.error()};
            std::vector<measurement::WorkerPlacementValidityEvidence> placement_values;
            std::vector<measurement::WorkerMigrationValidityEvidence> migration_values;
            std::vector<core::WorkerId> worker_ids;
            worker_ids.reserve(std::max(*bandwidth_workers, *latency_workers));
            placement_values.reserve(*placements);
            migration_values.reserve(*migrations);
            storage->entries.push_back(Storage::Entry{
                std::move(*measurement), std::move(*monitoring), std::move(worker_ids),
                std::move(placement_values), std::move(migration_values), false});
        }
        return PreparedFactualObservationPool{std::move(storage)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedFactualObservationPool::build(
    std::size_t ordinal, std::span<const char> body,
    std::span<const PreparedParsedNode> nodes, const PreparedParsedNode& factual,
    const measurement::ScheduledObservationSlot& authenticated_slot,
    std::span<const core::LinuxCpuId> authenticated_cpus) noexcept
    -> std::expected<FactualObservationEvidence, ActiveCodecFailure> {
    const auto reject = [](ActivePipelineFailureKind kind, std::uint32_t definition,
                           std::uint64_t limit = 0U, std::uint64_t observed = 0U) {
        auto value = ActiveCodecFailure{kind, ActivePipelineStage::active, definition,
                                        limit, observed, std::nullopt};
        return std::expected<FactualObservationEvidence, ActiveCodecFailure>{
            std::unexpected{value}};
    };
    if (!storage_ || ordinal >= storage_->entries.size())
        return reject(ActivePipelineFailureKind::capacity_invariant,
                      OuterObservationDefinitions::factual,
                      storage_ ? storage_->entries.size() : 0U, ordinal + 1U);
    auto& entry = storage_->entries[ordinal];
    if (entry.consumed)
        return reject(ActivePipelineFailureKind::transport_state,
                      OuterObservationDefinitions::factual);
    entry.consumed = true;
    if (factual.definition != OuterObservationDefinitions::factual ||
        factual.occurrence != ordinal)
        return reject(ActivePipelineFailureKind::ledger_validation,
                      OuterObservationDefinitions::factual);
    const auto observation_id = unsigned_inside(body, nodes,
        OuterObservationDefinitions::observation_id, factual);
    const auto* slot = parsed_node_inside(nodes, OuterObservationDefinitions::slot, factual);
    const auto block = slot ? unsigned_inside(body, nodes, OuterObservationDefinitions::slot_block, *slot)
                            : std::nullopt;
    const auto index = slot ? unsigned_inside(body, nodes, OuterObservationDefinitions::slot_index, *slot)
                            : std::nullopt;
    const auto variant = slot ? observation_token(body, nodes,
        OuterObservationDefinitions::slot_variant, *slot) : std::nullopt;
    const auto expected_variant = authenticated_slot.comparison_variant() ==
        measurement::ComparisonVariantSlot::first ? "first" : "second";
    if (!observation_id || !slot || !block || !index || !variant ||
        *block != authenticated_slot.block_id().value() ||
        *index != authenticated_slot.slot_index() || *variant != expected_variant)
        return reject(ActivePipelineFailureKind::ledger_validation,
                      OuterObservationDefinitions::slot);

    auto reconstructed = entry.measurement.build(body, nodes, factual);
    if (!reconstructed)
        return std::unexpected{reconstructed.error()};
    bool measurement_block_matches{};
    const bool worker_capacity = std::visit([&](const auto& measured) {
        measurement_block_matches = measured.block_id() == authenticated_slot.block_id();
        if (!measurement_block_matches || measured.workers().size() > entry.worker_ids.capacity())
            return false;
        for (const auto& worker : measured.workers()) entry.worker_ids.push_back(worker.worker_id());
        return true;
    }, reconstructed->observation);
    if (!measurement_block_matches)
        return reject(ActivePipelineFailureKind::ledger_validation,
                      OuterObservationDefinitions::slot);
    if (!worker_capacity)
        return reject(ActivePipelineFailureKind::capacity_invariant,
                      OuterObservationDefinitions::placements, entry.worker_ids.capacity(),
                      entry.worker_ids.size());

    const auto parse_associations = [&](std::uint32_t array_definition,
                                        std::uint32_t item_definition,
                                        std::uint32_t worker_definition,
                                        std::uint32_t state_definition, auto& destination,
                                        auto parse_state) -> bool {
        const auto* array = parsed_node_inside(nodes, array_definition, factual);
        if (!array || array->element_count != entry.worker_ids.size() ||
            array->element_count > destination.capacity()) return false;
        for (std::uint64_t item_index{}; item_index < array->element_count; ++item_index) {
            const auto* item = parsed_node_inside(nodes, item_definition, *array, item_index);
            const auto worker = item ? unsigned_inside(body, nodes, worker_definition, *item)
                                     : std::nullopt;
            const auto state = item ? observation_token(body, nodes, state_definition, *item)
                                    : std::nullopt;
            const auto decoded = state ? parse_state(*state) : std::nullopt;
            if (!worker || *worker > std::numeric_limits<std::uint32_t>::max() || !decoded ||
                core::WorkerId{static_cast<std::uint32_t>(*worker)} != entry.worker_ids[item_index]) return false;
            destination.push_back(std::decay_t<decltype(destination.front())>::create(
                entry.worker_ids[item_index], *decoded));
        }
        return true;
    };
    if (!parse_associations(OuterObservationDefinitions::placements,
                            OuterObservationDefinitions::placement,
                            OuterObservationDefinitions::placement_worker,
                            OuterObservationDefinitions::placement_state, entry.placements,
                            placement_state) ||
        !parse_associations(OuterObservationDefinitions::migrations,
                            OuterObservationDefinitions::migration,
                            OuterObservationDefinitions::migration_worker,
                            OuterObservationDefinitions::migration_state, entry.migrations,
                            migration_state))
        return reject(ActivePipelineFailureKind::ledger_validation,
                      OuterObservationDefinitions::placements);

    const auto interval = std::visit([](const auto& measured) {
        return std::pair{measured.enclosing_interval().start(), measured.enclosing_interval().end()};
    }, reconstructed->observation);
    auto monitoring = entry.monitoring.build(body, nodes, factual,
        entry.worker_ids, authenticated_cpus,
        interval.first, interval.second, storage_->selected_cpus[
            authenticated_slot.comparison_variant() == measurement::ComparisonVariantSlot::first ? 0U : 1U]);
    if (!monitoring)
        return std::unexpected{monitoring.error()};
    const auto& checks = reconstructed->validity.checks();
    if (checks[3].outcome() != projected_worker_outcome(entry.placements) ||
        checks[4].outcome() != projected_worker_outcome(entry.migrations))
        return reject(ActivePipelineFailureKind::ledger_validation,
                      ObservationDefinitions::validity);
    const auto monitor_outcome = [](monitoring::MonitoringSignalState state,
                                    bool detected_is_failure) {
        using Outcome = measurement::ObservationValidityCheckOutcome;
        switch (state) {
        case monitoring::MonitoringSignalState::clear: return Outcome::pass;
        case monitoring::MonitoringSignalState::detected:
            return detected_is_failure ? Outcome::fail : Outcome::warning;
        case monitoring::MonitoringSignalState::unavailable: return Outcome::unavailable;
        case monitoring::MonitoringSignalState::not_applicable: return Outcome::not_applicable;
        }
        return Outcome::unavailable;
    };
    const auto page = *monitoring ? (*monitoring)->evidence().page_faults_state()
                                  : monitoring::MonitoringSignalState::not_applicable;
    const auto context = *monitoring ? (*monitoring)->evidence().context_switches_state()
                                     : monitoring::MonitoringSignalState::not_applicable;
    const auto frequency = *monitoring ? (*monitoring)->evidence().frequency_state()
                                       : monitoring::MonitoringSignalState::not_applicable;
    const auto temperature = *monitoring ? (*monitoring)->evidence().temperature_state()
                                         : monitoring::MonitoringSignalState::not_applicable;
    if (checks[5].outcome() != monitor_outcome(page, true) ||
        checks[6].outcome() != measurement::ObservationValidityCheckOutcome::not_applicable ||
        checks[7].outcome() != monitor_outcome(context, false) ||
        checks[8].outcome() != monitor_outcome(frequency, false) ||
        checks[9].outcome() != monitor_outcome(temperature, false))
        return reject(ActivePipelineFailureKind::ledger_validation,
                      ObservationDefinitions::validity);
    return FactualObservationEvidence{authenticated_slot, core::ObservationId{*observation_id},
        std::move(reconstructed->observation), std::move(reconstructed->validity),
        std::move(*monitoring), std::move(entry.placements), std::move(entry.migrations)};
}

namespace {
struct BlockDefinitions final {
    static constexpr auto block = required_definition("block");
    static constexpr auto plan = required_definition("block_plan");
    static constexpr auto block_id = required_definition("scalar__block_plan__block_id");
    static constexpr auto order = required_definition("comparison_order");
    static constexpr auto strategy = required_definition("scalar__comparison_order__strategy");
    static constexpr auto seed = required_definition("scalar__comparison_order__seed");
    static constexpr auto preparation = required_definition("block_preparation");
    static constexpr auto calibration =
        required_definition("scalar__block_preparation__calibration_required");
    static constexpr auto warmup =
        required_definition("scalar__block_preparation__warmup_required");
    static constexpr auto plan_slots = required_definition("array__block_plan__observations");
    static constexpr auto observations = required_definition("array__block__observations");
    static constexpr auto failed_option = required_definition("option__block__failed_observation");
    static constexpr auto failed = required_definition("failed_observation");
    static constexpr auto failed_id =
        required_definition("scalar__failed_observation__observation_id");
    static constexpr auto failed_workers =
        required_definition("array__failed_observation__worker_errors");
    static constexpr auto error = required_definition("finalized_error");
    static constexpr auto completion = required_definition("scalar__block__completion");
};
} // namespace

struct PreparedBlockPool::Storage final {
    struct Entry final {
        std::vector<FactualObservationEvidence> observations;
        std::vector<FinalizedEvidenceError> worker_errors;
        bool consumed{};
    };
    PreparedFactualObservationPool factual;
    PreparedFinalizedErrorPool errors;
    std::vector<Entry> entries;
};

PreparedBlockPool::~PreparedBlockPool() noexcept = default;
PreparedBlockPool::PreparedBlockPool(std::unique_ptr<Storage> storage) noexcept
    : storage_{std::move(storage)} {}

auto PreparedBlockPool::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedBlockPool, prepared_resources_v1::PreparedResourceError> {
    return prepare_impl(resource, nullptr, nullptr);
}

auto PreparedBlockPool::prepare(const prepared_resources_v1::PreparedResourceValue& resource,
    const wire_v1::CalculatedEvidenceCapacity& capacity,
    const detail::PairwiseHostCapacityRequestTranscript& request) noexcept
    -> std::expected<PreparedBlockPool, prepared_resources_v1::PreparedResourceError> {
    return prepare_impl(resource, &capacity, &request);
}

auto PreparedBlockPool::prepare_impl(const prepared_resources_v1::PreparedResourceValue& resource,
    const wire_v1::CalculatedEvidenceCapacity* capacity,
    const detail::PairwiseHostCapacityRequestTranscript* request) noexcept
    -> std::expected<PreparedBlockPool, prepared_resources_v1::PreparedResourceError> {
    try {
        if (resource.definitions.size() != wire_v1::detail::schema_nodes.size())
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        const auto owners = resource.definitions[BlockDefinitions::block].owner_occurrences;
        const auto observations = stock_cardinality(resource, BlockDefinitions::observations);
        const auto failed_errors = stock_cardinality(resource, BlockDefinitions::failed_workers);
        auto factual = capacity && request ? PreparedFactualObservationPool::prepare(resource, *capacity, *request)
                                           : PreparedFactualObservationPool::prepare(resource);
        auto errors = PreparedFinalizedErrorPool::prepare(resource);
        if (!observations || !failed_errors || !factual || !errors || observations.value() != 4U ||
            owners > std::numeric_limits<std::size_t>::max())
            return std::unexpected{!factual ? factual.error()
                                   : !errors ? errors.error()
                                   : prepared_resources_v1::PreparedResourceError{
                                         prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        auto storage = std::make_unique<Storage>(Storage{
            std::move(*factual), std::move(*errors), {}});
        storage->entries.reserve(static_cast<std::size_t>(owners));
        for (std::uint64_t i{}; i < owners; ++i) {
            std::vector<FactualObservationEvidence> factual_values;
            std::vector<FinalizedEvidenceError> worker_values;
            factual_values.reserve(*observations);
            worker_values.reserve(*failed_errors);
            storage->entries.push_back(Storage::Entry{
                std::move(factual_values), std::move(worker_values), false});
        }
        return PreparedBlockPool{std::move(storage)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedBlockPool::build(
    std::size_t ordinal, std::span<const char> body,
    std::span<const PreparedParsedNode> nodes, const PreparedParsedNode& block,
    std::span<const core::LinuxCpuId> authenticated_cpus) noexcept
    -> std::expected<PairwiseBlockAttemptEvidence, ActiveCodecFailure> {
    const auto reject = [](ActivePipelineFailureKind kind, std::uint32_t definition,
                           std::uint64_t limit = 0U, std::uint64_t observed = 0U) {
        return std::expected<PairwiseBlockAttemptEvidence, ActiveCodecFailure>{std::unexpected{
            ActiveCodecFailure{kind, ActivePipelineStage::active, definition, limit, observed,
                               std::nullopt}}};
    };
    if (!storage_ || ordinal >= storage_->entries.size())
        return reject(ActivePipelineFailureKind::capacity_invariant, BlockDefinitions::block,
                      storage_ ? storage_->entries.size() : 0U, ordinal + 1U);
    auto& entry = storage_->entries[ordinal];
    if (entry.consumed)
        return reject(ActivePipelineFailureKind::transport_state, BlockDefinitions::block);
    entry.consumed = true;
    if (block.definition != BlockDefinitions::block || block.occurrence != ordinal)
        return reject(ActivePipelineFailureKind::ledger_validation, BlockDefinitions::block);
    const auto* plan_node = parsed_node_inside(nodes, BlockDefinitions::plan, block);
    const auto id = plan_node ? unsigned_inside(body, nodes, BlockDefinitions::block_id, *plan_node)
                              : std::nullopt;
    const auto* order = plan_node ? parsed_node_inside(nodes, BlockDefinitions::order, *plan_node)
                                  : nullptr;
    const auto strategy_token = order ? observation_token(body, nodes, BlockDefinitions::strategy, *order)
                                      : std::nullopt;
    const auto seed = order ? unsigned_inside(body, nodes, BlockDefinitions::seed, *order)
                            : std::nullopt;
    const auto* preparation = plan_node
        ? parsed_node_inside(nodes, BlockDefinitions::preparation, *plan_node) : nullptr;
    const auto calibration = preparation
        ? boolean_inside(body, nodes, BlockDefinitions::calibration, *preparation) : std::nullopt;
    const auto warmup = preparation
        ? boolean_inside(body, nodes, BlockDefinitions::warmup, *preparation) : std::nullopt;
    std::optional<measurement::ComparisonOrderStrategy> strategy;
    if (strategy_token == "randomized_abba") strategy = measurement::ComparisonOrderStrategy::randomized_abba;
    else if (strategy_token == "randomized_baab") strategy = measurement::ComparisonOrderStrategy::randomized_baab;
    else if (strategy_token == "randomized_balanced") strategy = measurement::ComparisonOrderStrategy::randomized_balanced;
    if (!plan_node || !id || *id != ordinal || !strategy || !seed || !calibration || !warmup)
        return reject(ActivePipelineFailureKind::ledger_validation, BlockDefinitions::plan);
    auto plan = measurement::MeasurementBlockPlan::create(
        kernel::DependentLoadLatencyBlockId{*id},
        measurement::ComparisonOrderPolicy::create(*strategy, *seed),
        measurement::BlockPreparationPlan::create(*calibration, *warmup));
    const auto* stated_slots = parsed_node_inside(nodes, BlockDefinitions::plan_slots, *plan_node);
    if (!stated_slots || stated_slots->element_count != plan.observations().size())
        return reject(ActivePipelineFailureKind::ledger_validation, BlockDefinitions::plan_slots);
    for (std::size_t i{}; i < plan.observations().size(); ++i) {
        const auto* stated = parsed_node_inside(nodes, OuterObservationDefinitions::slot,
                                               *stated_slots, i);
        const auto stated_block = stated ? unsigned_inside(body, nodes,
            OuterObservationDefinitions::slot_block, *stated) : std::nullopt;
        const auto stated_index = stated ? unsigned_inside(body, nodes,
            OuterObservationDefinitions::slot_index, *stated) : std::nullopt;
        const auto stated_variant = stated ? observation_token(body, nodes,
            OuterObservationDefinitions::slot_variant, *stated) : std::nullopt;
        const auto& expected = plan.observations()[i];
        const auto variant = expected.comparison_variant() == measurement::ComparisonVariantSlot::first
            ? "first" : "second";
        if (!stated || !stated_block || !stated_index || !stated_variant ||
            *stated_block != *id || *stated_index != i || *stated_variant != variant)
            return reject(ActivePipelineFailureKind::ledger_validation,
                          BlockDefinitions::plan_slots);
    }
    const auto* observations = parsed_node_inside(nodes, BlockDefinitions::observations, block);
    if (!observations || observations->element_count > plan.observations().size() ||
        observations->element_count > entry.observations.capacity())
        return reject(ActivePipelineFailureKind::ledger_validation, BlockDefinitions::observations);
    for (std::size_t i{}; i < observations->element_count; ++i) {
        const auto* factual = parsed_node_inside(nodes, OuterObservationDefinitions::factual,
                                                *observations, i);
        if (!factual)
            return reject(ActivePipelineFailureKind::ledger_validation,
                          BlockDefinitions::observations);
        auto value = storage_->factual.build(factual->occurrence, body, nodes, *factual,
                                             plan.observations()[i], authenticated_cpus);
        if (!value) return std::unexpected{value.error()};
        const auto expected_id = static_cast<std::uint64_t>(ordinal) * 4U + i;
        if (value->observation_id.value() != expected_id)
            return reject(ActivePipelineFailureKind::ledger_validation,
                          OuterObservationDefinitions::observation_id);
        entry.observations.push_back(std::move(*value));
    }
    const auto* failed_option = parsed_node_inside(nodes, BlockDefinitions::failed_option, block);
    const auto* failed = failed_option && failed_option->present
        ? parsed_node_inside(nodes, BlockDefinitions::failed, *failed_option) : nullptr;
    const auto completion = observation_token(body, nodes, BlockDefinitions::completion, block);
    std::optional<BlockAttemptCompletion> completed;
    if (completion == "complete_valid") completed = BlockAttemptCompletion::complete_valid;
    else if (completion == "complete_invalid") completed = BlockAttemptCompletion::complete_invalid;
    else if (completion == "operationally_incomplete") completed = BlockAttemptCompletion::operationally_incomplete;
    if (!failed_option || !completion || !completed)
        return reject(ActivePipelineFailureKind::ledger_validation, BlockDefinitions::completion);
    std::optional<FailedObservationEvidence> failed_value;
    if (failed) {
        const auto slot_index = entry.observations.size();
        const auto failed_id = unsigned_inside(body, nodes, BlockDefinitions::failed_id, *failed);
        const auto* failed_slot = parsed_node_inside(nodes, OuterObservationDefinitions::slot, *failed);
        const auto* primary = parsed_node_inside(nodes, BlockDefinitions::error, *failed);
        const auto* workers = parsed_node_inside(nodes, BlockDefinitions::failed_workers, *failed);
        if (slot_index >= plan.observations().size() || !failed_id || !failed_slot || !primary ||
            !workers || workers->element_count > entry.worker_errors.capacity() ||
            *failed_id != static_cast<std::uint64_t>(ordinal) * 4U + slot_index)
            return reject(ActivePipelineFailureKind::ledger_validation, BlockDefinitions::failed);
        const auto& expected_slot = plan.observations()[slot_index];
        const auto slot_block = unsigned_inside(body, nodes, OuterObservationDefinitions::slot_block, *failed_slot);
        const auto stated_index = unsigned_inside(body, nodes, OuterObservationDefinitions::slot_index, *failed_slot);
        const auto stated_variant = observation_token(body, nodes, OuterObservationDefinitions::slot_variant, *failed_slot);
        const auto variant = expected_slot.comparison_variant() == measurement::ComparisonVariantSlot::first ? "first" : "second";
        if (!slot_block || !stated_index || !stated_variant || *slot_block != *id ||
            *stated_index != slot_index || *stated_variant != variant)
            return reject(ActivePipelineFailureKind::ledger_validation, BlockDefinitions::failed);
        auto primary_value = storage_->errors.build(primary->occurrence, body, nodes, *primary);
        if (!primary_value) return std::unexpected{primary_value.error()};
        for (std::size_t i{}; i < workers->element_count; ++i) {
            const auto* error = parsed_node_inside(nodes, BlockDefinitions::error, *workers, i);
            if (!error) return reject(ActivePipelineFailureKind::ledger_validation, BlockDefinitions::failed_workers);
            auto value = storage_->errors.build(error->occurrence, body, nodes, *error);
            if (!value) return std::unexpected{value.error()};
            entry.worker_errors.push_back(std::move(*value));
        }
        failed_value.emplace(FailedObservationEvidence{expected_slot, core::ObservationId{*failed_id},
            std::move(*primary_value), std::move(entry.worker_errors)});
    }
    const bool all_valid = std::ranges::all_of(entry.observations, [](const auto& value) {
        return value.validity.disposition() == measurement::ObservationValidityDisposition::valid;
    });
    if ((*completed == BlockAttemptCompletion::operationally_incomplete &&
         (!failed_value || entry.observations.size() >= plan.observations().size())) ||
        (*completed != BlockAttemptCompletion::operationally_incomplete &&
         (failed_value || entry.observations.size() != plan.observations().size())) ||
        (*completed == BlockAttemptCompletion::complete_valid && !all_valid) ||
        (*completed == BlockAttemptCompletion::complete_invalid && all_valid))
        return reject(ActivePipelineFailureKind::ledger_validation, BlockDefinitions::completion);
    return PairwiseBlockAttemptEvidence{std::move(plan), std::move(entry.observations),
                                        std::move(failed_value), *completed};
}

namespace {
struct TerminalDefinitions final {
    static constexpr auto stopping_array = required_definition("array__root__stopping_evaluations");
    static constexpr auto stopping = required_definition("stopping");
    static constexpr auto boundary = required_definition("scalar__stopping__boundary_block_id");
    static constexpr auto completed = required_definition("scalar__stopping__completed_blocks");
    static constexpr auto valid_blocks = required_definition("scalar__stopping__complete_valid_blocks");
    static constexpr auto first = required_definition("scalar__stopping__valid_first_observations");
    static constexpr auto second = required_definition("scalar__stopping__valid_second_observations");
    static constexpr auto bootstrap = required_definition("bootstrap");
    static constexpr auto bootstrap_policy = required_definition("bootstrap_policy");
    static constexpr auto bootstrap_count = required_definition("scalar__bootstrap_policy__replicate_count");
    static constexpr auto bootstrap_confidence = required_definition("scalar__bootstrap_policy__confidence");
    static constexpr auto bootstrap_seed = required_definition("scalar__bootstrap_policy__seed");
    static constexpr auto log_lower = required_definition("scalar__bootstrap__log_lower");
    static constexpr auto log_upper = required_definition("scalar__bootstrap__log_upper");
    static constexpr auto ratio_lower = required_definition("scalar__bootstrap__ratio_lower");
    static constexpr auto ratio_upper = required_definition("scalar__bootstrap__ratio_upper");
    static constexpr auto evaluation = required_definition("adaptive_evaluation");
    static constexpr auto observed = required_definition("scalar__adaptive_evaluation__observed_relative_half_width");
    static constexpr auto attainment = required_definition("scalar__adaptive_evaluation__attainment");
    static constexpr auto decision = required_definition("scalar__stopping__decision");
    static constexpr auto subsequent = required_definition("scalar__stopping__subsequent_disposition");
    static constexpr auto terminal = required_definition("terminal");
    static constexpr auto operational = required_definition("operational_terminal_branch");
    static constexpr auto kind = required_definition("scalar__terminal__kind");
    static constexpr auto outcome = required_definition("repetition_outcome");
    static constexpr auto repetition_policy = required_definition("repetition_policy");
    static constexpr auto minimum_blocks = required_definition("scalar__repetition_policy__minimum_blocks");
    static constexpr auto maximum_blocks = required_definition("scalar__repetition_policy__maximum_blocks");
    static constexpr auto minimum_valid = required_definition("scalar__repetition_policy__minimum_valid_observations_per_variant");
    static constexpr auto stopping_enabled = required_definition("scalar__repetition_policy__stopping_enabled");
    static constexpr auto outcome_experiment = required_definition("scalar__repetition_outcome__experiment_id");
    static constexpr auto outcome_disposition = required_definition("scalar__repetition_outcome__disposition");
    static constexpr auto outcome_completed = required_definition("scalar__repetition_outcome__completed_blocks");
    static constexpr auto outcome_valid = required_definition("scalar__repetition_outcome__complete_valid_blocks");
    static constexpr auto outcome_first = required_definition("scalar__repetition_outcome__valid_first_variant_observations");
    static constexpr auto outcome_second = required_definition("scalar__repetition_outcome__valid_second_variant_observations");
    static constexpr auto error = required_definition("finalized_error");
    static constexpr auto secondary = required_definition("array__terminal__secondary_errors");
    static constexpr auto active_option = required_definition("option__terminal__active_block_id");
    static constexpr auto active_id = required_definition("block_id");
    static constexpr auto finalization_option = required_definition("optional_finalization");
    static constexpr auto finalization = required_definition("finalization");
    static constexpr auto variant = required_definition("finalized_variant");
    static constexpr auto variant_slot = required_definition("scalar__finalized_variant__slot");
    static constexpr auto workers = required_definition("array__finalized_variant__workers");
    static constexpr auto worker = required_definition("finalized_worker");
    static constexpr auto worker_id = required_definition("scalar__finalized_worker__worker_id");
    static constexpr auto region = required_definition("region_diagnostic");
    static constexpr auto primary_option = required_definition("option__finalized_worker__primary_error_ordinal");
    static constexpr auto primary_ordinal = required_definition("u64");
    static constexpr auto cleanup_option = required_definition("option__finalized_worker__cleanup_error_ordinal");
    static constexpr auto cleanup_ordinal = required_definition("u64");
};

auto option_unsigned_inside(std::span<const char> body,
                            std::span<const PreparedParsedNode> nodes,
                            std::uint32_t option_definition, std::uint32_t scalar_definition,
                            const PreparedParsedNode& parent) noexcept
    -> std::optional<std::optional<std::uint64_t>> {
    const auto* option = parsed_node_inside(nodes, option_definition, parent);
    if (!option) return std::nullopt;
    if (!option->present) return std::optional<std::uint64_t>{};
    const auto value = unsigned_inside(body, nodes, scalar_definition, *option);
    return value ? std::optional<std::optional<std::uint64_t>>{*value} : std::nullopt;
}

auto binary64_inside(std::span<const char> body, std::span<const PreparedParsedNode> nodes,
                     std::uint32_t definition, const PreparedParsedNode& parent) noexcept
    -> std::optional<double> {
    const auto* node = parsed_node_inside(nodes, definition, parent);
    const auto view = node ? lexical(body, *node) : std::nullopt;
    const auto payload = view ? quoted_payload(*view) : std::nullopt;
    if (!payload || payload->size() != 16U) return std::nullopt;
    std::uint64_t bits{};
    for (const char character : *payload) {
        const auto digit = character >= '0' && character <= '9'
            ? static_cast<std::uint64_t>(character - '0')
            : character >= 'a' && character <= 'f'
                ? static_cast<std::uint64_t>(character - 'a' + 10) : 16U;
        if (digit == 16U) return std::nullopt;
        bits = (bits << 4U) | digit;
    }
    const auto value = std::bit_cast<double>(bits);
    return std::isfinite(value) ? std::optional{value} : std::nullopt;
}
} // namespace

struct PreparedTerminalReconstruction::Storage final {
    std::vector<RunnerStoppingEvaluationEvidence> stopping;
    std::vector<FinalizedEvidenceError> secondary;
    std::array<std::vector<FinalizedWorkerEvidence>, 2> workers;
    std::vector<PreparedMemoryDiagnosticBuilder> regions;
    PreparedFinalizedErrorPool errors;
    std::vector<std::uint8_t> ordinal_used;
    std::optional<std::uint64_t> previous_ordinal;
    bool consumed{};
};

PreparedTerminalReconstruction::~PreparedTerminalReconstruction() noexcept = default;
PreparedTerminalReconstruction::PreparedTerminalReconstruction(
    std::unique_ptr<Storage> storage) noexcept : storage_{std::move(storage)} {}

auto PreparedTerminalReconstruction::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedTerminalReconstruction,
                     prepared_resources_v1::PreparedResourceError> {
    try {
        if (resource.definitions.size() != wire_v1::detail::schema_nodes.size())
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        const auto stopping = stock_cardinality(resource, TerminalDefinitions::stopping_array);
        const auto secondary = stock_cardinality(resource, TerminalDefinitions::secondary);
        const auto workers = stock_cardinality(resource, TerminalDefinitions::workers);
        auto errors = PreparedFinalizedErrorPool::prepare(resource);
        if (!stopping || !secondary || !workers || !errors)
            return std::unexpected{!errors ? errors.error() :
                prepared_resources_v1::PreparedResourceError{
                    prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        auto storage = std::make_unique<Storage>(
            Storage{{}, {}, {}, {}, std::move(*errors), {}, std::nullopt, false});
        storage->stopping.reserve(*stopping);
        storage->secondary.reserve(*secondary);
        for (auto& values : storage->workers) values.reserve(*workers);
        const auto region_count = resource.definitions[TerminalDefinitions::worker].owner_occurrences;
        storage->regions.reserve(static_cast<std::size_t>(region_count));
        for (std::uint64_t i{}; i < region_count; ++i) {
            auto region = PreparedMemoryDiagnosticBuilder::prepare(resource);
            if (!region) return std::unexpected{region.error()};
            storage->regions.push_back(std::move(*region));
        }
        const auto error_count = resource.definitions[TerminalDefinitions::error].owner_occurrences;
        storage->ordinal_used.resize(static_cast<std::size_t>(error_count));
        return PreparedTerminalReconstruction{std::move(storage)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedTerminalReconstruction::build(
    std::span<const char> body, std::span<const PreparedParsedNode> nodes,
    const wire_v1::detail::ReconstructedEvidenceHeader& header,
    std::span<const PairwiseBlockAttemptEvidence> blocks) noexcept
    -> std::expected<std::pair<std::vector<RunnerStoppingEvaluationEvidence>,
                               wire_v1::detail::ReconstructedTerminalAndFinalization>,
                     ActiveCodecFailure> {
    return build(body, nodes, header.statistics_policy, header.repetition_policy,
                 header.provenance.experiment_id, blocks);
}

auto PreparedTerminalReconstruction::build(
    std::span<const char> body, std::span<const PreparedParsedNode> nodes,
    const StatisticsProductionPolicy& statistics_policy,
    const measurement::RepetitionPolicy& repetition_policy,
    core::ExperimentId experiment_id,
    std::span<const PairwiseBlockAttemptEvidence> blocks) noexcept
    -> std::expected<std::pair<std::vector<RunnerStoppingEvaluationEvidence>,
                               wire_v1::detail::ReconstructedTerminalAndFinalization>,
                     ActiveCodecFailure> {
    using Result = std::expected<std::pair<std::vector<RunnerStoppingEvaluationEvidence>,
                                           wire_v1::detail::ReconstructedTerminalAndFinalization>,
                                 ActiveCodecFailure>;
    const auto reject = [](std::uint32_t definition) -> Result {
        return std::unexpected{ActiveCodecFailure{ActivePipelineFailureKind::ledger_validation,
            ActivePipelineStage::active, definition, 0U, 0U, std::nullopt}};
    };
    if (!storage_ || storage_->consumed)
        return reject(TerminalDefinitions::terminal);
    storage_->consumed = true;
    const auto* stopping_array = parsed_node(nodes, TerminalDefinitions::stopping_array);
    if (!stopping_array || stopping_array->element_count > storage_->stopping.capacity())
        return reject(TerminalDefinitions::stopping_array);
    std::uint64_t previous{};
    for (std::size_t index{}; index < stopping_array->element_count; ++index) {
        const auto* item = parsed_node_inside(nodes, TerminalDefinitions::stopping, *stopping_array, index);
        const auto boundary = item ? unsigned_inside(body, nodes, TerminalDefinitions::boundary, *item) : std::nullopt;
        const auto completed = item ? unsigned_inside(body, nodes, TerminalDefinitions::completed, *item) : std::nullopt;
        const auto valid = item ? unsigned_inside(body, nodes, TerminalDefinitions::valid_blocks, *item) : std::nullopt;
        const auto first = item ? unsigned_inside(body, nodes, TerminalDefinitions::first, *item) : std::nullopt;
        const auto second = item ? unsigned_inside(body, nodes, TerminalDefinitions::second, *item) : std::nullopt;
        const auto* boot = item ? parsed_node_inside(nodes, TerminalDefinitions::bootstrap, *item) : nullptr;
        const auto* policy = boot ? parsed_node_inside(nodes, TerminalDefinitions::bootstrap_policy, *boot) : nullptr;
        const auto count = policy ? unsigned_inside(body, nodes, TerminalDefinitions::bootstrap_count, *policy) : std::nullopt;
        const auto confidence = policy ? binary64_inside(body, nodes, TerminalDefinitions::bootstrap_confidence, *policy) : std::nullopt;
        const auto seed = policy ? unsigned_inside(body, nodes, TerminalDefinitions::bootstrap_seed, *policy) : std::nullopt;
        const auto lower = boot ? binary64_inside(body, nodes, TerminalDefinitions::log_lower, *boot) : std::nullopt;
        const auto upper = boot ? binary64_inside(body, nodes, TerminalDefinitions::log_upper, *boot) : std::nullopt;
        const auto ratio_lower = boot ? binary64_inside(body, nodes, TerminalDefinitions::ratio_lower, *boot) : std::nullopt;
        const auto ratio_upper = boot ? binary64_inside(body, nodes, TerminalDefinitions::ratio_upper, *boot) : std::nullopt;
        const auto* evaluation = item ? parsed_node_inside(nodes, TerminalDefinitions::evaluation, *item) : nullptr;
        const auto observed = evaluation ? binary64_inside(body, nodes, TerminalDefinitions::observed, *evaluation) : std::nullopt;
        const auto attainment = evaluation ? observation_token(body, nodes, TerminalDefinitions::attainment, *evaluation) : std::nullopt;
        const auto decision = item ? observation_token(body, nodes, TerminalDefinitions::decision, *item) : std::nullopt;
        const auto subsequent = item ? observation_token(body, nodes, TerminalDefinitions::subsequent, *item) : std::nullopt;
        if (!boundary || !completed || !valid || !first || !second || !count || !confidence ||
            !seed || !lower || !upper || !ratio_lower || !ratio_upper || !observed ||
            !attainment || !decision || !subsequent || *boundary == UINT64_MAX ||
            *completed != *boundary + 1U || *valid > *completed || *valid > UINT64_MAX / 2U ||
            *first != 2U * *valid || *second != 2U * *valid ||
            (index && *boundary <= previous) || *lower > *upper || *ratio_lower <= 0.0 ||
            *ratio_lower > *ratio_upper || std::exp(*lower) != *ratio_lower ||
            std::exp(*upper) != *ratio_upper || *observed < 0.0)
            return reject(TerminalDefinitions::stopping);
        std::uint64_t derived_completed{}, derived_valid{};
        bool found{};
        for (const auto& block : blocks) {
            if (block.plan.block_id().value() > *boundary) break;
            derived_completed += block.completion != BlockAttemptCompletion::operationally_incomplete;
            derived_valid += block.completion == BlockAttemptCompletion::complete_valid;
            found = block.plan.block_id().value() == *boundary &&
                    block.completion != BlockAttemptCompletion::operationally_incomplete;
        }
        const bool met = *observed <= statistics_policy.relative_half_width_target().value();
        const auto expected_subsequent = met ? std::string_view{"precision_target_reached"}
            : (*completed >= repetition_policy.maximum_blocks()
                   ? std::string_view{"maximum_blocks_reached"} : std::string_view{"continued"});
        if (!found || derived_completed != *completed || derived_valid != *valid ||
            *count != statistics_policy.stopping_bootstrap_policy().replicate_count().value() ||
            *confidence != statistics_policy.stopping_bootstrap_policy().confidence().value() ||
            *seed != statistics_policy.stopping_bootstrap_policy().seed().value() ||
            *attainment != (met ? "target_met" : "target_not_met") ||
            *decision != (met ? "target_met" : "target_not_met") ||
            *subsequent != expected_subsequent)
            return reject(TerminalDefinitions::stopping);
        auto boot_result = statistics::BlockBootstrapResult{
            statistics_policy.stopping_bootstrap_policy(),
            statistics::detail::BootstrapAccess::log_interval(
                statistics::detail::LogRatioAccess::paired(*lower),
                statistics::detail::LogRatioAccess::paired(*upper)),
            statistics::detail::BootstrapAccess::ratio_interval(
                statistics::detail::LogRatioAccess::ratio(*ratio_lower),
                statistics::detail::LogRatioAccess::ratio(*ratio_upper))};
        storage_->stopping.push_back(RunnerStoppingEvaluationEvidence{
            kernel::DependentLoadLatencyBlockId{*boundary}, *completed, *valid, *first, *second,
            std::move(boot_result), statistics::AdaptiveStoppingEvaluation{
                statistics::detail::AdaptiveStoppingAccess::observed(*observed),
                met ? statistics::PrecisionTargetAttainment::target_met
                    : statistics::PrecisionTargetAttainment::target_not_met},
            met ? measurement::PrecisionStoppingDecision::target_met
                : measurement::PrecisionStoppingDecision::target_not_met,
            met ? RepetitionBoundaryDisposition::precision_target_reached
                : (*completed >= repetition_policy.maximum_blocks()
                       ? RepetitionBoundaryDisposition::maximum_blocks_reached
                       : RepetitionBoundaryDisposition::continued)});
        previous = *boundary;
    }

    const auto* terminal = parsed_node(nodes, TerminalDefinitions::terminal);
    const auto kind = terminal ? observation_token(body, nodes, TerminalDefinitions::kind, *terminal) : std::nullopt;
    if (!terminal || !kind) return reject(TerminalDefinitions::terminal);
    std::optional<PairwiseRunTerminalEvidence> terminal_value;
    std::uint64_t operational_errors{};
    if (*kind == "normal") {
        const auto* outcome = parsed_node_inside(nodes, TerminalDefinitions::outcome, *terminal);
        const auto* policy = outcome ? parsed_node_inside(nodes, TerminalDefinitions::repetition_policy, *outcome) : nullptr;
        const auto minimum = policy ? unsigned_inside(body, nodes, TerminalDefinitions::minimum_blocks, *policy) : std::nullopt;
        const auto maximum = policy ? unsigned_inside(body, nodes, TerminalDefinitions::maximum_blocks, *policy) : std::nullopt;
        const auto minimum_valid = policy ? unsigned_inside(body, nodes, TerminalDefinitions::minimum_valid, *policy) : std::nullopt;
        const auto stopping_enabled = policy ? boolean_inside(body, nodes, TerminalDefinitions::stopping_enabled, *policy) : std::nullopt;
        const auto experiment = outcome ? unsigned_inside(body, nodes, TerminalDefinitions::outcome_experiment, *outcome) : std::nullopt;
        const auto disposition = outcome ? observation_token(body, nodes, TerminalDefinitions::outcome_disposition, *outcome) : std::nullopt;
        const auto completed = outcome ? unsigned_inside(body, nodes, TerminalDefinitions::outcome_completed, *outcome) : std::nullopt;
        const auto valid = outcome ? unsigned_inside(body, nodes, TerminalDefinitions::outcome_valid, *outcome) : std::nullopt;
        const auto first = outcome ? unsigned_inside(body, nodes, TerminalDefinitions::outcome_first, *outcome) : std::nullopt;
        const auto second = outcome ? unsigned_inside(body, nodes, TerminalDefinitions::outcome_second, *outcome) : std::nullopt;
        std::uint64_t derived_completed{}, derived_valid{};
        for (const auto& block : blocks) {
            derived_completed += block.completion != BlockAttemptCompletion::operationally_incomplete;
            derived_valid += block.completion == BlockAttemptCompletion::complete_valid;
        }
        std::optional<measurement::RepetitionDisposition> parsed_disposition;
        if (disposition == "precision_target_reached") parsed_disposition = measurement::RepetitionDisposition::precision_target_reached;
        else if (disposition == "maximum_blocks_reached") parsed_disposition = measurement::RepetitionDisposition::maximum_blocks_reached;
        else if (disposition == "selected_work_unavailable") parsed_disposition = measurement::RepetitionDisposition::selected_work_unavailable;
        if (!outcome || !policy || !minimum || !maximum || !minimum_valid || !stopping_enabled ||
            !experiment || !parsed_disposition || !completed || !valid || !first || !second ||
            *minimum != repetition_policy.minimum_blocks() ||
            *maximum != repetition_policy.maximum_blocks() ||
            *minimum_valid != repetition_policy.minimum_valid_observations_per_variant() ||
            *stopping_enabled != repetition_policy.stopping_enabled() ||
            *experiment != experiment_id.value() || *completed != derived_completed ||
            *valid != derived_valid || *first != 2U * derived_valid || *second != 2U * derived_valid ||
            (*parsed_disposition == measurement::RepetitionDisposition::precision_target_reached &&
             (storage_->stopping.empty() || storage_->stopping.back().subsequent_disposition != RepetitionBoundaryDisposition::precision_target_reached)) ||
            (*parsed_disposition == measurement::RepetitionDisposition::maximum_blocks_reached &&
             *completed != repetition_policy.maximum_blocks()) ||
            (*parsed_disposition == measurement::RepetitionDisposition::selected_work_unavailable &&
             (!blocks.empty() || *completed != 0U)))
            return reject(TerminalDefinitions::outcome);
        terminal_value.emplace(NormalRunTerminalEvidence{measurement::detail::MeasurementLeafAccess::repetition_outcome(
            core::ExperimentId{*experiment}, repetition_policy, *parsed_disposition,
            *completed, *valid, *first, *second)});
    } else if (*kind == "operational") {
        const auto* primary = parsed_node_inside(nodes, TerminalDefinitions::error, *terminal);
        const auto* secondary = parsed_node_inside(nodes, TerminalDefinitions::secondary, *terminal);
        const auto* active_option = parsed_node_inside(nodes, TerminalDefinitions::active_option, *terminal);
        if (!primary || !secondary || !active_option || secondary->element_count > storage_->secondary.capacity())
            return reject(TerminalDefinitions::operational);
        auto primary_value = storage_->errors.build(primary->occurrence, body, nodes, *primary);
        if (!primary_value) return std::unexpected{primary_value.error()};
        for (std::size_t i{}; i < secondary->element_count; ++i) {
            const auto* error = parsed_node_inside(nodes, TerminalDefinitions::error, *secondary, i);
            if (!error) return reject(TerminalDefinitions::secondary);
            auto value = storage_->errors.build(error->occurrence, body, nodes, *error);
            if (!value) return std::unexpected{value.error()};
            storage_->secondary.push_back(std::move(*value));
        }
        std::optional<kernel::DependentLoadLatencyBlockId> active;
        if (active_option->present) {
            const auto id = unsigned_inside(body, nodes, TerminalDefinitions::active_id, *active_option);
            if (!id || blocks.empty() || blocks.back().completion != BlockAttemptCompletion::operationally_incomplete ||
                blocks.back().plan.block_id().value() != *id)
                return reject(TerminalDefinitions::active_option);
            active.emplace(*id);
        }
        operational_errors = 1U + storage_->secondary.size();
        terminal_value.emplace(OperationalRunTerminalEvidence{std::move(*primary_value),
            std::move(storage_->secondary), active});
    } else return reject(TerminalDefinitions::kind);

    std::optional<PairwiseFinalizationEvidence> finalization_value;
    const auto* final_option = parsed_node(nodes, TerminalDefinitions::finalization_option);
    if (!final_option) return reject(TerminalDefinitions::finalization_option);
    if (final_option->present) {
        const auto* finalization = parsed_node_inside(
            nodes, TerminalDefinitions::finalization, *final_option);
        if (!finalization) return reject(TerminalDefinitions::finalization);
        std::size_t region_ordinal{};
        for (std::size_t variant_index{}; variant_index < 2U; ++variant_index) {
            const auto* variant = parsed_node_inside(nodes, TerminalDefinitions::variant, *finalization, variant_index);
            const auto slot = variant ? observation_token(body, nodes, TerminalDefinitions::variant_slot, *variant) : std::nullopt;
            const auto* workers = variant ? parsed_node_inside(nodes, TerminalDefinitions::workers, *variant) : nullptr;
            const auto expected = variant_index == 0U ? std::string_view{"first"} : std::string_view{"second"};
            if (!variant || !slot || *slot != expected || !workers ||
                workers->element_count > storage_->workers[variant_index].capacity())
                return reject(TerminalDefinitions::variant);
            for (std::size_t i{}; i < workers->element_count; ++i, ++region_ordinal) {
                const auto* worker = parsed_node_inside(nodes, TerminalDefinitions::worker, *workers, i);
                const auto id = worker ? unsigned_inside(body, nodes, TerminalDefinitions::worker_id, *worker) : std::nullopt;
                const auto* region = worker ? parsed_node_inside(nodes, TerminalDefinitions::region, *worker) : nullptr;
                const auto primary = worker ? option_unsigned_inside(body, nodes, TerminalDefinitions::primary_option,
                    TerminalDefinitions::primary_ordinal, *worker) : std::nullopt;
                const auto cleanup = worker ? option_unsigned_inside(body, nodes, TerminalDefinitions::cleanup_option,
                    TerminalDefinitions::cleanup_ordinal, *worker) : std::nullopt;
                if (!id || *id != i || *id > UINT32_MAX || !region || !primary || !cleanup ||
                    region_ordinal >= storage_->regions.size())
                    return reject(TerminalDefinitions::worker);
                auto diagnostic = storage_->regions[region_ordinal].build(body, nodes, *region,
                    core::WorkerId{static_cast<std::uint32_t>(*id)});
                if (!diagnostic) return std::unexpected{diagnostic.error()};
                for (auto ordinal : {*primary, *cleanup}) {
                    if (!ordinal) continue;
                    if (*ordinal >= operational_errors || *ordinal >= storage_->ordinal_used.size() ||
                        storage_->ordinal_used[*ordinal]) return reject(TerminalDefinitions::worker);
                    if (storage_->previous_ordinal &&
                        (*storage_->previous_ordinal == UINT64_MAX ||
                         *ordinal != *storage_->previous_ordinal + 1U))
                        return reject(TerminalDefinitions::worker);
                    storage_->ordinal_used[*ordinal] = 1U;
                    storage_->previous_ordinal = *ordinal;
                }
                if (*cleanup && !*primary) return reject(TerminalDefinitions::worker);
                if (*primary && *cleanup && **primary >= **cleanup) return reject(TerminalDefinitions::worker);
                if (const auto* available = diagnostic->available()) {
                    if ((available->disposition() == memory::RegionFinalizationDisposition::succeeded &&
                         (*primary || *cleanup)) ||
                        (available->disposition() == memory::RegionFinalizationDisposition::failed && !*primary))
                        return reject(TerminalDefinitions::worker);
                } else if (!*primary) return reject(TerminalDefinitions::worker);
                storage_->workers[variant_index].push_back(FinalizedWorkerEvidence{
                    core::WorkerId{static_cast<std::uint32_t>(*id)}, std::move(*diagnostic),
                    *primary, *cleanup});
            }
        }
        if (storage_->workers[0].empty() || storage_->workers[0].size() != storage_->workers[1].size())
            return reject(TerminalDefinitions::finalization);
        finalization_value.emplace(PairwiseFinalizationEvidence{
            FinalizedVariantEvidence{measurement::ComparisonVariantSlot::first, std::move(storage_->workers[0])},
            FinalizedVariantEvidence{measurement::ComparisonVariantSlot::second, std::move(storage_->workers[1])}});
    }
    if (!finalization_value && (*kind == "normal" || !blocks.empty() || !storage_->stopping.empty()))
        return reject(TerminalDefinitions::finalization);
    if (*kind == "normal") {
        for (const auto* variant : {&finalization_value->first, &finalization_value->second})
            for (const auto& worker : variant->workers) {
                const auto* available = worker.region.available();
                if (!available || worker.primary_error_ordinal || worker.cleanup_error_ordinal ||
                    available->disposition() != memory::RegionFinalizationDisposition::succeeded ||
                    !available->cleanup_evidence().release_attempted() ||
                    !available->cleanup_evidence().release_succeeded() ||
                    available->cleanup_evidence().mapping_active_after() || !available->post_run())
                    return reject(TerminalDefinitions::finalization);
                if (available->request().verify_after()) {
                    if (available->post_run()->state() !=
                            memory::PostRunVerificationState::observed ||
                        available->post_run()->qualification_state() !=
                            memory::PlacementQualificationState::qualified)
                        return reject(TerminalDefinitions::finalization);
                } else if (available->post_run()->state() !=
                           memory::PostRunVerificationState::skipped) {
                    return reject(TerminalDefinitions::finalization);
                }
            }
    }
    if (finalization_value) {
        const std::array<std::span<const FinalizedWorkerEvidence>, 2U> admitted{
            finalization_value->first.workers, finalization_value->second.workers};
        for (const auto& block : blocks)
            for (const auto& factual : block.observations) {
                const auto variant = factual.slot.comparison_variant() ==
                                             measurement::ComparisonVariantSlot::first
                                         ? 0U : 1U;
                const bool associated = std::visit([&](const auto& observation) {
                    const auto workers = observation.workers();
                    if (workers.size() != admitted[variant].size() ||
                        workers.size() != factual.placements.size() ||
                        workers.size() != factual.migrations.size()) return false;
                    for (std::size_t index{}; index < workers.size(); ++index)
                        if (workers[index].worker_id() != admitted[variant][index].worker_id ||
                            factual.placements[index].worker_id() != admitted[variant][index].worker_id ||
                            factual.migrations[index].worker_id() != admitted[variant][index].worker_id)
                            return false;
                    return true;
                }, factual.observation);
                if (!associated) return reject(TerminalDefinitions::finalization);
            }
        for (std::size_t variant{}; variant < admitted.size(); ++variant)
            for (std::size_t worker_index{}; worker_index < admitted[variant].size(); ++worker_index) {
                const auto* available = admitted[variant][worker_index].region.available();
                if (!available) continue;
                const auto qualification = available->pre_run().qualification_state();
                for (const auto& block : blocks)
                    for (const auto& factual : block.observations) {
                        const auto factual_variant = factual.slot.comparison_variant() ==
                            measurement::ComparisonVariantSlot::first ? 0U : 1U;
                        if (factual_variant != variant) continue;
                        const auto state = factual.placements[worker_index].state();
                        if ((available->request().verify_before() &&
                             (!qualification || available->pre_run().state() !=
                                  memory::PreRunDiagnosticState::observed)) ||
                            (!available->request().verify_before() &&
                             (qualification || available->pre_run().state() !=
                                  memory::PreRunDiagnosticState::skipped)) ||
                            (qualification == memory::PlacementQualificationState::qualified &&
                             state != measurement::WorkerPlacementValidityState::qualified) ||
                            (qualification == memory::PlacementQualificationState::not_qualified &&
                             state != measurement::WorkerPlacementValidityState::not_qualified) ||
                            (!qualification && state !=
                                 measurement::WorkerPlacementValidityState::not_applicable))
                            return reject(TerminalDefinitions::finalization);
                    }
            }
    }
    return std::pair{std::move(storage_->stopping),
        wire_v1::detail::ReconstructedTerminalAndFinalization{
            std::move(*terminal_value), std::move(finalization_value)}};
}

struct PreparedFinalizedEvidenceReconstruction::Storage final {
    PreparedCanonicalCursor cursor;
    PreparedTypedBuilderPlan plan;
    PreparedEvidenceHeaderBuilder header;
    PreparedCalibrationWarmupBuilder calibration;
    PreparedBlockPool blocks;
    PreparedTerminalReconstruction terminal;
    detail::PreparedPairwiseLedgerStorage ledger;
    PreparedCanonicalWriter writer;
    bool consumed{};

    Storage(PreparedCanonicalCursor cursor_value, PreparedTypedBuilderPlan plan_value,
            PreparedEvidenceHeaderBuilder header_value,
            PreparedCalibrationWarmupBuilder calibration_value,
            PreparedBlockPool blocks_value, PreparedTerminalReconstruction terminal_value,
            detail::PreparedPairwiseLedgerStorage ledger_value,
            PreparedCanonicalWriter writer_value) noexcept
        : cursor{std::move(cursor_value)}, plan{std::move(plan_value)},
          header{std::move(header_value)}, calibration{std::move(calibration_value)},
          blocks{std::move(blocks_value)}, terminal{std::move(terminal_value)},
          ledger{std::move(ledger_value)}, writer{std::move(writer_value)} {}
};

PreparedFinalizedEvidenceReconstruction::~PreparedFinalizedEvidenceReconstruction() noexcept =
    default;
PreparedFinalizedEvidenceReconstruction::PreparedFinalizedEvidenceReconstruction(
    PreparedFinalizedEvidenceReconstruction&&) noexcept = default;

PreparedFinalizedEvidenceReconstruction::PreparedFinalizedEvidenceReconstruction(
    std::unique_ptr<Storage> storage) noexcept
    : storage_{std::move(storage)} {}

auto PreparedFinalizedEvidenceReconstruction::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedFinalizedEvidenceReconstruction,
                     prepared_resources_v1::PreparedResourceError> {
    return prepare_impl(resource, nullptr, nullptr);
}

auto PreparedFinalizedEvidenceReconstruction::prepare(
    const prepared_resources_v1::PreparedResourceValue& resource,
    const wire_v1::CalculatedEvidenceCapacity& capacity,
    const detail::PairwiseHostCapacityRequestTranscript& request) noexcept
    -> std::expected<PreparedFinalizedEvidenceReconstruction,
                     prepared_resources_v1::PreparedResourceError> {
    return prepare_impl(resource, &capacity, &request);
}

auto PreparedFinalizedEvidenceReconstruction::prepare_impl(
    const prepared_resources_v1::PreparedResourceValue& resource,
    const wire_v1::CalculatedEvidenceCapacity* capacity,
    const detail::PairwiseHostCapacityRequestTranscript* request) noexcept
    -> std::expected<PreparedFinalizedEvidenceReconstruction,
                     prepared_resources_v1::PreparedResourceError> {
    try {
        static constexpr auto root_blocks = required_definition("array__root__blocks");
        if (resource.definitions.size() != wire_v1::detail::schema_nodes.size())
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};
        const auto maximum_blocks = stock_cardinality(resource, root_blocks);
        if (!maximum_blocks)
            return std::unexpected{prepared_resources_v1::PreparedResourceError{
                prepared_resources_v1::PreparedResourceErrorCode::resource_mismatch}};

        auto decode_owner = prepared_resources_v1::PreparedResourceOwner::create(resource);
        auto encode_owner = prepared_resources_v1::PreparedResourceOwner::create(resource);
        if (!decode_owner || !encode_owner)
            return std::unexpected{!decode_owner ? decode_owner.error() : encode_owner.error()};
        auto cursor = capacity ? PreparedCanonicalCursor::prepare(std::move(*decode_owner), *capacity)
                               : PreparedCanonicalCursor::prepare(std::move(*decode_owner));
        auto plan = PreparedTypedBuilderPlan::prepare(resource);
        auto header = PreparedEvidenceHeaderBuilder::prepare(resource);
        auto calibration = PreparedCalibrationWarmupBuilder::prepare(resource);
        auto blocks = capacity && request ? PreparedBlockPool::prepare(resource, *capacity, *request)
                                          : PreparedBlockPool::prepare(resource);
        auto terminal = PreparedTerminalReconstruction::prepare(resource);
        auto ledger = detail::PreparedPairwiseLedgerStorage::prepare(*maximum_blocks);
        auto writer = PreparedCanonicalWriter::prepare(std::move(*encode_owner));
        if (!cursor || !plan || !header || !calibration || !blocks || !terminal || !ledger ||
            !writer) {
            if (!cursor) return std::unexpected{cursor.error()};
            if (!plan) return std::unexpected{plan.error()};
            if (!header) return std::unexpected{header.error()};
            if (!calibration) return std::unexpected{calibration.error()};
            if (!blocks) return std::unexpected{blocks.error()};
            if (!terminal) return std::unexpected{terminal.error()};
            if (!ledger)
                return std::unexpected{prepared_resources_v1::PreparedResourceError{
                    prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
            return std::unexpected{writer.error()};
        }
        auto storage = std::make_unique<Storage>(
            std::move(*cursor), std::move(*plan), std::move(*header),
            std::move(*calibration), std::move(*blocks), std::move(*terminal),
            std::move(*ledger), std::move(*writer));
        return PreparedFinalizedEvidenceReconstruction{std::move(storage)};
    } catch (...) {
        return std::unexpected{prepared_resources_v1::PreparedResourceError{
            prepared_resources_v1::PreparedResourceErrorCode::allocation_failure}};
    }
}

auto PreparedFinalizedEvidenceReconstruction::build(
    std::span<const char> body,
    std::span<const core::LinuxCpuId> authenticated_cpus) noexcept
    -> std::expected<FinalizedPairwiseRunEvidence, ActiveCodecFailure> {
    const auto reject = [](ActivePipelineFailureKind kind, std::uint64_t sequence = 0U,
                           std::uint64_t limit = 0U, std::uint64_t observed = 0U) {
        return std::expected<FinalizedPairwiseRunEvidence, ActiveCodecFailure>{std::unexpected{
            ActiveCodecFailure{kind, ActivePipelineStage::active, sequence, limit, observed,
                               std::nullopt}}};
    };
    if (!storage_ || storage_->consumed)
        return reject(ActivePipelineFailureKind::transport_state);
    storage_->consumed = true;
    if (auto active = storage_->plan.activate(); !active)
        return std::unexpected{active.error()};
    if (auto active = storage_->cursor.activate(); !active)
        return std::unexpected{active.error()};
    if (auto decoded = storage_->cursor.decode(body, storage_->plan); !decoded)
        return std::unexpected{decoded.error()};
    auto nodes = storage_->plan.finish();
    if (!nodes) return std::unexpected{nodes.error()};
    if (auto completed = storage_->cursor.complete(storage_->plan); !completed)
        return std::unexpected{completed.error()};

    auto header = storage_->header.build(body, *nodes);
    if (!header) return std::unexpected{header.error()};
    auto calibration = storage_->calibration.build(body, *nodes);
    if (!calibration) return std::unexpected{calibration.error()};
    const bool has_prelude = calibration->calibration.has_value() || calibration->warmup.has_value();
    auto ledger = detail::PairwiseLedgerPreparedAccess::create(
        std::move(header->provenance), std::move(header->statistics_policy),
        std::move(header->monitoring_policy), std::move(header->repetition_policy),
        std::move(header->comparison_order), std::move(storage_->ledger));
    if (!ledger)
        return reject(ActivePipelineFailureKind::ledger_validation, 1U);
    if (calibration->calibration) {
        auto associated = ledger->associate_calibration(std::move(*calibration->calibration));
        if (!associated) return reject(ActivePipelineFailureKind::ledger_validation, 2U);
    }
    if (calibration->warmup) {
        auto associated = ledger->associate_warmup(std::move(*calibration->warmup));
        if (!associated) return reject(ActivePipelineFailureKind::ledger_validation, 3U);
    }

    static constexpr auto root_blocks = required_definition("array__root__blocks");
    static constexpr auto block = required_definition("block");
    const auto* block_array = parsed_node(*nodes, root_blocks);
    if (!block_array)
        return reject(ActivePipelineFailureKind::ledger_validation, root_blocks);
    for (std::size_t ordinal{}; ordinal < block_array->element_count; ++ordinal) {
        const auto* block_node = parsed_node_inside(*nodes, block, *block_array, ordinal);
        if (!block_node)
            return reject(ActivePipelineFailureKind::ledger_validation, block, ordinal,
                          block_array->element_count);
        auto value = storage_->blocks.build(ordinal, body, *nodes, *block_node,
                                            authenticated_cpus);
        if (!value) return std::unexpected{value.error()};
        if (auto appended = ledger->append_block(std::move(*value)); !appended)
            return reject(ActivePipelineFailureKind::ledger_validation, block, ordinal,
                          block_array->element_count);
    }

    auto terminal = storage_->terminal.build(
        body, *nodes, detail::PairwiseLedgerPreparedAccess::statistics_policy(*ledger),
        detail::PairwiseLedgerPreparedAccess::repetition_policy(*ledger),
        detail::PairwiseLedgerPreparedAccess::experiment_id(*ledger),
        detail::PairwiseLedgerPreparedAccess::blocks(*ledger));
    if (!terminal) return std::unexpected{terminal.error()};
    if (has_prelude && !terminal->second.finalization)
        return reject(ActivePipelineFailureKind::ledger_validation, TerminalDefinitions::finalization_option);
    for (std::size_t ordinal{}; ordinal < terminal->first.size(); ++ordinal)
        if (!detail::PairwiseLedgerPreparedAccess::append_reconstructed_stopping_evaluation(
                *ledger, std::move(terminal->first[ordinal])))
            return reject(ActivePipelineFailureKind::ledger_validation, ordinal);
    if (terminal->second.finalization) {
        auto associated =
            ledger->associate_finalization(std::move(*terminal->second.finalization));
        if (!associated) return reject(ActivePipelineFailureKind::ledger_validation, 4U);
    }
    const bool recorded = std::visit(
        [&ledger](auto& value) noexcept {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Value, NormalRunTerminalEvidence>)
                return ledger->record_normal_terminal(std::move(value)).has_value();
            else
                return ledger->record_operational_terminal(std::move(value)).has_value();
        },
        terminal->second.terminal);
    if (!recorded)
        return reject(ActivePipelineFailureKind::ledger_validation, 5U);
    auto finalized = std::move(*ledger).finalize();
    if (!finalized)
        return reject(ActivePipelineFailureKind::ledger_validation, 6U);

    if (auto active = storage_->writer.activate(); !active)
        return std::unexpected{active.error()};
    if (auto written = write_finalized_evidence(storage_->writer, *finalized); !written)
        return std::unexpected{written.error()};
    auto encoded = storage_->writer.finish();
    if (!encoded) return std::unexpected{encoded.error()};
    if (encoded->size() != body.size() ||
        !std::equal(encoded->begin(), encoded->end(), body.begin()))
        return reject(ActivePipelineFailureKind::reencode_mismatch, 0U, body.size(),
                      encoded->size());
    return std::move(*finalized);
}

namespace {
using WriteResult = std::expected<void, ActiveCodecFailure>;
#include "runtime/finalized_evidence_prepared_typed_sink.generated.inc"
} // namespace

auto write_progress_policy_fields(PreparedCanonicalWriter& writer,
    const StatisticsProductionPolicy& statistics, const monitoring::MonitoringExecutionPolicy& monitoring,
    const measurement::RepetitionPolicy& repetition, const measurement::ComparisonOrderPolicy& comparison) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (auto r = writer.append_literal("{\"comparison_order\":"); !r) return r;
    if (auto r = write_comparison_order(writer, comparison); !r) return r;
    if (auto r = writer.append_literal(",\"monitoring_policy\":"); !r) return r;
    if (auto r = write_monitoring_policy(writer, monitoring); !r) return r;
    if (auto r = writer.append_literal(",\"repetition_policy\":"); !r) return r;
    if (auto r = write_repetition_policy(writer, repetition); !r) return r;
    if (auto r = writer.append_literal(",\"statistics_policy\":"); !r) return r;
    if (auto r = write_statistics_policy(writer, statistics); !r) return r;
    return writer.append_literal("}");
}
auto write_progress_terminal(PreparedCanonicalWriter& writer,
    const NormalRunTerminalEvidence& value) noexcept -> std::expected<void, ActiveCodecFailure> {
    return write_normal_terminal_branch(writer, value, "normal");
}
auto write_progress_terminal(PreparedCanonicalWriter& writer,
    const OperationalRunTerminalEvidence& value) noexcept -> std::expected<void, ActiveCodecFailure> {
    return write_operational_terminal_branch(writer, value, "operational");
}
auto write_progress_calibration(PreparedCanonicalWriter& writer,
    const PairwiseCalibrationEvidence& value) noexcept -> std::expected<void, ActiveCodecFailure> {
    return write_calibration_present(writer, value);
}
auto write_progress_warmup(PreparedCanonicalWriter& writer,
    const PairwiseWarmupEvidence& value) noexcept -> std::expected<void, ActiveCodecFailure> {
    return write_warmup_present(writer, value);
}
auto write_progress_block_plan(PreparedCanonicalWriter& writer,
    const measurement::MeasurementBlockPlan& value) noexcept -> std::expected<void, ActiveCodecFailure> {
    return write_block_plan(writer, value);
}
auto write_progress_failed_observation(PreparedCanonicalWriter& writer,
    const FailedObservationEvidence& value) noexcept -> std::expected<void, ActiveCodecFailure> {
    return write_failed_observation(writer, value);
}
auto write_progress_finalization(PreparedCanonicalWriter& writer,
    const PairwiseFinalizationEvidence& value) noexcept -> std::expected<void, ActiveCodecFailure> {
    return write_finalization(writer, value);
}

auto write_calibration_warmup(PreparedCanonicalWriter& writer,
                              const wire_v1::detail::ReconstructedCalibrationWarmup& value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (auto result = writer.append_literal("{\"calibration\":"); !result)
        return result;
    if (auto result = write_calibration(writer, value.calibration); !result)
        return result;
    if (auto result = writer.append_literal(",\"warmup\":"); !result)
        return result;
    if (auto result = write_warmup(writer, value.warmup); !result)
        return result;
    return writer.append_literal("}");
}

auto write_measurement_validity(PreparedCanonicalWriter& writer,
                                const ReconstructedMeasurementValidity& value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (auto result = writer.append_literal("{\"measurement\":"); !result)
        return result;
    if (auto result = write_measurement(writer, value.observation); !result)
        return result;
    if (auto result = writer.append_literal(",\"validity\":"); !result)
        return result;
    if (auto result = write_validity(writer, value.validity); !result)
        return result;
    return writer.append_literal("}");
}

auto write_monitoring(PreparedCanonicalWriter& writer,
                      const std::optional<monitoring::MonitoringCaptureResult>& value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    return value ? write_monitoring_result(writer, *value) : writer.append_literal("null");
}

auto write_finalized_error_value(PreparedCanonicalWriter& writer,
                                 const FinalizedEvidenceError& value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    return write_finalized_error(writer, value);
}

auto write_memory_diagnostic(PreparedCanonicalWriter& writer,
                             const memory::RegionFinalizationDiagnostic& value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    return write_region_diagnostic(writer, value);
}

auto write_factual_observation(PreparedCanonicalWriter& writer,
                               const FactualObservationEvidence& value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    return write_factual_observation(writer, value, {});
}

auto write_block_attempt(PreparedCanonicalWriter& writer,
                         const PairwiseBlockAttemptEvidence& value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    return write_block(writer, value, {});
}

auto write_stopping_evaluation(PreparedCanonicalWriter& writer,
                               const RunnerStoppingEvaluationEvidence& value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    return write_stopping(writer, value, {});
}

auto write_terminal_and_finalization(
    PreparedCanonicalWriter& writer,
    const wire_v1::detail::ReconstructedTerminalAndFinalization& value) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (auto result = writer.append_literal("{\"finalization\":"); !result) return result;
    if (value.finalization) {
        if (auto result = write_finalization(writer, *value.finalization); !result) return result;
    } else if (auto result = writer.append_literal("null"); !result) return result;
    if (auto result = writer.append_literal(",\"terminal\":"); !result) return result;
    if (auto result = write_terminal(writer, value.terminal); !result) return result;
    return writer.append_literal("}");
}

auto write_finalized_evidence(PreparedCanonicalWriter& writer,
                              const FinalizedPairwiseRunEvidence& evidence) noexcept
    -> std::expected<void, ActiveCodecFailure> {
    if (auto r = writer.append_literal("{\"blocks\":"); !r)
        return r;
    if (auto r = write_array__root__blocks(writer, evidence.blocks()); !r)
        return r;
    if (auto r = writer.append_literal(",\"calibration\":"); !r)
        return r;
    if (auto r = write_calibration(writer, evidence.calibration()); !r)
        return r;
    if (auto r = writer.append_literal(",\"comparison_order\":"); !r)
        return r;
    if (auto r = write_comparison_order(writer, evidence.comparison_order()); !r)
        return r;
    if (auto r = writer.append_literal(",\"finalization\":"); !r)
        return r;
    if (auto r = write_finalization(writer, evidence.finalization()); !r)
        return r;
    if (auto r = writer.append_literal(",\"monitoring_policy\":"); !r)
        return r;
    if (auto r = write_monitoring_policy(writer, evidence.monitoring_policy()); !r)
        return r;
    if (auto r = writer.append_literal(",\"provenance\":"); !r)
        return r;
    if (auto r = write_provenance(writer, evidence.provenance()); !r)
        return r;
    if (auto r = writer.append_literal(",\"repetition_policy\":"); !r)
        return r;
    if (auto r = write_repetition_policy(writer, evidence.repetition_policy()); !r)
        return r;
    if (auto r = writer.append_literal(",\"schema\":\"0\",\"statistics_policy\":"); !r)
        return r;
    if (auto r = write_statistics_policy(writer, evidence.statistics_policy()); !r)
        return r;
    if (auto r = writer.append_literal(",\"stopping_evaluations\":"); !r)
        return r;
    if (auto r = write_array__root__stopping_evaluations(writer, evidence.stopping_evaluations());
        !r)
        return r;
    if (auto r = writer.append_literal(",\"terminal\":"); !r)
        return r;
    if (auto r = write_terminal(writer, evidence.terminal()); !r)
        return r;
    if (auto r = writer.append_literal(",\"warmup\":"); !r)
        return r;
    if (auto r = write_warmup(writer, evidence.warmup()); !r)
        return r;
    return writer.append_literal("}");
}
} // namespace epycbench::runtime::prepared_codec_v1
