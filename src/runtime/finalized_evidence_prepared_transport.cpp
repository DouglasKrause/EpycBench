#include "runtime/finalized_evidence_transport.hpp"
#include "runtime/allocation_free_json_retirement.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>

namespace epycbench::runtime::detail {
namespace {
using Failure = FinalizedEvidenceTransportFailure;
constexpr std::size_t frame_capacity = static_cast<std::size_t>(ipc::maximum_frame_body_size) + 4U;
constexpr std::string_view manifest_a = R"({"payload":{"canonical_bytes":")";
constexpr std::string_view manifest_b = R"(","chunk_bytes":"786132","chunk_count":")";
constexpr std::string_view manifest_c =
    R"(","evidence_profile":"epycbench.finalized-pairwise-evidence/v1","first_request_id":")";
constexpr std::string_view manifest_d = R"(","second_request_id":")";
constexpr std::string_view manifest_e = R"(","sha256":")";
constexpr std::string_view manifest_f =
    R"("},"payload_profile":"epycbench.finalized-pairwise-evidence-manifest/v1","protocol":"epycbench.controller-runner-ipc/v2","request_id":")";
constexpr std::string_view manifest_g = R"(","sequence":"1","type":"pairwise_evidence_manifest"})";
constexpr std::string_view chunk_a = R"({"payload":{"chunk_count":")";
constexpr std::string_view chunk_b = R"(","chunk_index":")";
constexpr std::string_view chunk_c = R"(","data":")";
constexpr std::string_view chunk_d = R"(","first_request_id":")";
constexpr std::string_view chunk_e = R"(","second_request_id":")";
constexpr std::string_view chunk_f =
    R"("},"payload_profile":"epycbench.finalized-pairwise-evidence-chunk/v1","protocol":"epycbench.controller-runner-ipc/v2","request_id":")";
constexpr std::string_view chunk_g = R"(","sequence":")";
constexpr std::string_view chunk_h = R"(","type":"pairwise_evidence_chunk"})";
constexpr std::string_view completion_a = R"({"payload":{"canonical_bytes":")";
constexpr std::string_view completion_b = R"(","chunk_count":")";
constexpr std::string_view completion_c = R"(","first_request_id":")";
constexpr std::string_view completion_d = R"(","second_request_id":")";
constexpr std::string_view completion_e = R"(","sha256":")";
constexpr std::string_view completion_f =
    R"("},"payload_profile":"epycbench.finalized-pairwise-evidence-completed/v1","protocol":"epycbench.controller-runner-ipc/v2","request_id":")";
constexpr std::string_view completion_g = R"(","sequence":")";
constexpr std::string_view completion_h = R"(","type":"pairwise_completed"})";
constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

auto chunks(std::uint64_t bytes) noexcept -> std::uint64_t {
    return bytes / finalized_evidence_raw_chunk_bytes +
           (bytes % finalized_evidence_raw_chunk_bytes != 0U ? 1U : 0U);
}

auto prepare_frame() -> std::vector<std::byte> {
    std::vector<std::byte> result(frame_capacity);
    std::ranges::fill(result, std::byte{});
    return result;
}

auto classification(const core::Error& error) noexcept -> std::string_view {
    try {
        if (!error.context.is_object())
            return {};
        const auto found = error.context.find("classification");
        if (found == error.context.end() || !found->is_string())
            return {};
        return found->get_ref<const std::string&>();
    } catch (...) {
        return {};
    }
}

