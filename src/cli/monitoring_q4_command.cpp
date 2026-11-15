#include "cli/monitoring_q4_command.hpp"
#include "cli/monitoring_q4_command_test_access.hpp"
#include "monitoring/host_q4_backend.hpp"
#include "monitoring/host_q4_backend_transport.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "monitoring/privacy_validation.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <charconv>
#include <ostream>
#include <ranges>
namespace epycbench::cli {
namespace {
thread_local detail::MonitoringQ4Dispatch dispatch{};
auto valid_qid(std::string_view value) noexcept -> bool {
    constexpr std::string_view prefix = "MON-007.host-v2.";
    if (!value.starts_with(prefix) || value.size() <= prefix.size() ||
        value.size() > prefix.size() + 64U) return false;
    const auto suffix=value.substr(prefix.size());
    const auto alnum=[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9');};
    return alnum(suffix.front()) && alnum(suffix.back()) &&
           std::ranges::all_of(suffix,[&](char c){return alnum(c)||c=='-';});
}
auto write_exact(std::string_view bytes, std::ostream& stream, std::uint64_t limit,
                 int status) noexcept -> int {
    if (bytes.size() > limit || bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        return status == 0 ? 1 : 2;
    try {
        auto* buffer=stream.rdbuf();
        if (!buffer || buffer->sputn(bytes.data(),static_cast<std::streamsize>(bytes.size())) !=
                           static_cast<std::streamsize>(bytes.size()) || buffer->pubsync()!=0)
            return status == 0 ? 1 : 2;
        return status;
    } catch (...) { return status == 0 ? 1 : 2; }
}
auto fixed_failure(bool usage_value=false) -> monitoring::qualification::detail::MonitoringHostQ4BackendOutput {
    using namespace monitoring::qualification::detail;
    MonitoringHostQ4BackendFailure failure; failure.usage=usage_value;
    failure.terminal_containment=!usage_value;
    auto bytes=serialize_monitoring_host_q4_backend_failure_pre_source(failure);
    return {MonitoringHostQ4BackendOutputKind::failure, bytes ? std::move(*bytes) : std::string{}, 792302U};
}
auto valid_wire(const monitoring::qualification::detail::MonitoringHostQ4BackendOutput& value) noexcept
    -> bool {
    try {
        if (value.canonical_json.empty() || value.canonical_json.size()>value.byte_limit ||
            value.canonical_json.back()!='\n' ||
            value.canonical_json.find('\0')!=std::string::npos) return false;
        auto root=nlohmann::ordered_json::parse(value.canonical_json.begin(),value.canonical_json.end(),nullptr,false);
        if(root.is_discarded()||!root.is_object()||!root.contains("schema")||!root["schema"].is_string()||
           !monitoring::qualification::detail::privacy_safe(root)) return false;
        auto canonical=core::canonicalize_json(root);if(!canonical||*canonical+"\n"!=value.canonical_json)return false;
        std::string_view expected;
        using K=monitoring::qualification::detail::MonitoringHostQ4BackendOutputKind;
        switch(value.kind){case K::success:expected="epycbench.monitoring-host-q4-backend-result/v1";break;case K::failure:expected="epycbench.monitoring-host-q4-backend-error/v1";break;case K::authored:expected="epycbench.monitoring-host-q4-backend-authored/v1";break;default:return false;}
        return root["schema"]==expected;
    } catch (...) {return false;}
}
}
auto run_monitoring_host_q4_command(int argc,char* argv[],
    runtime::ControllerProcessProductionAuthority& authority,std::ostream& out,std::ostream& err) noexcept->int {
    using namespace monitoring::qualification::detail;
    try {
        if(argc!=8||std::string_view{argv[1]}!="__monitoring-host-q4-frequency"||
           std::string_view{argv[2]}!="--repository-root"||std::string_view{argv[3]}.empty()||
           !std::filesystem::path{argv[3]}.is_absolute()||
           std::string_view{argv[4]}!="--qualification-id"||!valid_qid(argv[5])||
           std::string_view{argv[6]}!="--capacity-control-fd") {
            auto value=fixed_failure(true); if(value.canonical_json.empty()) return 1;
            return write_exact(value.canonical_json,err,value.byte_limit,2);
        }
        int capacity_fd{-1};const auto*begin=argv[7];const auto*end=begin+std::char_traits<char>::length(begin);auto parsed=std::from_chars(begin,end,capacity_fd);
        if(parsed.ec!=std::errc{}||parsed.ptr!=end||capacity_fd<3){auto value=fixed_failure(true);if(value.canonical_json.empty())return 1;return write_exact(value.canonical_json,err,value.byte_limit,2);}
        auto result=dispatch?dispatch(argv[3],argv[5],authority,capacity_fd):run_monitoring_host_q4_backend(argv[3],argv[5],authority,capacity_fd);
        if(!result) { auto value=fixed_failure(); if(value.canonical_json.empty()) return 1;
            return write_exact(value.canonical_json,err,value.byte_limit,2); }
        if(!valid_wire(*result)) { auto value=fixed_failure(); if(value.canonical_json.empty()) return 1;
            return write_exact(value.canonical_json,err,value.byte_limit,2); }
        const bool success=result->kind==MonitoringHostQ4BackendOutputKind::success;
        return write_exact(result->canonical_json,success?out:err,result->byte_limit,
                           success?0:2);
    } catch (...) { auto value=fixed_failure(); if(value.canonical_json.empty()) return 1;
        return write_exact(value.canonical_json,err,value.byte_limit,2); }
}
void detail::MonitoringQ4CommandTestAccess::set(MonitoringQ4Dispatch value) noexcept {dispatch=value;}
void detail::MonitoringQ4CommandTestAccess::reset() noexcept {dispatch=nullptr;}
}
