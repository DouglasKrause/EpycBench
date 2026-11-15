#include "runtime/finalized_evidence_transport.hpp"

#include "epycbench/core/canonical_json.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <set>

namespace epycbench::runtime::detail {
namespace {
using Failure = FinalizedEvidenceTransportFailure;
using Json = nlohmann::json;

constexpr std::string_view protocol = "epycbench.controller-runner-ipc/v2";
constexpr std::string_view manifest_type = "pairwise_evidence_manifest";
constexpr std::string_view manifest_profile = "epycbench.finalized-pairwise-evidence-manifest/v1";
constexpr std::string_view chunk_type = "pairwise_evidence_chunk";
constexpr std::string_view chunk_profile = "epycbench.finalized-pairwise-evidence-chunk/v1";
constexpr std::string_view completed_type = "pairwise_completed";
constexpr std::string_view completed_profile = "epycbench.finalized-pairwise-evidence-completed/v1";

template <class Callable, class Result = std::invoke_result_t<Callable>>
auto contained(Callable&& callable) noexcept -> std::expected<Result, Failure> {
    try {
        return std::forward<Callable>(callable)();
    } catch (Failure failure) {
        return std::unexpected{failure};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto exact(const Json& value, std::initializer_list<std::string_view> keys) -> bool {
    return value.is_object() && value.size() == keys.size() &&
           std::ranges::all_of(keys, [&](auto key) { return value.contains(key); });
}

auto decimal(const Json& value) -> std::optional<std::uint64_t> {
    if (!value.is_string())
        return std::nullopt;
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || (text.size() > 1U && text.front() == '0'))
        return std::nullopt;
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return result;
}

auto base64(std::span<const std::byte> input) -> std::string {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(4U * ((input.size() + 2U) / 3U));
    for (std::size_t index{}; index < input.size(); index += 3U) {
        const auto a = std::to_integer<unsigned>(input[index]);
        const auto b =
            index + 1U < input.size() ? std::to_integer<unsigned>(input[index + 1U]) : 0U;
        const auto c =
            index + 2U < input.size() ? std::to_integer<unsigned>(input[index + 2U]) : 0U;
        output.push_back(alphabet[a >> 2U]);
        output.push_back(alphabet[((a & 3U) << 4U) | (b >> 4U)]);
        output.push_back(index + 1U < input.size() ? alphabet[((b & 15U) << 2U) | (c >> 6U)] : '=');
        output.push_back(index + 2U < input.size() ? alphabet[c & 63U] : '=');
    }
    return output;
}

auto sextet(char value) -> std::optional<unsigned> {
    if (value >= 'A' && value <= 'Z')
        return static_cast<unsigned>(value - 'A');
    if (value >= 'a' && value <= 'z')
        return 26U + static_cast<unsigned>(value - 'a');
    if (value >= '0' && value <= '9')
        return 52U + static_cast<unsigned>(value - '0');
    if (value == '+')
        return 62U;
    if (value == '/')
        return 63U;
    return std::nullopt;
}

auto decode_base64(std::string_view input) -> std::optional<std::vector<std::byte>> {
    if (input.empty() || input.size() % 4U != 0U)
        return std::nullopt;
    const std::size_t padding = input.ends_with("==") ? 2U : (input.ends_with('=') ? 1U : 0U);
    std::vector<std::byte> output;
    output.reserve(input.size() / 4U * 3U - padding);
    for (std::size_t index{}; index < input.size(); index += 4U) {
        const bool last = index + 4U == input.size();
        auto a = sextet(input[index]);
        auto b = sextet(input[index + 1U]);
        auto c = input[index + 2U] == '=' ? std::optional<unsigned>{} : sextet(input[index + 2U]);
        auto d = input[index + 3U] == '=' ? std::optional<unsigned>{} : sextet(input[index + 3U]);
        if (!a || !b || (!c && input[index + 2U] != '=') || (!d && input[index + 3U] != '=') ||
            (!last && (!c || !d)) || (input[index + 2U] == '=' && input[index + 3U] != '=') ||
            (input[index + 2U] == '=' && (*b & 15U) != 0U) ||
            (input[index + 3U] == '=' && c && (*c & 3U) != 0U))
            return std::nullopt;
        output.push_back(std::byte{static_cast<unsigned char>((*a << 2U) | (*b >> 4U))});
        if (c)
            output.push_back(std::byte{static_cast<unsigned char>((*b << 4U) | (*c >> 2U))});
        if (d)
            output.push_back(std::byte{static_cast<unsigned char>((*c << 6U) | *d)});
    }
    if (base64(output) != input)
        return std::nullopt;
    return output;
}

struct Envelope {
    std::string type;
    std::uint64_t sequence{};
    std::string profile;
    Json payload;
};

auto parse(std::string_view body, ipc::RequestId first) -> std::expected<Envelope, Failure> {
    if (body.empty() || body.size() > ipc::maximum_frame_body_size)
        return std::unexpected{body.empty() ? Failure::invalid_encoding
                                            : Failure::frame_over_limit};
    bool duplicate{};
    std::vector<std::set<std::string>> stack;
    auto callback = [&](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::object_start)
            stack.emplace_back();
        else if (event == Json::parse_event_t::key &&
                 !stack.back().insert(value.get<std::string>()).second)
            duplicate = true;
        else if (event == Json::parse_event_t::object_end)
            stack.pop_back();
        return true;
    };
    auto value = Json::parse(body, callback, false);
    if (value.is_discarded() || duplicate)
        return std::unexpected{Failure::invalid_encoding};
    auto canonical = core::canonicalize_json(value);
    if (!canonical || *canonical != body)
        return std::unexpected{Failure::invalid_encoding};
    if (!exact(value,
               {"payload", "payload_profile", "protocol", "request_id", "sequence", "type"}) ||
        !value["protocol"].is_string() || !value["request_id"].is_string() ||
        !value["sequence"].is_string() || !value["type"].is_string() ||
        !value["payload_profile"].is_string() || !value["payload"].is_object())
        return std::unexpected{Failure::invalid_envelope};
    if (value["protocol"] != protocol)
        return std::unexpected{Failure::unsupported_protocol};
    auto request = ipc::RequestId::parse(value["request_id"].get_ref<const std::string&>());
    auto sequence = decimal(value["sequence"]);
    if (!request || !sequence)
        return std::unexpected{Failure::invalid_envelope};
    if (*request != first)
        return std::unexpected{Failure::identity_mismatch};
    return Envelope{value["type"].get<std::string>(), *sequence,
                    value["payload_profile"].get<std::string>(), std::move(value["payload"])};
}

auto encode(std::string_view type, const FinalizedEvidenceTransportIdentity& identity,
            std::uint64_t sequence, std::string_view profile, Json payload)
    -> std::expected<std::string, Failure> {
    Json envelope{{"payload", std::move(payload)},
                  {"payload_profile", profile},
                  {"protocol", protocol},
                  {"request_id", identity.first.text()},
                  {"sequence", std::to_string(sequence)},
                  {"type", type}};
    auto body = core::canonicalize_json(envelope);
    if (!body)
        return std::unexpected{Failure::invalid_encoding};
    if (body->size() > ipc::maximum_frame_body_size)
        return std::unexpected{Failure::frame_over_limit};
    return *body;
}

auto identity(const Json& payload) -> std::optional<FinalizedEvidenceTransportIdentity> {
    auto first =
        payload.contains("first_request_id") ? decimal(payload["first_request_id"]) : std::nullopt;
    auto second = payload.contains("second_request_id") ? decimal(payload["second_request_id"])
                                                        : std::nullopt;
    if (!first || !second || *first == *second || *first == 0U || *second == 0U)
        return std::nullopt;
    auto first_id = ipc::RequestId::create(*first);
    auto second_id = ipc::RequestId::create(*second);
    if (!first_id || !second_id)
        return std::nullopt;
    return FinalizedEvidenceTransportIdentity{*first_id, *second_id};
}

auto valid_identity(const FinalizedEvidenceTransportIdentity& value) noexcept -> bool {
    return value.first != value.second && value.first.value() != 0U && value.second.value() != 0U;
}

auto required_chunks(std::uint64_t bytes) noexcept -> std::uint64_t {
    return bytes / finalized_evidence_raw_chunk_bytes +
           (bytes % finalized_evidence_raw_chunk_bytes != 0U ? 1U : 0U);
}
} // namespace

auto FinalizedEvidenceTransportAccess::plan(std::uint64_t maximum) noexcept
    -> std::expected<FinalizedEvidenceTransportPlan, Failure> {
    if (maximum == 0U)
        return std::unexpected{Failure::invalid_capacity};
    const auto count = required_chunks(maximum);
    if (count == 0U || count > std::numeric_limits<std::uint64_t>::max() - 2U)
        return std::unexpected{Failure::arithmetic_overflow};
    return FinalizedEvidenceTransportPlan{maximum, count};
}

auto FinalizedEvidenceTransportAccess::create_reader(
    FinalizedEvidenceTransportPlan plan, FinalizedEvidenceTransportIdentity identity_value) noexcept
    -> std::expected<FinalizedEvidenceTransportReader, Failure> {
    const auto expected = FinalizedEvidenceTransportAccess::plan(plan.maximum_canonical_bytes);
    if (!valid_identity(identity_value) || !expected || *expected != plan ||
        plan.maximum_canonical_bytes > std::numeric_limits<std::size_t>::max())
        return std::unexpected{Failure::invalid_capacity};
    return contained([&] {
        std::vector<std::byte> storage;
        if (plan.maximum_canonical_bytes > storage.max_size())
            throw Failure::invalid_capacity;
        storage.reserve(static_cast<std::size_t>(plan.maximum_canonical_bytes));
        return FinalizedEvidenceTransportReader{plan, identity_value, std::move(storage)};
    });
}

auto FinalizedEvidenceTransportAccess::encode_manifest(
    const FinalizedEvidenceManifest& value) noexcept -> std::expected<std::string, Failure> {
    if (!valid_identity(value.identity) || value.canonical_bytes == 0U || value.chunk_count == 0U ||
        value.chunk_count != required_chunks(value.canonical_bytes) ||
        value.chunk_count > std::numeric_limits<std::uint64_t>::max() - 2U)
        return std::unexpected{Failure::invalid_payload};
    return contained([&] {
        auto result = encode(manifest_type, value.identity, 1U, manifest_profile,
                             {{"canonical_bytes", std::to_string(value.canonical_bytes)},
                              {"chunk_bytes", std::to_string(finalized_evidence_raw_chunk_bytes)},
                              {"chunk_count", std::to_string(value.chunk_count)},
                              {"evidence_profile", finalized_evidence_profile},
                              {"first_request_id", value.identity.first.text()},
                              {"second_request_id", value.identity.second.text()},
                              {"sha256", value.sha256.to_hex()}});
        if (!result)
            throw result.error();
        return std::move(*result);
    });
}

auto FinalizedEvidenceTransportAccess::encode_chunk(const FinalizedEvidenceChunk& value) noexcept
    -> std::expected<std::string, Failure> {
    if (value.chunk_count > std::numeric_limits<std::uint64_t>::max() - 2U)
        return std::unexpected{Failure::arithmetic_overflow};
    if (!valid_identity(value.identity) || value.chunk_count == 0U ||
        value.chunk_index >= value.chunk_count || value.bytes.empty() ||
        value.bytes.size() > finalized_evidence_raw_chunk_bytes ||
        (value.chunk_index + 1U < value.chunk_count &&
         value.bytes.size() != finalized_evidence_raw_chunk_bytes))
        return std::unexpected{Failure::invalid_payload};
    return contained([&] {
        auto result = encode(chunk_type, value.identity, 2U + value.chunk_index, chunk_profile,
                             {{"chunk_count", std::to_string(value.chunk_count)},
                              {"chunk_index", std::to_string(value.chunk_index)},
                              {"data", base64(value.bytes)},
                              {"first_request_id", value.identity.first.text()},
                              {"second_request_id", value.identity.second.text()}});
        if (!result)
            throw result.error();
        return std::move(*result);
    });
}

auto FinalizedEvidenceTransportAccess::encode_completion(
    const FinalizedEvidenceCompletion& value) noexcept -> std::expected<std::string, Failure> {
    if (!valid_identity(value.identity) || value.canonical_bytes == 0U || value.chunk_count == 0U ||
        value.chunk_count != required_chunks(value.canonical_bytes))
        return std::unexpected{Failure::invalid_payload};
    if (value.chunk_count > std::numeric_limits<std::uint64_t>::max() - 2U)
        return std::unexpected{Failure::arithmetic_overflow};
    return contained([&] {
        auto result =
            encode(completed_type, value.identity, 2U + value.chunk_count, completed_profile,
                   {{"canonical_bytes", std::to_string(value.canonical_bytes)},
                    {"chunk_count", std::to_string(value.chunk_count)},
                    {"first_request_id", value.identity.first.text()},
                    {"second_request_id", value.identity.second.text()},
                    {"sha256", value.sha256.to_hex()}});
        if (!result)
            throw result.error();
        return std::move(*result);
    });
}

auto FinalizedEvidenceTransportReader::read(std::string_view body) noexcept
    -> std::expected<void, Failure> {
    auto result = contained([&]() -> std::expected<void, Failure> {
        if (phase_ == Phase::complete)
            return std::unexpected{Failure::extra_event};
        if (phase_ == Phase::invalid)
            return std::unexpected{Failure::stream_incomplete};
        auto envelope = parse(body, identity_.first);
        if (!envelope) {
            return std::unexpected{envelope.error()};
        }
        if (phase_ == Phase::manifest) {
            if (envelope->type != manifest_type)
                return std::unexpected{Failure::unexpected_message};
            if (envelope->sequence != 1U)
                return std::unexpected{Failure::sequence_mismatch};
            if (envelope->profile != manifest_profile)
                return std::unexpected{Failure::unsupported_profile};
            auto& p = envelope->payload;
            if (!exact(p, {"canonical_bytes", "chunk_bytes", "chunk_count", "evidence_profile",
                           "first_request_id", "second_request_id", "sha256"}))
                return std::unexpected{Failure::invalid_payload};
            auto ids = identity(p);
            auto bytes = decimal(p["canonical_bytes"]);
            auto chunks = decimal(p["chunk_count"]);
            auto chunk_bytes = decimal(p["chunk_bytes"]);
            if (!p["sha256"].is_string())
                return std::unexpected{Failure::invalid_payload};
            auto sha = core::Sha256Digest::parse(p["sha256"].get_ref<const std::string&>());
            if (!ids || *ids != identity_ || !bytes || !*bytes ||
                *bytes > plan_.maximum_canonical_bytes || !chunks || !*chunks || !chunk_bytes ||
                *chunk_bytes != finalized_evidence_raw_chunk_bytes ||
                !p["evidence_profile"].is_string() ||
                p["evidence_profile"] != finalized_evidence_profile || !sha ||
                *chunks != (*bytes / finalized_evidence_raw_chunk_bytes +
                            (*bytes % finalized_evidence_raw_chunk_bytes != 0U)))
                return std::unexpected{Failure::invalid_payload};
            canonical_bytes_ = *bytes;
            chunk_count_ = *chunks;
            expected_sha256_ = *sha;
            phase_ = Phase::chunks;
            return {};
        }
        if (phase_ == Phase::chunks) {
            if (envelope->type != chunk_type)
                return std::unexpected{Failure::unexpected_message};
            if (envelope->sequence != 2U + next_chunk_)
                return std::unexpected{Failure::sequence_mismatch};
            if (envelope->profile != chunk_profile)
                return std::unexpected{Failure::unsupported_profile};
            auto& p = envelope->payload;
            if (!exact(p, {"chunk_count", "chunk_index", "data", "first_request_id",
                           "second_request_id"}) ||
                !p["data"].is_string())
                return std::unexpected{Failure::invalid_payload};
            auto ids = identity(p);
            auto count = decimal(p["chunk_count"]);
            auto index = decimal(p["chunk_index"]);
            auto decoded = decode_base64(p["data"].get_ref<const std::string&>());
            const auto expected =
                next_chunk_ + 1U < chunk_count_
                    ? finalized_evidence_raw_chunk_bytes
                    : canonical_bytes_ - finalized_evidence_raw_chunk_bytes * (chunk_count_ - 1U);
            if (!ids || *ids != identity_ || !count || *count != chunk_count_ || !index ||
                *index != next_chunk_ || !decoded || decoded->size() != expected)
                return std::unexpected{Failure::invalid_payload};
            storage_.insert(storage_.end(), decoded->begin(), decoded->end());
            ++next_chunk_;
            if (next_chunk_ == chunk_count_)
                phase_ = Phase::completion;
            return {};
        }
        if (phase_ == Phase::completion) {
            if (envelope->type != completed_type)
                return std::unexpected{Failure::unexpected_message};
            if (envelope->sequence != 2U + chunk_count_)
                return std::unexpected{Failure::sequence_mismatch};
            if (envelope->profile != completed_profile)
                return std::unexpected{Failure::unsupported_profile};
            auto& p = envelope->payload;
            if (!exact(p, {"canonical_bytes", "chunk_count", "first_request_id",
                           "second_request_id", "sha256"}))
                return std::unexpected{Failure::invalid_payload};
            auto ids = identity(p);
            auto bytes = decimal(p["canonical_bytes"]);
            auto count = decimal(p["chunk_count"]);
            if (!p["sha256"].is_string())
                return std::unexpected{Failure::invalid_payload};
            auto sha = core::Sha256Digest::parse(p["sha256"].get_ref<const std::string&>());
            if (!ids || *ids != identity_ || !bytes || *bytes != canonical_bytes_ || !count ||
                *count != chunk_count_ || !sha || *sha != expected_sha256_)
                return std::unexpected{Failure::invalid_payload};
            if (core::sha256(std::span<const std::byte>{storage_}) != expected_sha256_)
                return std::unexpected{Failure::digest_mismatch};
            phase_ = Phase::complete;
            return {};
        }
        return std::unexpected{Failure::stream_incomplete};
    });
    if (!result) {
        phase_ = Phase::invalid;
        return std::unexpected{result.error()};
    }
    if (!*result) {
        phase_ = Phase::invalid;
        return std::unexpected{result->error()};
    }
    return {};
}

auto FinalizedEvidenceTransportReader::finish() noexcept
    -> std::expected<std::span<const std::byte>, Failure> {
    if (phase_ != Phase::complete)
        return std::unexpected{Failure::stream_incomplete};
    return std::span<const std::byte>{storage_};
}
} // namespace epycbench::runtime::detail