struct Builder final {
    std::span<std::byte> bytes;
    std::size_t cursor{4U};
    auto text(std::string_view value) noexcept -> bool {
        if (value.size() > bytes.size() - cursor)
            return false;
        std::transform(value.begin(), value.end(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                       [](char c) { return std::byte{static_cast<unsigned char>(c)}; });
        cursor += value.size();
        return true;
    }
    auto decimal(std::uint64_t value) noexcept -> bool {
        std::array<char, 20> scratch{};
        const auto [end, error] =
            std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
        return error == std::errc{} &&
               text({scratch.data(), static_cast<std::size_t>(end - scratch.data())});
    }
    auto digest(const core::Sha256Digest& value) noexcept -> bool {
        constexpr std::string_view hex = "0123456789abcdef";
        for (auto byte : value.bytes()) {
            const auto v = std::to_integer<unsigned>(byte);
            if (!text(hex.substr(v >> 4U, 1U)) || !text(hex.substr(v & 15U, 1U)))
                return false;
        }
        return true;
    }
    auto base64(std::span<const char> input) noexcept -> bool {
        for (std::size_t index{}; index < input.size(); index += 3U) {
            const auto a = static_cast<unsigned char>(input[index]);
            const auto b =
                index + 1U < input.size() ? static_cast<unsigned char>(input[index + 1U]) : 0U;
            const auto c =
                index + 2U < input.size() ? static_cast<unsigned char>(input[index + 2U]) : 0U;
            const std::array<char, 4> out{
                alphabet[a >> 2U], alphabet[((a & 3U) << 4U) | (b >> 4U)],
                index + 1U < input.size() ? alphabet[((b & 15U) << 2U) | (c >> 6U)] : '=',
                index + 2U < input.size() ? alphabet[c & 63U] : '='};
            if (!text(std::string_view{out.data(), out.size()}))
                return false;
        }
        return true;
    }
    auto finish() noexcept -> bool {
        const auto body = cursor - 4U;
        if (body == 0U || body > ipc::maximum_frame_body_size)
            return false;
        bytes[0] = std::byte{static_cast<unsigned char>(body >> 24U)};
        bytes[1] = std::byte{static_cast<unsigned char>(body >> 16U)};
        bytes[2] = std::byte{static_cast<unsigned char>(body >> 8U)};
        bytes[3] = std::byte{static_cast<unsigned char>(body)};
        return true;
    }
};

struct Parser final {
    std::string_view body;
    std::size_t cursor{};
    auto literal(std::string_view value) noexcept -> bool {
        if (!body.substr(cursor).starts_with(value))
            return false;
        cursor += value.size();
        return true;
    }
    auto quoted_decimal(std::uint64_t& value) noexcept -> bool {
        const auto end = body.find('"', cursor);
        if (end == std::string_view::npos || end == cursor ||
            (end - cursor > 1U && body[cursor] == '0'))
            return false;
        const auto [last, error] = std::from_chars(body.data() + cursor, body.data() + end, value);
        if (error != std::errc{} || last != body.data() + end)
            return false;
        cursor = end;
        return true;
    }
    auto digest(std::array<char, 64>& value) noexcept -> bool {
        if (body.size() - cursor < 64U)
            return false;
        for (std::size_t index{}; index < value.size(); ++index) {
            const auto character = body[cursor + index];
            if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
                return false;
            value[index] = character;
        }
        cursor += 64U;
        return true;
    }
};

auto digest_hex(const core::Sha256Digest& digest) noexcept -> std::array<char, 64> {
    constexpr std::string_view digits = "0123456789abcdef";
    std::array<char, 64> result{};
    std::size_t cursor{};
    for (const auto byte : digest.bytes()) {
        const auto value = std::to_integer<unsigned>(byte);
        result[cursor++] = digits[value >> 4U];
        result[cursor++] = digits[value & 15U];
    }
    return result;
}

auto sextet(char value) noexcept -> int {
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return 26 + value - 'a';
    if (value >= '0' && value <= '9')
        return 52 + value - '0';
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    return -1;
}

auto decode_into(std::string_view input, std::span<char> output) noexcept -> bool {
    if (input.empty() || input.size() % 4U != 0U)
        return false;
    const auto padding = input.ends_with("==") ? 2U : (input.ends_with('=') ? 1U : 0U);
    if (input.size() / 4U * 3U - padding != output.size())
        return false;
    std::size_t out{};
    for (std::size_t i{}; i < input.size(); i += 4U) {
        const bool last = i + 4U == input.size();
        const auto a = sextet(input[i]);
        const auto b = sextet(input[i + 1U]);
        const auto c = input[i + 2U] == '=' ? -1 : sextet(input[i + 2U]);
        const auto d = input[i + 3U] == '=' ? -1 : sextet(input[i + 3U]);
        if (a < 0 || b < 0 || (!last && (c < 0 || d < 0)) ||
            (input[i + 2U] == '=' && input[i + 3U] != '=') || (c < 0 && (b & 15) != 0) ||
            (d < 0 && c >= 0 && (c & 3) != 0))
            return false;
        output[out++] = static_cast<char>((a << 2) | (b >> 4));
        if (c >= 0)
            output[out++] = static_cast<char>((b << 4) | (c >> 2));
        if (d >= 0)
            output[out++] = static_cast<char>((c << 6) | d);
    }
    return out == output.size();
}
} // namespace

