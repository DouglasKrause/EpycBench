#include "runtime/pairwise_v2_progress.hpp"
#include "epycbench/runtime/production_control_monitor.hpp"
#include "runtime/pairwise_preparation_graph.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace epycbench::runtime::detail::progress {
namespace {
constexpr std::array<std::string_view, kind_count> names{
    "preparation", "calibration", "warmup", "block_opened", "observation",
    "failed_observation", "block_closed", "stopping", "finalization", "terminal", "failure"};
constexpr std::array<std::string_view, 6> messages{"offer", "echo", "begin", "chunk", "end", "ack"};
constexpr auto hex_digit(char c) noexcept -> int {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
auto add(std::uint64_t& value, std::uint64_t amount) noexcept -> bool {
    if (amount > std::numeric_limits<std::uint64_t>::max() - value) return false;
    value += amount;
    return true;
}
auto multiply(std::uint64_t a, std::uint64_t b, std::uint64_t& value) noexcept -> bool {
    if (b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b) return false;
    value = a * b;
    return true;
}
// The closed grammar is shared by emission and admission. No DOM, escaping,
// locale, allocation or exception-capable conversion occurs on this path.
struct Cursor {
    std::span<const std::byte> input;
    std::span<std::byte> output;
    std::size_t position{};
    bool reading{}, good{true};
    void literal(std::string_view value) noexcept {
        if (!good) return;
        const auto available = reading ? input.size() : output.size();
        if (value.size() > available - position) { good = false; return; }
        for (const auto c : value) {
            const auto byte = static_cast<std::byte>(c);
            if (reading) { if (input[position] != byte) good = false; }
            else output[position] = byte;
            ++position;
        }
    }
    void text(std::string_view& value) noexcept {
        literal("\"");
        if (!good) return;
        if (reading) {
            const auto begin = position;
            while (position < input.size() && input[position] != std::byte{'"'}) {
                const auto c = std::to_integer<unsigned char>(input[position]);
                if (c < 0x20U || c >= 0x7fU || c == '\\') { good = false; return; }
                ++position;
            }
            value = {reinterpret_cast<const char*>(input.data() + begin), position - begin};
        } else literal(value);
        literal("\"");
    }
    void fixed(std::string_view value) noexcept {
        auto observed = value;
        text(observed);
        if (observed != value) good = false;
    }
    void number(std::uint64_t& value) noexcept {
        std::array<char, 20> buffer{};
        const auto rendered = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        std::string_view spelling{buffer.data(), static_cast<std::size_t>(rendered.ptr - buffer.data())};
        text(spelling);
        if (!good || !reading) return;
        if (spelling.empty() || (spelling.size() > 1 && spelling.front() == '0')) { good = false; return; }
        const auto result = std::from_chars(spelling.data(), spelling.data() + spelling.size(), value);
        if (result.ec != std::errc{} || result.ptr != spelling.data() + spelling.size()) good = false;
    }
    void hex(std::span<std::byte> value, std::size_t& count, bool variable = false) noexcept {
        std::array<char, chunk_bytes * 2> buffer{};
        constexpr std::string_view alphabet = "0123456789abcdef";
        if (count > value.size() || count > chunk_bytes) { good = false; return; }
        if (!reading) for (std::size_t i = 0; i < count; ++i) {
            const auto byte = std::to_integer<unsigned char>(value[i]);
            buffer[2*i] = alphabet[byte >> 4U];
            buffer[2*i+1] = alphabet[byte & 15U];
        }
        std::string_view spelling{buffer.data(), count * 2};
        text(spelling);
        if (!good || !reading) return;
        if (spelling.size() % 2 != 0 || spelling.size()/2 > value.size() ||
            (!variable && spelling.size()/2 != count)) { good = false; return; }
        count = spelling.size()/2;
        for (std::size_t i = 0; i < count; ++i) {
            const auto high = hex_digit(spelling[2*i]);
            const auto low = hex_digit(spelling[2*i+1]);
            if (high < 0 || low < 0) { good = false; return; }
            value[i] = static_cast<std::byte>((high << 4) | low);
        }
    }
    void digest(Digest& value) noexcept { std::size_t count = value.size(); hex(value, count); }
    template<class Enum, std::size_t N>
    void choice(Enum& value, const std::array<std::string_view, N>& options) noexcept {
        const auto index = static_cast<std::size_t>(value);
        if (index >= N) { good = false; return; }
        auto spelling = options[index];
        text(spelling);
        const auto found = std::find(options.begin(), options.end(), spelling);
        if (found == options.end()) good = false;
        else value = static_cast<Enum>(found - options.begin());
    }
};
void grammar(Cursor& c, Frame& frame, bool omit_digest = false) noexcept {
    auto& o = frame.offer;
    auto& p = frame.payload;
    c.literal("{\"first_request\":"); c.number(o.identity.first);
    if (!omit_digest) { c.literal(",\"offer_digest\":"); c.digest(o.identity.offer); }
    c.literal(",\"payload\":{");
    switch (frame.message) {
    case Message::offer: {
        c.literal("\"durability\":");
        c.choice(o.durability, std::array<std::string_view,2>{"process", "persistent_block"});
        c.literal(",\"limits\":{\"arena_bytes\":"); c.number(o.limits.arena_bytes);
        c.literal(",\"chunk_bytes\":\"256\",\"components\":[");
        for (std::size_t i = 0; i < kind_count; ++i) {
            if (i != 0) c.literal(",");
            c.literal("{\"bytes\":"); c.number(o.limits.components[i].bytes);
            c.literal(",\"count\":"); c.number(o.limits.components[i].count);
            c.literal(",\"kind\":"); c.fixed(names[i]); c.literal("}");
        }
        c.literal("],\"transitions\":"); c.number(o.limits.transitions);
        c.literal(",\"wire_bytes\":"); c.number(o.limits.wire_bytes);
        c.literal("},\"provenance\":{\"inputs\":\"retained_supplied_evidence\",\"platform\":");
        c.choice(o.platform, std::array<std::string_view,2>{"injected_operations", "native_operations"});
        c.literal(",\"utc\":"); c.choice(o.utc, std::array<std::string_view,2>{"supplied", "acquired"});
        c.literal("},\"resource_commitment\":"); c.digest(o.resource);
        break;
    }
    case Message::echo: c.literal("\"resource_commitment\":"); c.digest(o.resource); break;
    case Message::begin:
        c.literal("\"availability\":"); c.number(p.availability);
        c.literal(",\"bytes\":"); c.number(p.bytes);
        c.literal(",\"chunks\":"); c.number(p.chunks);
        c.literal(",\"epoch\":"); c.number(p.epoch);
        c.literal(",\"kind\":"); c.choice(p.kind, names);
        c.literal(",\"operation\":"); c.number(p.operation);
        c.literal(",\"ordinal\":"); c.number(p.ordinal);
        c.literal(",\"sha256\":"); c.digest(p.sha256); break;
    case Message::chunk:
        c.literal("\"data\":"); c.hex(p.data, p.data_size, true);
        c.literal(",\"index\":"); c.number(p.index);
        c.literal(",\"ordinal\":"); c.number(p.ordinal); break;
    case Message::end:
        c.literal("\"chunks\":"); c.number(p.chunks);
        c.literal(",\"ordinal\":"); c.number(p.ordinal);
        c.literal(",\"sha256\":"); c.digest(p.sha256); break;
    case Message::ack:
        c.literal("\"first_record\":"); c.number(p.first_record);
        c.literal(",\"journal_digest\":"); c.digest(p.journal_digest);
        c.literal(",\"last_record\":"); c.number(p.last_record);
        c.literal(",\"ordinal\":"); c.number(p.ordinal);
        c.literal(",\"receipt\":");
        c.choice(p.receipt, std::array<std::string_view,2>{"complete_write", "synchronized_boundary"});
        c.literal(",\"sha256\":"); c.digest(p.sha256); break;
    default: c.good = false; return;
    }
    c.literal("},\"profile\":"); c.fixed(profile);
    c.literal(",\"second_request\":"); c.number(o.identity.second);
    c.literal(",\"session\":");
    std::string_view session{o.identity.session.data(), o.identity.session.size()};
    c.text(session);
    if (session.size() != o.identity.session.size()) c.good = false;
    else if (c.reading) std::copy(session.begin(), session.end(), o.identity.session.begin());
    c.literal(",\"type\":"); c.fixed(messages[static_cast<std::size_t>(frame.message)]);
    c.literal("}");
}
auto shape(const Frame& frame) noexcept -> bool {
    const auto& p = frame.payload;
    if (!identity({frame.offer.identity.session.data(),64}, frame.offer.identity.first,
                  frame.offer.identity.second)) return false;
    switch (frame.message) {
    case Message::offer: {
        std::uint64_t count{}, bytes{};
        for (const auto& component : frame.offer.limits.components) {
            std::uint64_t stock{};
            if ((component.count != 0 && component.bytes == 0) ||
                !multiply(component.count, component.bytes, stock) || !add(count, component.count) ||
                !add(bytes, stock)) return false;
        }
        return count != 0 && count == frame.offer.limits.transitions && bytes == frame.offer.limits.arena_bytes &&
               frame.offer.limits.wire_bytes != 0;
    }
    case Message::echo: return true;
    case Message::begin: return p.ordinal && p.availability && p.epoch && p.bytes &&
        p.chunks == p.bytes/chunk_bytes + (p.bytes%chunk_bytes != 0 ? 1U : 0U) &&
        static_cast<std::size_t>(p.kind) < kind_count;
    case Message::chunk: return p.ordinal && p.data_size > 0 && p.data_size <= chunk_bytes;
    case Message::end: return p.ordinal && p.chunks;
    case Message::ack: return p.ordinal && p.first_record && p.last_record >= p.first_record;
    }
    return false;
}
auto body(Frame frame, bool omit_digest) noexcept -> std::expected<EncodedFrame, Error> {
    EncodedFrame output;
    Cursor cursor{{}, std::span{output.storage}.subspan(4), 0, false, true};
    grammar(cursor, frame, omit_digest);
    const auto cap = frame.message == Message::offer ? 4096U : 1024U;
    if (!cursor.good || cursor.position > cap) return std::unexpected{Error::capacity};
    output.size = cursor.position + 4;
    const auto length = static_cast<std::uint32_t>(cursor.position);
    for (unsigned i = 0; i < 4; ++i)
        output.storage[i] = static_cast<std::byte>((length >> ((3U-i)*8U)) & 255U);
    return output;
}
} // namespace

auto kind_name(Kind kind) noexcept -> std::string_view {
    const auto index = static_cast<std::size_t>(kind);
    return index < names.size() ? names[index] : std::string_view{};
}
auto validate_component_ceilings(const Limits& offered, const wire_v1::CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<void, Error> {
    using Child = wire_v1::EvidenceComponentKind;
    using Side = wire_v1::EvidenceComponentSide;
    constexpr std::array selected{
        std::pair{Kind::calibration, Child::calibration}, std::pair{Kind::warmup, Child::warmup},
        std::pair{Kind::block_opened, Child::block_plan}, std::pair{Kind::observation, Child::factual_observation},
        std::pair{Kind::failed_observation, Child::failed_observation},
        std::pair{Kind::stopping, Child::stopping_evaluation}, std::pair{Kind::finalization, Child::finalization},
        std::pair{Kind::terminal, Child::terminal}, std::pair{Kind::failure, Child::finalized_error}};
    for (const auto& [kind, child] : selected) {
        const auto& limit = offered.components[static_cast<std::size_t>(kind)];
        for (const auto side : {Side::first, Side::second}) {
            const auto shape = wire_v1::maximum_component_shape(child, side, capacity);
            if (!shape) return std::unexpected{shape.error().code == wire_v1::CapacityErrorCode::arithmetic_overflow
                ? Error::overflow : Error::limits};
            if (limit.bytes < shape->canonical_logical_bytes.value) return std::unexpected{Error::capacity};
        }
    }
    return {};
}
auto identity(std::string_view session, std::uint64_t first, std::uint64_t second) noexcept
    -> std::expected<Identity, Error> {
    if (session.size() != 64 || first == 0 || second == 0 || first == second ||
        !std::all_of(session.begin(), session.end(), [](char c) { return hex_digit(c) >= 0; }))
        return std::unexpected{Error::identity};
    Identity result;
    std::copy(session.begin(), session.end(), result.session.begin());
    result.first = first; result.second = second;
    return result;
}
auto limits(std::uint64_t blocks, const std::array<std::uint64_t,kind_count>& ceilings) noexcept
    -> std::expected<Limits, Error> {
    if (blocks == 0) return std::unexpected{Error::limits};
    std::uint64_t observations{};
    if (!multiply(blocks,4,observations)) return std::unexpected{Error::overflow};
    const std::array<std::uint64_t,kind_count> counts{1,1,1,blocks,observations,1,blocks,blocks,1,1,2};
    Limits result;
    for (std::size_t i = 0; i < kind_count; ++i) {
        if (ceilings[i] == 0) return std::unexpected{Error::limits};
        result.components[i] = {counts[i],ceilings[i]};
        std::uint64_t stock{}, framed{}, chunks = ceilings[i]/chunk_bytes + (ceilings[i]%chunk_bytes != 0 ? 1U : 0U);
        if (!multiply(counts[i],ceilings[i],stock) || !add(result.arena_bytes,stock) ||
            !add(result.transitions,counts[i]) || !add(chunks,3) ||
            !multiply(chunks,1028,framed) || !multiply(framed,counts[i],framed) ||
            !add(result.wire_bytes,framed)) return std::unexpected{Error::overflow};
    }
    // One offer and one echo, in addition to begin/chunks/end/ack per transition.
    if (!add(result.wire_bytes,4100+1028)) return std::unexpected{Error::overflow};
    return result;
}
auto bind_offer(Offer offer) noexcept -> std::expected<Offer, Error> {
    Frame frame{Message::offer,offer,{}};
    if (!shape(frame)) return std::unexpected{Error::limits};
    auto encoded = body(frame,true);
    if (!encoded) return std::unexpected{encoded.error()};
    offer.identity.offer = core::sha256(encoded->bytes().subspan(4)).bytes();
    return offer;
}
auto encode(const Frame& frame) noexcept -> std::expected<EncodedFrame, Error> {
    if (static_cast<std::size_t>(frame.message) >= messages.size() || !shape(frame))
        return std::unexpected{Error::schema};
    if (frame.message == Message::offer) {
        const auto bound = bind_offer(frame.offer);
        if (!bound || bound->identity.offer != frame.offer.identity.offer)
            return std::unexpected{Error::digest};
    }
    return body(frame,false);
}
auto decode(std::span<const std::byte> bytes) noexcept -> std::expected<Frame, Error> {
    if (bytes.size() < 4 || bytes.size() > 4100) return std::unexpected{Error::framing};
    std::uint32_t size{};
    for (std::size_t i = 0; i < 4; ++i) size = (size << 8U) | std::to_integer<unsigned char>(bytes[i]);
    if (size != bytes.size()-4) return std::unexpected{Error::framing};
    // Type is the final canonical member: six fixed alternatives, never an
    // unbounded scan or allocation-dependent JSON dispatch.
    for (std::size_t i = 0; i < messages.size(); ++i) {
        Frame frame; frame.message = static_cast<Message>(i);
        Cursor cursor{bytes.subspan(4),{},0,true,true};
        grammar(cursor,frame);
        if (!cursor.good || cursor.position != size) continue;
        if (size > (frame.message == Message::offer ? 4096U : 1024U) || !shape(frame))
            return std::unexpected{Error::schema};
        if (frame.message == Message::offer) {
            const auto bound = bind_offer(frame.offer);
            if (!bound || bound->identity.offer != frame.offer.identity.offer)
                return std::unexpected{Error::digest};
        }
        return frame;
    }
    return std::unexpected{Error::schema};
}
auto FrameReader::feed(std::span<const std::byte> input) noexcept -> std::expected<Feed, Error> {
    if (failed_) return std::unexpected{Error::state};
    Feed result;
    while (result.consumed < input.size() && frame_.size < target_) {
        const auto count = std::min(input.size()-result.consumed,target_-frame_.size);
        std::copy_n(input.data()+result.consumed,count,frame_.storage.data()+frame_.size);
        frame_.size += count; result.consumed += count;
        if (frame_.size == 4 && target_ == 4) {
            std::uint32_t length{};
            for (std::size_t i = 0; i < 4; ++i)
                length = (length << 8U) | std::to_integer<unsigned char>(frame_.storage[i]);
            if (length == 0 || length > 4096) { failed_ = true; return std::unexpected{Error::framing}; }
            target_ = static_cast<std::size_t>(length)+4;
        }
    }
    result.ready = frame_.size == target_;
    return result;
}
auto FrameReader::take() noexcept -> std::expected<Frame, Error> {
    if (failed_ || frame_.size != target_ || target_ == 4) return std::unexpected{Error::state};
    auto result = decode(frame_.bytes());
    if (!result) { failed_ = true; return result; }
    frame_.size = 0; target_ = 4;
    return result;
}
auto FrameReader::finish() const noexcept -> std::expected<void, Error> {
    if (failed_ || frame_.size != 0) return std::unexpected{Error::framing};
    return {};
}
namespace {
auto bound(const Offer& offer) noexcept -> bool {
    const auto checked = bind_offer(offer);
    return checked && checked->identity.offer == offer.identity.offer;
}
auto receipt_matches(const Offer& offer, Kind kind, Receipt receipt) noexcept -> bool {
    const auto wanted = offer.durability == Durability::persistent_block && kind == Kind::block_closed
        ? Receipt::synchronized_boundary : Receipt::complete_write;
    return receipt == wanted;
}
}
Sender::Sender(Offer offer) noexcept : offer_{offer} {
    if (!bound(offer_)) state_ = State::failed;
}
auto Sender::begin(const Transition& transition) noexcept -> std::expected<void, Error> {
    if (state_ != State::idle || cancelled_) return std::unexpected{Error::state};
    const auto kind = static_cast<std::size_t>(transition.kind);
    if (kind >= kind_count || transition.ordinal != ordinal_ || ordinal_ == UINT64_MAX ||
        transition.availability == 0 || transition.epoch == 0 || transition.canonical.empty())
        return std::unexpected{Error::sequence};
    const auto& limit = offer_.limits.components[kind];
    if (counts_[kind] >= limit.count || transition.canonical.size() > limit.bytes ||
        transition.canonical.size() > offer_.limits.arena_bytes-bytes_)
        return std::unexpected{Error::capacity};
    if (core::sha256(std::string_view{transition.canonical.data(),transition.canonical.size()}).bytes()
        != transition.digest) return std::unexpected{Error::digest};
    transition_ = transition;
    index_ = 0;
    pending_size_ = 0;
    state_ = State::begin;
    ++counts_[kind]; bytes_ += transition.canonical.size();
    return {};
}
auto Sender::next() noexcept -> std::expected<EncodedFrame, Error> {
    if ((cancelled_ && pending_size_ == 0) ||
        (state_ != State::begin && state_ != State::chunks && state_ != State::end))
        return std::unexpected{Error::state};
    Frame frame; frame.offer = offer_;
    auto& p = frame.payload;
    p.ordinal = ordinal_; p.sha256 = transition_.digest;
    p.chunks = transition_.canonical.size()/chunk_bytes +
        (transition_.canonical.size()%chunk_bytes != 0 ? 1U : 0U);
    if (state_ == State::begin) {
        frame.message = Message::begin;
        p.kind = transition_.kind; p.operation = transition_.operation;
        p.availability = transition_.availability; p.epoch = transition_.epoch;
        p.bytes = transition_.canonical.size();
    } else if (state_ == State::chunks) {
        frame.message = Message::chunk; p.index = index_;
        const auto offset = static_cast<std::size_t>(index_)*chunk_bytes;
        p.data_size = std::min(chunk_bytes,transition_.canonical.size()-offset);
        for (std::size_t i = 0; i < p.data_size; ++i)
            p.data[i] = static_cast<std::byte>(transition_.canonical[offset+i]);
    } else frame.message = Message::end;
    auto result = encode(frame);
    if (!result) { state_ = State::failed; return result; }
    if (result->size > offer_.limits.wire_bytes-wire_) {
        state_ = State::failed; return std::unexpected{Error::capacity};
    }
    pending_size_ = result->size;
    return result;
}
auto Sender::transferred() noexcept -> std::expected<void, Error> {
    if (pending_size_ == 0 || (state_ != State::begin && state_ != State::chunks && state_ != State::end))
        return std::unexpected{Error::state};
    wire_ += pending_size_; pending_size_ = 0;
    if (state_ == State::end) state_ = State::waiting;
    else if (cancelled_) state_ = State::stopped;
    else if (state_ == State::begin) state_ = State::chunks;
    else {
        ++index_;
        const auto chunks = transition_.canonical.size()/chunk_bytes +
            (transition_.canonical.size()%chunk_bytes != 0 ? 1U : 0U);
        if (index_ == chunks) state_ = State::end;
    }
    return {};
}
auto Sender::acknowledge(const Frame& frame) noexcept -> std::expected<void, Error> {
    if (state_ != State::waiting) { state_ = State::failed; return std::unexpected{Error::state}; }
    const auto& p = frame.payload;
    if (frame.message != Message::ack || frame.offer.identity != offer_.identity ||
        p.ordinal != ordinal_ || p.sha256 != transition_.digest || p.first_record <= last_record_ ||
        p.last_record < p.first_record || !receipt_matches(offer_,transition_.kind,p.receipt)) {
        state_ = State::failed; return std::unexpected{Error::receipt};
    }
    last_record_ = p.last_record; ++ordinal_;
    transition_ = {};
    state_ = cancelled_ ? State::stopped : State::idle;
    return {};
}
void Sender::cancel() noexcept {
    cancelled_ = true;
    if (pending_size_ == 0 && state_ != State::waiting && state_ != State::failed) state_ = State::stopped;
}
auto Sender::waiting() const noexcept -> bool { return state_ == State::waiting; }
auto Sender::idle() const noexcept -> bool { return state_ == State::idle && !cancelled_; }
auto Sender::partial() const noexcept -> bool { return pending_size_ != 0; }

Receiver::Receiver(Offer offer, std::vector<char> buffer) noexcept
    : offer_{offer}, buffer_{std::move(buffer)} {}
Receiver::Receiver(Receiver&& source) noexcept
    : offer_{source.offer_}, buffer_{std::move(source.buffer_)}, counts_{source.counts_},
      pending_{source.pending_}, ordinal_{source.ordinal_}, index_{source.index_}, bytes_{source.bytes_},
      wire_{source.wire_}, last_record_{source.last_record_},
      last_availability_{source.last_availability_}, last_operation_{source.last_operation_},
      last_epoch_{source.last_epoch_}, filled_{source.filled_}, state_{source.state_} {
    source.state_ = State::failed;
}
auto Receiver::prepare(Offer offer) noexcept -> std::expected<Receiver, Error> {
    if (!bound(offer)) return std::unexpected{Error::identity};
    std::uint64_t largest{};
    for (const auto& component : offer.limits.components)
        if (component.count != 0) largest = std::max(largest,component.bytes);
    if (largest > std::vector<char>{}.max_size()) return std::unexpected{Error::capacity};
    try { return Receiver{offer,std::vector<char>(static_cast<std::size_t>(largest))}; }
    catch (...) { return std::unexpected{Error::allocation}; }
}
auto Receiver::accept(const Frame& frame) noexcept -> std::expected<bool, Error> {
    const auto fail = [this](Error error) -> std::expected<bool, Error> {
        state_ = State::failed; return std::unexpected{error};
    };
    if (state_ != State::idle && state_ != State::chunks) return fail(Error::state);
    if (frame.offer.identity != offer_.identity) return fail(Error::identity);
    const auto encoded = encode(frame);
    if (!encoded) return fail(encoded.error());
    if (encoded->size > offer_.limits.wire_bytes-wire_) return fail(Error::capacity);
    const auto& p = frame.payload;
    if (p.ordinal != ordinal_ || ordinal_ == UINT64_MAX) return fail(Error::sequence);
    if (state_ == State::idle) {
        if (frame.message != Message::begin) return fail(Error::state);
        if (ordinal_ == 1) {
            if (p.kind != Kind::preparation || p.availability != 1 || p.operation != 0 || p.epoch == 0)
                return fail(Error::sequence);
        } else if (p.kind == Kind::preparation || last_availability_ == UINT64_MAX ||
            p.availability != last_availability_ + 1U || p.operation < last_operation_ ||
            (p.operation == 0 && p.kind != Kind::block_opened) || p.epoch < last_epoch_) return fail(Error::sequence);
        const auto kind = static_cast<std::size_t>(p.kind);
        const auto& limit = offer_.limits.components[kind];
        if (counts_[kind] >= limit.count || p.bytes > limit.bytes || p.bytes > buffer_.size() ||
            p.bytes > offer_.limits.arena_bytes-bytes_) return fail(Error::capacity);
        pending_ = p; filled_ = 0; index_ = 0; state_ = State::chunks;
    } else if (frame.message == Message::chunk) {
        if (index_ >= pending_.chunks || p.index != index_ ||
            p.data_size != std::min<std::uint64_t>(chunk_bytes,pending_.bytes-filled_))
            return fail(Error::sequence);
        for (std::size_t i = 0; i < p.data_size; ++i)
            buffer_[filled_+i] = static_cast<char>(std::to_integer<unsigned char>(p.data[i]));
        filled_ += p.data_size; ++index_;
    } else if (frame.message == Message::end) {
        if (index_ != pending_.chunks || filled_ != pending_.bytes || p.chunks != pending_.chunks ||
            p.sha256 != pending_.sha256) return fail(Error::sequence);
        if (core::sha256(std::string_view{buffer_.data(),filled_}).bytes() != pending_.sha256)
            return fail(Error::digest);
        state_ = State::ended;
        ++counts_[static_cast<std::size_t>(pending_.kind)]; bytes_ += filled_;
    } else return fail(Error::state);
    wire_ += encoded->size;
    return state_ == State::ended;
}
auto Receiver::transition() const noexcept -> std::expected<Transition, Error> {
    if (state_ != State::ended && state_ != State::acknowledging) return std::unexpected{Error::state};
    return Transition{pending_.kind,pending_.ordinal,pending_.operation,pending_.availability,
                      pending_.epoch,pending_.sha256,{buffer_.data(),filled_}};
}
auto Receiver::acknowledgement(const Acknowledgement& receipt) noexcept -> std::expected<EncodedFrame, Error> {
    if (state_ != State::ended || receipt.ordinal != ordinal_ || receipt.component != pending_.sha256 ||
        receipt.first_record <= last_record_ || receipt.last_record < receipt.first_record ||
        !receipt_matches(offer_,pending_.kind,receipt.receipt)) return std::unexpected{Error::receipt};
    Frame frame{Message::ack,offer_,{}};
    frame.payload.ordinal = receipt.ordinal; frame.payload.sha256 = receipt.component;
    frame.payload.first_record = receipt.first_record; frame.payload.last_record = receipt.last_record;
    frame.payload.journal_digest = receipt.journal; frame.payload.receipt = receipt.receipt;
    auto encoded = encode(frame);
    if (!encoded) return encoded;
    if (encoded->size > offer_.limits.wire_bytes-wire_) return std::unexpected{Error::capacity};
    wire_ += encoded->size; last_record_ = receipt.last_record;
    state_ = State::acknowledging;
    return encoded;
}
auto Receiver::acknowledged() noexcept -> std::expected<void, Error> {
    if (state_ != State::acknowledging) return std::unexpected{Error::state};
    last_availability_ = pending_.availability; last_operation_ = pending_.operation; last_epoch_ = pending_.epoch;
    ++ordinal_; state_ = State::idle; filled_ = 0; pending_ = {};
    return {};
}
void Receiver::stop() noexcept { state_ = State::stopped; }
auto Receiver::finish() const noexcept -> std::expected<void, Error> {
    if (state_ != State::idle && state_ != State::stopped) return std::unexpected{Error::state};
    return {};
}
} // namespace epycbench::runtime::detail::progress

namespace epycbench::runtime::detail {
PairwiseProgressState::Receipt::Receipt(const PairwiseProgressState* source,
    std::uint64_t epoch_value, std::uint64_t operation_value, std::uint64_t availability_value,
    bool retired_value) noexcept
    : owner{source}, epoch{epoch_value}, operation{operation_value},
      availability{availability_value}, retired{retired_value} {}
auto PairwiseProgressState::bind(const void* issuer) noexcept -> bool {
    if (issuer == nullptr || issuer_ != nullptr || retired_ || failed_) { failed_ = true; return false; }
    issuer_ = issuer;
    return true;
}
auto PairwiseProgressState::begin_operation(const void* issuer) noexcept -> bool {
    if (issuer != issuer_ || issuer_ == nullptr || retired_ || failed_ || leased_ || operation_ == UINT64_MAX) {
        failed_ = true; quiescent_ = false; return false;
    }
    ++operation_;
    invalidate(issuer);
    return !failed_;
}
void PairwiseProgressState::invalidate(const void* issuer) noexcept {
    quiescent_ = false;
    if (issuer != issuer_ || issuer_ == nullptr || retired_ || failed_ || leased_ || epoch_ == UINT64_MAX) {
        failed_ = true; return;
    }
    ++epoch_;
}
void PairwiseProgressState::quiesce(const void* issuer) noexcept {
    if (issuer == issuer_ && issuer_ != nullptr && !failed_ && !retired_ && !leased_) quiescent_ = true;
}
void PairwiseProgressState::seal(const void* issuer) noexcept {
    invalidate(issuer);
    if (failed_) return;
    retired_ = true; quiescent_ = true;
    issuer_ = nullptr; // No pointer to the soon-retired typed context survives.
}
auto PairwiseProgressState::take() noexcept -> std::expected<Receipt, progress::Error> {
    if (failed_ || !quiescent_ || leased_ || availability_ == UINT64_MAX)
        return std::unexpected{progress::Error::quiescence};
    ++availability_; leased_ = true;
    return Receipt{this,epoch_,operation_,availability_,retired_};
}
auto PairwiseProgressState::valid(const Receipt& receipt) const noexcept -> bool {
    return !failed_ && quiescent_ && leased_ && receipt.owner == this && receipt.epoch == epoch_ &&
        receipt.operation == operation_ && receipt.availability == availability_ && receipt.retired == retired_;
}
auto PairwiseProgressState::consume(const Receipt& receipt) noexcept -> bool {
    if (!valid(receipt)) return false;
    leased_ = false;
    return true;
}
} // namespace epycbench::runtime::detail

namespace epycbench::runtime::detail::progress {
auto Publication::publish(Kind kind, Fact fact, std::uint64_t original_availability,
    std::uint64_t original_operation) noexcept -> bool {
    auto receipt = state->take();
    if (!receipt) { interrupted = true; failure = receipt.error(); return false; }
    const auto result = callback.consume(callback.context,
        {kind,fact,original_availability,*receipt,original_operation});
    const bool consumed = state->consume(*receipt);
    if (!consumed || !result || !*result) {
        interrupted = true;
        if (!consumed) failure = Error::quiescence;
        else if (!result) failure = result.error();
        return false;
    }
    return true;
}
auto Publication::emit(Kind kind, Fact fact) noexcept -> bool {
    if (callback.consume == nullptr) return true;
    if (interrupted) return false;
    if (availability == UINT64_MAX || state == nullptr) {
        interrupted = true; failure = Error::state; return false;
    }
    ++availability;
    const auto operation = state->operation();
    if (pending_count) {
        if (kind != Kind::block_closed || pending_count == pending.size()) {
            interrupted = true; failure = Error::state; return false;
        }
        pending[pending_count++] = {kind,fact,availability,operation};
        return true;
    }
    auto receipt = state->take();
    if (!receipt) {
        if ((kind == Kind::failed_observation || kind == Kind::block_closed) &&
            pending_count < pending.size()) {
            pending[pending_count++] = {kind,fact,availability,operation};
            return true; // Retained source fact; no callback or continuation grant.
        }
        interrupted = true; failure = receipt.error(); return false;
    }
    const auto result = callback.consume(callback.context,{kind,fact,availability,*receipt,operation});
    const bool consumed = state->consume(*receipt);
    if (!consumed || !result || !*result) {
        interrupted = true;
        if (!consumed) failure = Error::quiescence;
        else if (!result) failure = result.error();
        return false;
    }
    return true;
}
auto Publication::drain() noexcept -> bool {
    if (interrupted) return false;
    if (!pending_count) return true;
    if (!state || !state->retired()) { interrupted = true; failure = Error::quiescence; return false; }
    while (pending_count) {
        const auto value = pending[0];
        if (!publish(value.kind,value.fact,value.availability,value.operation)) return false;
        for (std::size_t i=1;i<pending_count;++i) pending[i-1]=pending[i];
        --pending_count;
    }
    return true;
}
} // namespace epycbench::runtime::detail::progress

namespace epycbench::runtime::detail::progress {
namespace {
struct FactWriter {
    prepared_codec_v1::PreparedCanonicalWriter& writer;
    bool good{true};
    void literal(std::string_view text) noexcept { if (good) good = bool(writer.append_literal(text)); }
    void text(std::string_view text) noexcept { if (good) good = bool(writer.append_json_string(text)); }
    void number(std::uint64_t value) noexcept {
        literal("\""); if (good) good = bool(writer.append_unsigned(value)); literal("\"");
    }
    void boolean(bool value) noexcept { literal(value ? "true" : "false"); }
    void key(std::string_view name, bool& first) noexcept {
        if (!first) literal(",");
        first = false; text(name); literal(":");
    }
    void hex(std::span<const std::byte> bytes) noexcept {
        literal("\"");
        for (const auto byte : bytes) {
            constexpr char digits[] = "0123456789abcdef";
            const auto value = std::to_integer<unsigned>(byte);
            const std::array chars{digits[value >> 4U], digits[value & 15U]};
            literal({chars.data(), chars.size()});
        }
        literal("\"");
    }
    void slice(PreparationSlice value) noexcept {
        literal("{\"offset\":"); number(value.offset); literal(",\"size\":"); number(value.size); literal("}");
    }
    template<class Range, class Emit> void array(const Range& values, Emit emit) noexcept {
        literal("["); bool first = true;
        for (const auto& value : values) { if (!first) literal(","); first = false; emit(value); }
        literal("]");
    }
};
auto write_graph(prepared_codec_v1::PreparedCanonicalWriter& writer,
    const PairwisePreparationGraph& graph) noexcept -> bool {
    const auto facts = graph.facts();
    if (!graph.published() || !graph.stable() || !facts || !facts->policies || !facts->monitoring || !facts->measurement)
        return false;
    const auto& g = *facts;
    const auto& policies = *g.policies;
    FactWriter out{writer}; bool first = true;
    out.literal("{");
    out.key("cpus", first); out.array(g.cpus, [&](auto cpu) noexcept { out.number(cpu.value()); });
    out.key("experiment", first); out.number(g.experiment);
    out.key("first_request", first); out.number(g.first_request);
    out.key("frequency", first); out.array(g.frequency, [&](const auto& row) noexcept {
        out.literal("{\"cpus\":"); out.slice(row.cpus); out.literal(",\"driver\":"); out.slice(row.driver);
        out.literal(",\"governor\":"); out.slice(row.governor); out.literal(",\"kind\":");
        out.text(row.kind == monitoring::FrequencySourceKind::hardware_current ? "hardware_current" : "scaling_current");
        out.literal(",\"maximum\":"); out.number(row.maximum); out.literal(",\"minimum\":"); out.number(row.minimum); out.literal("}");
    });
    out.key("frequency_count", first); out.number(g.frequency_count);
    out.key("kernels", first); out.array(g.kernels, [&](const auto& row) noexcept {
        out.literal("{\"captured\":"); out.boolean(row.captured); out.literal(",\"index\":"); out.number(row.index);
        out.literal(",\"latency\":"); out.boolean(row.latency); out.literal(",\"second\":"); out.boolean(row.second);
        out.literal(",\"worker\":"); out.number(row.worker); out.literal("}");
    });
    out.key("memory", first); out.array(g.memory, [&](const auto& row) noexcept {
        if (!row.binding || !row.initialization || !row.protection) { out.good = false; return; }
        out.literal("{\"binding\":{\"migration_requested\":"); out.boolean(row.binding->migration_requested());
        out.literal(",\"policy\":"); out.number(static_cast<std::uint64_t>(row.binding->policy()));
        out.literal(",\"policy_call_accepted\":"); out.boolean(row.binding->policy_call_accepted());
        out.literal(",\"range\":"); out.number(static_cast<std::uint64_t>(row.binding->range()));
        out.literal(",\"strict_verification_requested\":"); out.boolean(row.binding->strict_verification_requested());
        out.literal("},\"bytes\":"); out.number(row.bytes); out.literal(",\"captured\":"); out.boolean(row.captured);
        out.literal(",\"denominator\":"); out.number(row.denominator); out.literal(",\"evaluation\":");
        if (row.evaluation) {
            const auto& e = *row.evaluation;
            out.literal("{\"denominator\":"); out.number(e.denominator); out.literal(",\"numerator\":"); out.number(e.numerator);
            out.literal(",\"requested\":"); out.number(e.requested); out.literal(",\"unexpected\":"); out.number(e.unexpected);
            out.literal(",\"unresolved\":"); out.number(e.unresolved); out.literal("}");
        } else out.literal("null");
        out.literal(",\"index\":"); out.number(row.index);
        out.literal(",\"initialization\":{\"initialization_completed\":"); out.boolean(row.initialization->initialization_completed());
        out.literal(",\"padding_touched\":"); out.boolean(row.initialization->padding_touched());
        out.literal(",\"prefault_pass_performed\":"); out.boolean(row.initialization->prefault_pass_performed());
        out.literal(",\"prefault_requested\":"); out.boolean(row.initialization->prefault_requested());
        out.literal(",\"range\":"); out.number(static_cast<std::uint64_t>(row.initialization->range())); out.literal("}");
        out.literal(",\"numerator\":"); out.number(row.numerator); out.literal(",\"observation\":");
        if (row.observation) {
            const auto& o = *row.observation;
            out.literal("{\"counts\":"); out.slice(o.counts); out.literal(",\"resident\":"); out.number(o.resident);
            out.literal(",\"total\":"); out.number(o.total); out.literal(",\"unresolved\":"); out.number(o.unresolved); out.literal("}");
        } else out.literal("null");
        out.literal(",\"page_mode\":"); out.number(static_cast<std::uint64_t>(row.page_mode));
        out.literal(",\"page_size\":"); out.number(row.page_size); out.literal(",\"page_storage\":"); out.slice(row.page_storage);
        out.literal(",\"placement_policy\":"); out.number(static_cast<std::uint64_t>(row.placement_policy));
        out.literal(",\"prefault\":"); out.boolean(row.prefault);
        out.literal(",\"protection\":{\"executable\":"); out.boolean(row.protection->executable());
        out.literal(",\"protection_accepted\":"); out.boolean(row.protection->protection_accepted());
        out.literal(",\"protection_kind\":"); out.number(static_cast<std::uint64_t>(row.protection->protection_kind()));
        out.literal(",\"range\":"); out.number(static_cast<std::uint64_t>(row.protection->range()));
        out.literal(",\"writable\":"); out.boolean(row.protection->writable()); out.literal("}");
        out.literal(",\"qualification\":"); out.number(static_cast<std::uint64_t>(row.qualification));
        out.literal(",\"query\":"); out.number(static_cast<std::uint64_t>(row.query));
        out.literal(",\"second\":"); out.boolean(row.second); out.literal(",\"targets\":"); out.slice(row.targets);
        out.literal(",\"verify_after\":"); out.boolean(row.verify_after); out.literal(",\"verify_before\":"); out.boolean(row.verify_before);
        out.literal(",\"worker\":"); out.number(row.worker); out.literal("}");
    });
    out.key("monitoring", first);
    const auto& m = *g.monitoring;
    out.literal("{\"capacity\":"); out.hex(m.capacity.bytes()); out.literal(",\"channels\":");
    out.array(m.channels, [&](const auto& channel) noexcept {
        out.literal("{\"effective\":"); out.number(static_cast<std::uint64_t>(channel.effective));
        out.literal(",\"support\":"); out.number(static_cast<std::uint64_t>(channel.support)); out.literal("}");
    });
    out.literal(",\"effective_digest\":"); out.hex(m.effective.digest().bytes());
    out.literal(",\"physical_digest\":"); out.hex(m.physical.digest().bytes());
    out.literal(",\"profile\":"); out.hex(m.profile.bytes());
    out.literal(",\"provenance\":"); out.hex(m.provenance.authority_digest().bytes());
    out.literal(",\"service\":");
    if (m.service) {
        out.literal("{\"assigned\":"); out.number(m.service->assigned.value());
        out.literal(",\"observed\":"); out.number(m.service->observed.value());
        out.literal(",\"verified_singleton\":"); out.number(m.service->verified_singleton.value()); out.literal("}");
    } else out.literal("null");
    out.literal("}");
    out.key("pages", first); out.array(g.pages, [&](const auto& row) noexcept {
        out.literal("{\"count\":"); out.number(row.count); out.literal(",\"node\":"); out.number(row.node); out.literal("}");
    });
    out.key("policies", first);
    out.literal("{\"calibration\":{\"maximum\":"); out.number(policies.calibration.maximum_duration().value());
    out.literal(",\"minimum\":"); out.number(policies.calibration.minimum_duration().value());
    out.literal(",\"target\":"); out.number(policies.calibration.target_duration().value()); out.literal("}");
    out.literal(",\"calibration_enabled\":"); out.boolean(policies.calibration_enabled);
    out.literal(",\"header\":");
    if (out.good) out.good = bool(prepared_codec_v1::write_progress_policy_fields(writer,
        policies.statistics, policies.monitoring, policies.repetition, policies.comparison));
    out.literal(",\"implementation\":"); out.number(static_cast<std::uint64_t>(policies.implementation));
    out.literal(",\"latency_seed\":"); out.number(policies.latency_seed);
    out.literal(",\"root_seed\":"); out.number(policies.root_seed);
    const auto overhead = policies.validity.maximum_harness_overhead();
    out.literal(",\"validity\":{\"denominator\":"); out.number(overhead.denominator());
    out.literal(",\"numerator\":"); out.number(overhead.numerator()); out.literal("}");
    out.literal(",\"warmup\":{\"maximum\":"); out.number(policies.warmup.maximum_trials());
    out.literal(",\"minimum\":"); out.number(policies.warmup.minimum_trials());
    const auto threshold = policies.warmup.stability_threshold();
    out.literal(",\"threshold\":{\"denominator\":"); out.number(threshold.denominator());
    out.literal(",\"numerator\":"); out.number(threshold.numerator());
    out.literal("},\"window\":"); out.number(policies.warmup.stability_window()); out.literal("}}");
    out.key("provenance", first); out.array(g.provenance, [&](auto value) noexcept { out.slice(value); });
    out.key("second_request", first); out.number(g.second_request);
    out.key("strings", first); out.hex(std::as_bytes(g.strings));
    out.key("targets", first); out.array(g.targets, [&](const auto& row) noexcept {
        out.literal("{\"linux_node\":"); out.number(row.linux_node); out.literal(",\"stable\":"); out.slice(row.stable);
        out.literal(",\"topology_node\":"); out.number(row.topology_node); out.literal("}");
    });
    out.key("workers", first); out.array(g.workers, [&](const auto& row) noexcept {
        out.literal("{\"affinity\":"); out.slice(row.affinity); out.literal(",\"captured\":"); out.boolean(row.captured);
        out.literal(",\"core\":"); out.slice(row.core); out.literal(",\"index\":"); out.number(row.index);
        out.literal(",\"observed_cpu\":"); out.number(row.observed_cpu); out.literal(",\"pu\":"); out.slice(row.pu);
        out.literal(",\"requested_cpu\":"); out.number(row.requested_cpu); out.literal(",\"tid\":"); out.number(row.tid);
        out.literal(",\"worker\":"); out.number(row.worker); out.literal("}");
    });
    out.literal("}");
    return out.good;
}
} // namespace
}

namespace epycbench::runtime::detail::progress {
auto encode_preparation(prepared_codec_v1::PreparedCanonicalWriter& writer,
    const AvailableFact& source, const ServiceThreadReadyEvidence& control) noexcept
    -> std::expected<std::span<const char>, Error> {
    if (source.kind != Kind::preparation || !source.receipt.owner ||
        !source.receipt.owner->valid(source.receipt)) return std::unexpected{Error::receipt};
    const auto* graph = std::get_if<const PairwisePreparationGraph*>(&source.fact);
    if (!graph || !*graph || control.role != ServiceRole::control_monitor ||
        control.observed_affinity.cardinality() != 1U) return std::unexpected{Error::schema};
    const auto begin = writer.size();
    FactWriter out{writer};
    out.literal("{\"control\":{\"assigned_cpu\":"); out.number(control.assigned_cpu.value());
    out.literal(",\"observed_affinity\":");
    out.array(control.observed_affinity, [&](auto cpu) noexcept { out.number(cpu.value()); });
    out.literal(",\"observed_cpu\":"); out.number(control.observed_cpu.value());
    out.literal(",\"role\":\"control_monitor\",\"stable_core\":");
    const auto core = control.stable_core.value(); out.hex(std::as_bytes(std::span{core.data(),core.size()}));
    out.literal(",\"stable_pu\":");
    const auto pu = control.stable_pu.value(); out.hex(std::as_bytes(std::span{pu.data(),pu.size()}));
    out.literal(",\"tid\":"); out.number(control.tid.value()); out.literal("},\"graph\":");
    if (!out.good || !write_graph(writer, **graph)) return std::unexpected{Error::encoding};
    out.literal("}"); if (!out.good) return std::unexpected{Error::encoding};
    const auto bytes = writer.progress_slice(begin);
    if (!bytes) return std::unexpected{Error::encoding};
    return *bytes;
}
auto preparation_ceiling(const PairwisePreparationGraph& graph,
    const PreparationGraphLimits& limits) noexcept -> std::expected<std::uint64_t, Error> {
    return preparation_ceiling(graph.usage(), graph.worker_count(), limits);
}
auto preparation_ceiling(const PreparationGraphUsage& usage, std::size_t workers,
    const PreparationGraphLimits& limits) noexcept -> std::expected<std::uint64_t, Error> {
    if (workers == 0 || workers > limits.workers || usage.strings > limits.strings ||
        usage.targets > limits.targets || usage.page_counts > limits.page_counts)
        return std::unexpected{Error::limits};
    // Each reserved member slot includes a <=40-byte key, <=66-byte scalar
    // (quoted SHA256 is the longest), separators and nested braces. Slices use
    // three slots, never an unbounded embedded string. Variable strings below
    // are hex encoded from the actual source bytes plus reserved driver labels.
    // Fixed policies/monitor/provenance/top-level structure uses <=128 slots;
    // row maxima are workers16, Memory64, Kernel8, target8, page4, frequency16.
    std::uint64_t bytes = 128U * 128U;
    const auto add = [&](std::uint64_t count, std::uint64_t width) noexcept {
        if (count > (UINT64_MAX - bytes) / width) return false;
        bytes += count * width;
        return true;
    };
    if (!add(workers,16U*128U) || !add(workers,2U*64U*128U) ||
        !add(workers,2U*8U*128U) || !add(usage.targets,8U*128U) ||
        !add(usage.page_counts,4U*128U) || !add(limits.domains,16U*128U) ||
        !add(workers,23U) || !add(limits.memberships,23U) ||
        !add(usage.strings,2U) || !add(limits.labels,2U))
        return std::unexpected{Error::overflow};
    return bytes;
}
auto encode_component(prepared_codec_v1::PreparedCanonicalWriter& writer,
    const AvailableFact& source) noexcept -> std::expected<std::span<const char>, Error> {
    namespace codec = prepared_codec_v1;
    if (!source.receipt.owner || !source.receipt.owner->valid(source.receipt))
        return std::unexpected{Error::receipt};
    const auto begin = writer.size();
    bool written = false;
    std::visit([&](const auto* value) noexcept {
        if (!value) return;
        using T = std::remove_cv_t<std::remove_pointer_t<decltype(value)>>;
        if constexpr (std::is_same_v<T, PairwisePreparationGraph>) {
            written = source.kind == Kind::preparation && write_graph(writer, *value);
        } else if constexpr (std::is_same_v<T, PairwiseCalibrationEvidence>) {
            written = source.kind == Kind::calibration && bool(codec::write_progress_calibration(writer, *value));
        } else if constexpr (std::is_same_v<T, PairwiseWarmupEvidence>) {
            written = source.kind == Kind::warmup && bool(codec::write_progress_warmup(writer, *value));
        } else if constexpr (std::is_same_v<T, measurement::MeasurementBlockPlan>) {
            written = source.kind == Kind::block_opened && bool(codec::write_progress_block_plan(writer, *value));
        } else if constexpr (std::is_same_v<T, FactualObservationEvidence>) {
            written = source.kind == Kind::observation && bool(codec::write_factual_observation(writer, *value));
        } else if constexpr (std::is_same_v<T, FailedObservationEvidence>) {
            written = source.kind == Kind::failed_observation && bool(codec::write_progress_failed_observation(writer, *value));
        } else if constexpr (std::is_same_v<T, PairwiseBlockAttemptEvidence>) {
            if (source.kind != Kind::block_closed) return;
            const auto number = [&](std::uint64_t n) noexcept {
                return writer.append_literal("\"") && writer.append_unsigned(n) && writer.append_literal("\"");
            };
            const char* completion = value->completion == BlockAttemptCompletion::complete_valid ? "complete_valid" :
                value->completion == BlockAttemptCompletion::complete_invalid ? "complete_invalid" :
                value->completion == BlockAttemptCompletion::operationally_incomplete ? "operationally_incomplete" : nullptr;
            if (!completion || !writer.append_literal("{\"block_id\":") || !number(value->plan.block_id().value()) ||
                !writer.append_literal(",\"completion\":") || !writer.append_json_string(completion) ||
                !writer.append_literal(",\"failed_observation\":")) return;
            if (value->failed_observation) {
                if (!number(value->failed_observation->observation_id.value())) return;
            } else if (!writer.append_literal("null")) return;
            if (!writer.append_literal(",\"observations\":[")) return;
            bool first = true;
            for (const auto& observation : value->observations) {
                if (!first && !writer.append_literal(",")) return;
                first = false;
                if (!number(observation.observation_id.value())) return;
            }
            written = bool(writer.append_literal("]}"));
        } else if constexpr (std::is_same_v<T, RunnerStoppingEvaluationEvidence>) {
            written = source.kind == Kind::stopping && bool(codec::write_stopping_evaluation(writer, *value));
        } else if constexpr (std::is_same_v<T, PairwiseFinalizationEvidence>) {
            written = source.kind == Kind::finalization && bool(codec::write_progress_finalization(writer, *value));
        } else if constexpr (std::is_same_v<T, NormalRunTerminalEvidence> ||
                             std::is_same_v<T, OperationalRunTerminalEvidence>) {
            written = source.kind == Kind::terminal && bool(codec::write_progress_terminal(writer, *value));
        }
    }, source.fact);
    if (!written) return std::unexpected{Error::encoding};
    const auto bytes = writer.progress_slice(begin);
    if (!bytes) return std::unexpected{Error::encoding};
    return *bytes;
}
} // namespace epycbench::runtime::detail::progress
