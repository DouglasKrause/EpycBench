#pragma once

#include "epycbench/core/error.hpp"
#include <nlohmann/json.hpp>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <utility>

namespace epycbench::runtime::ipc {
inline constexpr std::string_view protocol_profile="epycbench.controller-runner-ipc/v1";
inline constexpr std::string_view envelope_profile="epycbench.controller-runner-envelope/v1";
inline constexpr std::uint32_t maximum_frame_body_size=1'048'576U;
enum class MessageType { placement_request,cancel_request,request_accepted,placement_succeeded,request_cancelled,request_failed };
[[nodiscard]] auto message_type_token(MessageType) noexcept->std::string_view;
[[nodiscard]] auto message_payload_profile(MessageType) noexcept->std::string_view;
[[nodiscard]] auto parse_message_type(std::string_view)->std::expected<MessageType,core::Error>;

class RequestId { public:
 [[nodiscard]] static auto create(std::uint64_t)->std::expected<RequestId,core::Error>;
 [[nodiscard]] static auto parse(std::string_view)->std::expected<RequestId,core::Error>;
 [[nodiscard]] auto value()const noexcept->std::uint64_t{return value_;}
 [[nodiscard]] auto text()const->std::string; auto operator<=>(const RequestId&)const=default;
 private: explicit RequestId(std::uint64_t v):value_{v}{} std::uint64_t value_;
};
class SequenceNumber { public:
 [[nodiscard]] static auto create(std::uint64_t v)->SequenceNumber{return SequenceNumber{v};}
 [[nodiscard]] static auto parse(std::string_view)->std::expected<SequenceNumber,core::Error>;
 [[nodiscard]] auto value()const noexcept->std::uint64_t{return value_;}
 [[nodiscard]] auto text()const->std::string; auto operator<=>(const SequenceNumber&)const=default;
 private: explicit SequenceNumber(std::uint64_t v):value_{v}{} std::uint64_t value_;
};
class Envelope { public:
 [[nodiscard]] static auto create(MessageType,RequestId,SequenceNumber,nlohmann::ordered_json)->std::expected<Envelope,core::Error>;
 [[nodiscard]] static auto create(MessageType,RequestId,SequenceNumber,nlohmann::ordered_json,std::string_view)->std::expected<Envelope,core::Error>;
 [[nodiscard]] auto type()const noexcept->MessageType{return type_;}
 [[nodiscard]] auto request_id()const noexcept->const RequestId&{return request_;}
 [[nodiscard]] auto sequence()const noexcept->const SequenceNumber&{return sequence_;}
 [[nodiscard]] auto payload_profile()const noexcept->std::string_view{return profile_;}
 [[nodiscard]] auto payload()const noexcept->const nlohmann::ordered_json&{return payload_;}
 private: Envelope(MessageType t,RequestId r,SequenceNumber s,nlohmann::ordered_json p,std::string profile):type_{t},request_{r},sequence_{s},payload_(std::move(p)),profile_(std::move(profile)){}
 MessageType type_; RequestId request_; SequenceNumber sequence_; nlohmann::ordered_json payload_; std::string profile_;
};
[[nodiscard]] auto encode_body(const Envelope&)->std::expected<std::string,core::Error>;
[[nodiscard]] auto decode_body(std::string_view)->std::expected<Envelope,core::Error>;

class ByteReader{public:virtual~ByteReader()=default;virtual auto read(std::span<std::byte>)->std::expected<std::size_t,core::Error> =0;};
class ByteWriter{public:virtual~ByteWriter()=default;virtual auto write(std::span<const std::byte>)->std::expected<std::size_t,core::Error> =0;};
struct EndOfStream{auto operator<=>(const EndOfStream&)const=default;}; using FrameReadResult=std::variant<Envelope,EndOfStream>;
[[nodiscard]] auto encode_frame(const Envelope&)->std::expected<std::vector<std::byte>,core::Error>;
[[nodiscard]] auto read_frame(ByteReader&)->std::expected<FrameReadResult,core::Error>;
[[nodiscard]] auto write_frame(ByteWriter&,const Envelope&)->std::expected<void,core::Error>;

class RunnerControlState{public:[[nodiscard]]auto accept(const Envelope&)->std::expected<void,core::Error>;[[nodiscard]]auto end(bool terminal_complete)->std::expected<void,core::Error>;
private:enum class Phase{start,requested,cancelled};Phase phase_{Phase::start};std::uint64_t request_{};};
class ControllerEventState{public:explicit ControllerEventState(RequestId r):request_{r}{}[[nodiscard]]auto accept(const Envelope&)->std::expected<void,core::Error>;[[nodiscard]]auto end()->std::expected<void,core::Error>;
private:enum class Phase{start,accepted,terminal};Phase phase_{Phase::start};RequestId request_;};

struct RemoteFailure{core::ErrorCategory category;core::StableErrorCode code;std::string operation;std::optional<std::string> classification;std::optional<int> os_error;core::ErrorSeverity severity;core::ContinuationPolicy continuation;nlohmann::ordered_json context;auto operator==(const RemoteFailure&)const->bool=default;};
[[nodiscard]]auto project_failure(const core::Error&)->std::expected<nlohmann::ordered_json,core::Error>;
[[nodiscard]]auto parse_remote_failure(const nlohmann::json&)->std::expected<RemoteFailure,core::Error>;
} // namespace epycbench::runtime::ipc