auto PreparedFinalizedEvidenceSender::prepare(FinalizedEvidenceTransportPlan plan,
                                              FinalizedEvidenceTransportIdentity identity) noexcept
    -> std::expected<PreparedFinalizedEvidenceSender, Failure> {
    try {
        const auto expected = FinalizedEvidenceTransportAccess::plan(plan.maximum_canonical_bytes);
        if (!expected || *expected != plan || identity.first == identity.second)
            return std::unexpected{Failure::invalid_capacity};
        return PreparedFinalizedEvidenceSender{plan, identity, prepare_frame()};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PreparedFinalizedEvidenceSender::activate(std::span<const char> canonical) noexcept
    -> std::expected<void, Failure> {
    if (active_ || terminal_)
        return std::unexpected{Failure::one_use};
    if (canonical.empty() || canonical.size() > plan_.maximum_canonical_bytes) {
        terminal_ = true;
        return std::unexpected{Failure::invalid_capacity};
    }
    canonical_ = canonical;
    chunk_count_ = chunks(canonical.size());
    digest_ = core::sha256(std::as_bytes(canonical));
    active_ = true;
    return build_next_frame();
}

auto PreparedFinalizedEvidenceSender::build_next_frame() noexcept -> std::expected<void, Failure> {
    Builder b{frame_};
    const auto first = identity_.first.value();
    const auto second = identity_.second.value();
    bool ok{};
    if (next_frame_ == 0U) {
        ok = b.text(manifest_a) && b.decimal(canonical_.size()) && b.text(manifest_b) &&
             b.decimal(chunk_count_) && b.text(manifest_c) && b.decimal(first) &&
             b.text(manifest_d) && b.decimal(second) && b.text(manifest_e) && b.digest(digest_) &&
             b.text(manifest_f) && b.decimal(first) && b.text(manifest_g);
    } else if (next_frame_ <= chunk_count_) {
        const auto index = next_frame_ - 1U;
        const auto offset = static_cast<std::size_t>(index * finalized_evidence_raw_chunk_bytes);
        const auto length =
            std::min<std::size_t>(finalized_evidence_raw_chunk_bytes, canonical_.size() - offset);
        ok = b.text(chunk_a) && b.decimal(chunk_count_) && b.text(chunk_b) && b.decimal(index) &&
             b.text(chunk_c) && b.base64(canonical_.subspan(offset, length)) && b.text(chunk_d) &&
             b.decimal(first) && b.text(chunk_e) && b.decimal(second) && b.text(chunk_f) &&
             b.decimal(first) && b.text(chunk_g) && b.decimal(2U + index) && b.text(chunk_h);
    } else {
        ok = b.text(completion_a) && b.decimal(canonical_.size()) && b.text(completion_b) &&
             b.decimal(chunk_count_) && b.text(completion_c) && b.decimal(first) &&
             b.text(completion_d) && b.decimal(second) && b.text(completion_e) &&
             b.digest(digest_) && b.text(completion_f) && b.decimal(first) &&
             b.text(completion_g) && b.decimal(2U + chunk_count_) && b.text(completion_h);
    }
    if (!ok || !b.finish()) {
        terminal_ = true;
        return std::unexpected{Failure::invalid_encoding};
    }
    frame_size_ = b.cursor;
    frame_progress_ = 0U;
    return {};
}

auto PreparedFinalizedEvidenceSender::write_step(ipc::ByteWriter& writer,
    PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, Failure> {
    return write_step(PreparedByteWriter{&writer,
        [](void* context, std::span<const std::byte> bytes) noexcept
            -> std::expected<std::size_t, PreparedIoFailure> {
            try {
                auto result = static_cast<ipc::ByteWriter*>(context)->write(bytes);
                if (result) return *result;
                const auto kind = classification(result.error());
                const auto fixed = kind == "would_block" ? PreparedIoFailure::would_block
                    : kind == "interrupted" ? PreparedIoFailure::interrupted : PreparedIoFailure::io;
                retire_json(result.error().context);
                return std::unexpected{fixed};
            } catch (const std::exception&) {
                return std::unexpected{PreparedIoFailure::contained_exception};
            } catch (...) {
                return std::unexpected{PreparedIoFailure::contained_unknown_failure};
            }
        }}, signal);
}

auto PreparedFinalizedEvidenceSender::write_step(PreparedByteWriter writer,
    PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, Failure> {
    if (!active_ || terminal_)
        return std::unexpected{Failure::one_use};
    if (signal != PreparedTransportSignal::none) {
        terminal_ = true;
        return std::unexpected{signal == PreparedTransportSignal::timeout ? Failure::timeout
                                                                          : Failure::cancelled};
    }
    if (!writer.write) {
        terminal_ = true;
        return std::unexpected{Failure::io};
    }
    const auto wrote = writer.write(writer.context, std::span<const std::byte>{frame_}.first(frame_size_).subspan(frame_progress_));
    if (!wrote) {
        const auto kind = wrote.error();
        if (kind == PreparedIoFailure::would_block ||
            (frame_progress_ == 0U && kind == PreparedIoFailure::interrupted))
            return PreparedTransportProgress::pending;
        terminal_ = true;
        if (kind == PreparedIoFailure::contained_exception)
            return std::unexpected{Failure::contained_exception};
        if (kind == PreparedIoFailure::contained_unknown_failure)
            return std::unexpected{Failure::contained_unknown_failure};
        return std::unexpected{Failure::io};
    }
    if (*wrote == 0U || *wrote > frame_size_ - frame_progress_) {
        terminal_ = true;
        return std::unexpected{Failure::invalid_progress};
    }
    frame_progress_ += *wrote;
    if (frame_progress_ != frame_size_)
        return PreparedTransportProgress::pending;
    ++next_frame_;
    if (next_frame_ == chunk_count_ + 2U) {
        terminal_ = true;
        committed_ = true;
        return PreparedTransportProgress::complete;
    }
    auto next = build_next_frame();
    if (!next)
        return std::unexpected{next.error()};
    return PreparedTransportProgress::pending;
}

auto PreparedFinalizedEvidenceReceiver::prepare(
    FinalizedEvidenceTransportPlan plan, FinalizedEvidenceTransportIdentity identity) noexcept
    -> std::expected<PreparedFinalizedEvidenceReceiver, Failure> {
    try {
        const auto expected = FinalizedEvidenceTransportAccess::plan(plan.maximum_canonical_bytes);
        if (!expected || *expected != plan || identity.first == identity.second ||
            plan.maximum_canonical_bytes > std::numeric_limits<std::size_t>::max())
            return std::unexpected{Failure::invalid_capacity};
        auto frame = prepare_frame();
        std::vector<char> logical(static_cast<std::size_t>(plan.maximum_canonical_bytes));
        std::ranges::fill(logical, '\0');
        return PreparedFinalizedEvidenceReceiver{plan, identity, std::move(frame),
                                                 std::move(logical)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PreparedFinalizedEvidenceReceiver::activate() noexcept -> std::expected<void, Failure> {
    if (active_ || terminal_)
        return std::unexpected{Failure::one_use};
    active_ = true;
    return {};
}

auto PreparedFinalizedEvidenceReceiver::read_step(ipc::ByteReader& reader,
    PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, Failure> {
    return read_step(PreparedByteReader{&reader,
        [](void* context, std::span<std::byte> bytes) noexcept
            -> std::expected<std::size_t, PreparedIoFailure> {
            try {
                auto result = static_cast<ipc::ByteReader*>(context)->read(bytes);
                if (result) return *result;
                const auto kind = classification(result.error());
                const auto fixed = kind == "would_block" ? PreparedIoFailure::would_block
                    : kind == "interrupted" ? PreparedIoFailure::interrupted : PreparedIoFailure::io;
                retire_json(result.error().context);
                return std::unexpected{fixed};
            } catch (const std::exception&) {
                return std::unexpected{PreparedIoFailure::contained_exception};
            } catch (...) {
                return std::unexpected{PreparedIoFailure::contained_unknown_failure};
            }
        }}, signal);
}

auto PreparedFinalizedEvidenceReceiver::read_step(PreparedByteReader reader,
    PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, Failure> {
    if (!active_ || terminal_)
        return std::unexpected{Failure::one_use};
    if (signal != PreparedTransportSignal::none) {
        terminal_ = true;
        return std::unexpected{signal == PreparedTransportSignal::timeout ? Failure::timeout
                                                                          : Failure::cancelled};
    }
    if (!reader.read) {
        terminal_ = true;
        return std::unexpected{Failure::io};
    }
    const auto got = reader.read(reader.context, std::span<std::byte>{frame_}.first(frame_target_).subspan(frame_progress_));
    if (!got) {
        const auto kind = got.error();
        if (kind == PreparedIoFailure::would_block ||
            (frame_progress_ == 0U && kind == PreparedIoFailure::interrupted))
            return PreparedTransportProgress::pending;
        terminal_ = true;
        if (kind == PreparedIoFailure::contained_exception)
            return std::unexpected{Failure::contained_exception};
        if (kind == PreparedIoFailure::contained_unknown_failure)
            return std::unexpected{Failure::contained_unknown_failure};
        return std::unexpected{Failure::io};
    }
    if (*got > frame_target_ - frame_progress_) {
        terminal_ = true;
        return std::unexpected{Failure::invalid_progress};
    }
    if (*got == 0U) {
        if (frame_progress_ != 0U) {
            terminal_ = true;
            return std::unexpected{Failure::truncated_frame};
        }
        if (!completion_) {
            terminal_ = true;
            return std::unexpected{Failure::stream_incomplete};
        }
        eof_ = true;
        terminal_ = true;
        return PreparedTransportProgress::complete;
    }
    frame_progress_ += *got;
    if (frame_progress_ != frame_target_)
        return PreparedTransportProgress::pending;
    if (frame_target_ == 4U) {
        const auto length = (std::to_integer<std::uint32_t>(frame_[0]) << 24U) |
                            (std::to_integer<std::uint32_t>(frame_[1]) << 16U) |
                            (std::to_integer<std::uint32_t>(frame_[2]) << 8U) |
                            std::to_integer<std::uint32_t>(frame_[3]);
        if (length == 0U || length > ipc::maximum_frame_body_size) {
            terminal_ = true;
            return std::unexpected{Failure::frame_over_limit};
        }
        frame_target_ = static_cast<std::size_t>(length) + 4U;
        return PreparedTransportProgress::pending;
    }
    auto consumed = consume_frame();
    if (!consumed) {
        terminal_ = true;
        return std::unexpected{consumed.error()};
    }
    frame_progress_ = 0U;
    frame_target_ = 4U;
    return PreparedTransportProgress::pending;
}

auto PreparedFinalizedEvidenceReceiver::consume_frame() noexcept -> std::expected<void, Failure> {
    const auto body_bytes = std::span<const std::byte>{frame_}.subspan(4U, frame_target_ - 4U);
    const std::string_view body{reinterpret_cast<const char*>(body_bytes.data()),
                                body_bytes.size()};
    Parser p{body};
    const auto first = identity_.first.value();
    const auto second = identity_.second.value();
    std::uint64_t a{}, b{}, c{}, d{}, e{};
    if (next_frame_ == 0U) {
        std::array<char, 64> digest{};
        if (!p.literal(manifest_a) || !p.quoted_decimal(a) || !p.literal(manifest_b) ||
            !p.quoted_decimal(b) || !p.literal(manifest_c) || !p.quoted_decimal(c) ||
            !p.literal(manifest_d) || !p.quoted_decimal(d) || !p.literal(manifest_e) ||
            !p.digest(digest) || !p.literal(manifest_f) || !p.quoted_decimal(e) ||
            !p.literal(manifest_g) || p.cursor != body.size() || a == 0U ||
            a > plan_.maximum_canonical_bytes || b != chunks(a) || c != first || d != second ||
            e != first)
            return std::unexpected{Failure::invalid_payload};
        canonical_bytes_ = a;
        chunk_count_ = b;
        expected_digest_hex_ = digest;
    } else if (next_frame_ <= chunk_count_) {
        const auto index = next_frame_ - 1U;
        if (!p.literal(chunk_a) || !p.quoted_decimal(a) || !p.literal(chunk_b) ||
            !p.quoted_decimal(b) || !p.literal(chunk_c))
            return std::unexpected{Failure::invalid_payload};
        const auto data_end = body.find('"', p.cursor);
        if (data_end == std::string_view::npos)
            return std::unexpected{Failure::invalid_payload};
        const auto encoded = body.substr(p.cursor, data_end - p.cursor);
        p.cursor = data_end;
        const auto expected =
            index + 1U < chunk_count_
                ? finalized_evidence_raw_chunk_bytes
                : canonical_bytes_ - finalized_evidence_raw_chunk_bytes * (chunk_count_ - 1U);
        if (expected > logical_.size() - logical_size_ ||
            !decode_into(encoded, std::span<char>{logical_}.subspan(
                                      logical_size_, static_cast<std::size_t>(expected))) ||
            !p.literal(chunk_d) || !p.quoted_decimal(c) || !p.literal(chunk_e) ||
            !p.quoted_decimal(d) || !p.literal(chunk_f) || !p.quoted_decimal(e) ||
            !p.literal(chunk_g))
            return std::unexpected{Failure::invalid_payload};
        std::uint64_t sequence{};
        if (!p.quoted_decimal(sequence) || !p.literal(chunk_h) || p.cursor != body.size() ||
            a != chunk_count_ || b != index || c != first || d != second || e != first ||
            sequence != 2U + index)
            return std::unexpected{Failure::invalid_payload};
        logical_size_ += static_cast<std::size_t>(expected);
    } else if (next_frame_ == chunk_count_ + 1U) {
        std::array<char, 64> digest{};
        if (!p.literal(completion_a) || !p.quoted_decimal(a) || !p.literal(completion_b) ||
            !p.quoted_decimal(b) || !p.literal(completion_c) || !p.quoted_decimal(c) ||
            !p.literal(completion_d) || !p.quoted_decimal(d) || !p.literal(completion_e) ||
            !p.digest(digest) || !p.literal(completion_f) || !p.quoted_decimal(e) ||
            !p.literal(completion_g))
            return std::unexpected{Failure::invalid_payload};
        std::uint64_t sequence{};
        if (!p.quoted_decimal(sequence) || !p.literal(completion_h) || p.cursor != body.size() ||
            a != canonical_bytes_ || b != chunk_count_ || c != first || d != second || e != first ||
            sequence != 2U + chunk_count_ || digest != expected_digest_hex_ ||
            logical_size_ != canonical_bytes_ ||
            digest_hex(core::sha256(std::as_bytes(std::span{logical_}.first(logical_size_)))) !=
                expected_digest_hex_)
            return std::unexpected{Failure::digest_mismatch};
        completion_ = true;
    } else {
        return std::unexpected{Failure::extra_event};
    }
    ++next_frame_;
    return {};
}

auto PreparedFinalizedEvidenceReceiver::finish() noexcept
    -> std::expected<std::span<const char>, Failure> {
    if (!eof_ || !completion_)
        return std::unexpected{Failure::stream_incomplete};
    return std::span<const char>{logical_}.first(logical_size_);
}
} // namespace epycbench::runtime::detail
