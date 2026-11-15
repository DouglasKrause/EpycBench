#include "runtime/pairwise_v2_resource_startup_protocol.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "runtime/finalized_evidence_prepared_resources.hpp"
#include "runtime/finalized_evidence_wire_startup_codec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <nlohmann/json.hpp>
#include <set>
#include <vector>

namespace epycbench::runtime::detail {
namespace {

using Failure = PairwiseV2ResourceStartupFailure;
using Json = nlohmann::ordered_json;

constexpr std::string_view capacity_offer_type = "finalized_evidence_capacity_offer";
constexpr std::string_view prepared_offer_type = "finalized_evidence_prepared_offer";
constexpr std::string_view capacity_ack_type = "finalized_evidence_capacity_accepted";
constexpr std::string_view prepared_ack_type = "finalized_evidence_prepared_accepted";

struct Frame final {
    std::string type;
    std::uint64_t request{};
    std::uint64_t sequence{};
    std::string profile;
    std::string payload;
};

auto interrupted(const core::Error& error) noexcept -> bool {
    try {
        return error.code.value() == "runtime.ipc.io_failure" &&
               error.context.is_object() &&
               error.context.value("classification", "") == "interrupted";
    } catch (...) {
        return false;
    }
}

auto parse_json(std::string_view bytes) -> std::expected<Json, Failure> {
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::key &&
                 !keys.back().insert(value.get<std::string>()).second)
            return false;
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    };
    auto parsed = Json::parse(bytes, callback, false);
    if (parsed.is_discarded())
        return std::unexpected{Failure::malformed};
    auto canonical = core::canonicalize_json(parsed);
    if (!canonical || *canonical != bytes)
        return std::unexpected{Failure::noncanonical};
    return parsed;
}

auto decimal(const Json& value) -> std::expected<std::uint64_t, Failure> {
    if (!value.is_string())
        return std::unexpected{Failure::malformed};
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || (text.size() > 1U && text.front() == '0'))
        return std::unexpected{Failure::malformed};
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::unexpected{Failure::malformed};
    return result;
}

auto body(const Frame& frame) -> std::expected<std::string, Failure> {
    auto payload = parse_json(frame.payload);
    if (!payload)
        return std::unexpected{payload.error()};
    Json envelope{{"payload", std::move(*payload)},
                  {"payload_profile", frame.profile},
                  {"protocol", std::string{pairwise_v2_resource_startup_protocol}},
                  {"request_id", std::to_string(frame.request)},
                  {"sequence", std::to_string(frame.sequence)},
                  {"type", frame.type}};
    auto canonical = core::canonicalize_json(envelope);
    if (!canonical || canonical->empty() ||
        canonical->size() > ipc::maximum_frame_body_size)
        return std::unexpected{Failure::framing};
    return *canonical;
}

auto write_body(ipc::ByteWriter& writer, std::string_view value)
    -> std::expected<void, Failure> {
    std::vector<std::byte> frame(value.size() + 4U);
    const auto size = static_cast<std::uint32_t>(value.size());
    frame[0] = std::byte{static_cast<unsigned char>(size >> 24U)};
    frame[1] = std::byte{static_cast<unsigned char>(size >> 16U)};
    frame[2] = std::byte{static_cast<unsigned char>(size >> 8U)};
    frame[3] = std::byte{static_cast<unsigned char>(size)};
    std::transform(value.begin(), value.end(), frame.begin() + 4,
                   [](char c) { return std::byte{static_cast<unsigned char>(c)}; });
    std::size_t progress{};
    while (progress != frame.size()) {
        auto written = writer.write(std::span<const std::byte>{frame}.subspan(progress));
        if (!written) {
            if (interrupted(written.error()))
                continue;
            return std::unexpected{Failure::io};
        }
        if (*written == 0U || *written > frame.size() - progress)
            return std::unexpected{Failure::io};
        progress += *written;
    }
    return {};
}

auto read_body(ipc::ByteReader& reader) -> std::expected<std::string, Failure> {
    std::array<std::byte, 4> header{};
    std::size_t progress{};
    while (progress != header.size()) {
        auto read = reader.read(std::span<std::byte>{header}.subspan(progress));
        if (!read) {
            if (interrupted(read.error()))
                continue;
            return std::unexpected{Failure::io};
        }
        if (*read == 0U || *read > header.size() - progress)
            return std::unexpected{Failure::io};
        progress += *read;
    }
    const auto size = (std::to_integer<std::uint32_t>(header[0]) << 24U) |
                      (std::to_integer<std::uint32_t>(header[1]) << 16U) |
                      (std::to_integer<std::uint32_t>(header[2]) << 8U) |
                      std::to_integer<std::uint32_t>(header[3]);
    if (size == 0U || size > ipc::maximum_frame_body_size)
        return std::unexpected{Failure::framing};
    std::string result(size, '\0');
    progress = 0U;
    while (progress != result.size()) {
        auto bytes = std::as_writable_bytes(std::span{result}).subspan(progress);
        auto read = reader.read(bytes);
        if (!read) {
            if (interrupted(read.error()))
                continue;
            return std::unexpected{Failure::io};
        }
        if (*read == 0U || *read > bytes.size())
            return std::unexpected{Failure::io};
        progress += *read;
    }
    return result;
}

auto parse_frame(std::string_view bytes) -> std::expected<Frame, Failure> {
    auto value = parse_json(bytes);
    if (!value || !value->is_object() || value->size() != 6U ||
        !value->contains("payload") || !value->contains("payload_profile") ||
        !value->contains("protocol") || !value->contains("request_id") ||
        !value->contains("sequence") || !value->contains("type") ||
        !value->at("payload").is_object() || !value->at("payload_profile").is_string() ||
        !value->at("type").is_string() ||
        value->at("protocol") != pairwise_v2_resource_startup_protocol)
        return std::unexpected{Failure::malformed};
    auto request = decimal(value->at("request_id"));
    auto sequence = decimal(value->at("sequence"));
    auto payload = core::canonicalize_json(value->at("payload"));
    if (!request || !sequence || !payload)
        return std::unexpected{Failure::malformed};
    return Frame{value->at("type").get<std::string>(), *request, *sequence,
                 value->at("payload_profile").get<std::string>(), std::move(*payload)};
}

auto write(ipc::ByteWriter& writer, std::uint64_t request, std::uint64_t sequence,
           std::string_view type, std::string_view profile, std::string_view payload)
    -> std::expected<void, Failure> {
    auto encoded = body(Frame{std::string{type}, request, sequence, std::string{profile},
                              std::string{payload}});
    if (!encoded)
        return std::unexpected{encoded.error()};
    return write_body(writer, *encoded);
}

auto read(ipc::ByteReader& reader, std::uint64_t request, std::uint64_t sequence,
          std::string_view type, std::string_view profile,
          stage_b_policy::Selection policy = stage_b_policy::Selection::existing_caller)
    -> std::expected<std::string, Failure> {
    auto encoded = read_body(reader);
    if (!encoded)
        return std::unexpected{encoded.error()};
    if (policy == stage_b_policy::Selection::stage_b_v1 &&
        !stage_b_policy::admit_bootstrap(*encoded))
        return std::unexpected{Failure::malformed};
    auto frame = parse_frame(*encoded);
    if (!frame)
        return std::unexpected{frame.error()};
    if (frame->request != request)
        return std::unexpected{Failure::identity_mismatch};
    if (frame->sequence != sequence || frame->type != type || frame->profile != profile)
        return std::unexpected{Failure::unexpected_message};
    return std::move(frame->payload);
}

} // namespace

auto PairwiseV2ResourceStartupController::write_offers(
    ipc::ByteWriter& writer, std::string_view capacity, std::string_view prepared) noexcept
    -> std::expected<void, Failure> {
    if (phase_ != Phase::start)
        return std::unexpected{Failure::invalid_state};
    phase_ = Phase::failed;
    try {
        if (auto result = write(writer, request_.value(), 0U, capacity_offer_type,
                                wire_v1::detail::capacity_offer_profile, capacity); !result)
            return result;
        if (auto result = write(writer, request_.value(), 1U, prepared_offer_type,
                                prepared_resources_v1::prepared_offer_profile, prepared); !result)
            return result;
        phase_ = Phase::offered;
        return {};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseV2ResourceStartupController::read_capacity_ack(ipc::ByteReader& reader) noexcept
    -> std::expected<std::string, Failure> {
    if (phase_ != Phase::offered)
        return std::unexpected{Failure::invalid_state};
    phase_ = Phase::failed;
    try {
        auto capacity = read(reader, request_.value(), 0U, capacity_ack_type,
                             wire_v1::detail::capacity_accepted_profile);
        if (!capacity)
            return std::unexpected{capacity.error()};
        phase_ = Phase::capacity_read;
        return capacity;
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseV2ResourceStartupController::read_prepared_ack(ipc::ByteReader& reader) noexcept
    -> std::expected<std::string, Failure> {
    if (phase_ != Phase::capacity_read)
        return std::unexpected{Failure::invalid_state};
    phase_ = Phase::failed;
    try {
        auto prepared = read(reader, request_.value(), 1U, prepared_ack_type,
                             prepared_resources_v1::prepared_ack_profile);
        if (!prepared)
            return std::unexpected{prepared.error()};
        phase_ = Phase::complete;
        return prepared;
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseV2ResourceStartupController::read_acks(ipc::ByteReader& reader) noexcept
    -> std::expected<PairwiseV2ResourceStartupAcks, Failure> {
    auto capacity = read_capacity_ack(reader);
    if (!capacity)
        return std::unexpected{capacity.error()};
    auto prepared = read_prepared_ack(reader);
    if (!prepared)
        return std::unexpected{prepared.error()};
    return PairwiseV2ResourceStartupAcks{std::move(*capacity), std::move(*prepared)};
}

auto PairwiseV2ResourceStartupRunner::read_offers(
    ipc::ByteReader& reader, stage_b_policy::Selection policy) noexcept
    -> std::expected<PairwiseV2ResourceStartupOffers, Failure> {
    if (phase_ != Phase::start)
        return std::unexpected{Failure::invalid_state};
    phase_ = Phase::failed;
    try {
        auto capacity = read(reader, request_.value(), 0U, capacity_offer_type,
                             wire_v1::detail::capacity_offer_profile, policy);
        if (!capacity)
            return std::unexpected{capacity.error()};
        auto prepared = read(reader, request_.value(), 1U, prepared_offer_type,
                             prepared_resources_v1::prepared_offer_profile, policy);
        if (!prepared)
            return std::unexpected{prepared.error()};
        phase_ = Phase::offered;
        return PairwiseV2ResourceStartupOffers{std::move(*capacity), std::move(*prepared)};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseV2ResourceStartupRunner::write_acks(
    ipc::ByteWriter& writer, std::string_view capacity, std::string_view prepared) noexcept
    -> std::expected<void, Failure> {
    if (phase_ != Phase::offered)
        return std::unexpected{Failure::invalid_state};
    phase_ = Phase::failed;
    try {
        if (auto result = write(writer, request_.value(), 0U, capacity_ack_type,
                                wire_v1::detail::capacity_accepted_profile, capacity); !result)
            return result;
        if (auto result = write(writer, request_.value(), 1U, prepared_ack_type,
                                prepared_resources_v1::prepared_ack_profile, prepared); !result)
            return result;
        phase_ = Phase::complete;
        return {};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

} // namespace epycbench::runtime::detail
