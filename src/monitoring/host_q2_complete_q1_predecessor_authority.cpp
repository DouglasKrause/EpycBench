#include "monitoring/host_q2_complete_q1_predecessor_authority.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "monitoring/privacy_validation.hpp"
#include "monitoring/host_q1_plan_v2_value.hpp"
#include "monitoring/host_q1_validated_public_values.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <span>
#include <sys/stat.h>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
MonitoringQ2CompleteQ1PredecessorBinding::MonitoringQ2CompleteQ1PredecessorBinding(
    const MonitoringQ1ValidatedPublicValues& values,std::string_view current,std::string_view profile,
    std::string_view plan,MonitoringQ2CompleteQ1PredecessorOperations ops) noexcept
    :qualification_id{values.qualification_id()},current_commit{current},profile_bytes{profile},
     q1_plan_bytes{plan},historical{ops},validated{&values} {}
namespace {
using Json = nlohmann::json;
auto bootstrap_error() noexcept -> core::Error {
    core::Error result{};result.category=core::ErrorCategory::monitoring;
    result.severity=core::ErrorSeverity::error;
    result.continuation=core::ContinuationPolicy::abort_command;return result;
}
class BoundedSax final : public nlohmann::json_sax<Json> {
  public:
    bool null() override { return value(); } bool boolean(bool) override { return value(); }
    bool number_integer(number_integer_t) override { return value(); }
    bool number_unsigned(number_unsigned_t) override { return value(); }
    bool number_float(number_float_t,const string_t&) override { return value(); }
    bool string(string_t& s) override { return s.size() <= 1024U && value(); }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return enter(); }
    bool key(string_t& s) override { return s.size() <= 255U && value(); }
    bool end_object() override { --depth_; return true; }
    bool start_array(std::size_t) override { return enter(); }
    bool end_array() override { --depth_; return true; }
    bool parse_error(std::size_t,const std::string&,const nlohmann::detail::exception&) override { return false; }
  private:
    bool value() { return ++nodes_ <= 1100000U; }
    bool enter() { if (!value() || depth_ == 16U) return false; ++depth_; return true; }
    std::size_t nodes_{},depth_{};
};
auto failure(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q2.complete_q1_predecessor.invalid"},
            .message = "Monitoring Q2 complete Q1 predecessor is invalid",
            .operation = "authenticate_monitoring_q2_complete_q1_predecessor",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
template <typename Names>
auto exact_range(const Json& value, const Names& names) -> bool {
    if (!value.is_object() || value.size() != names.size()) return false;
    return std::ranges::all_of(names, [&](auto name) { return value.contains(name); });
}
auto exact(const Json& value, std::initializer_list<std::string_view> names) -> bool {
    return exact_range(value, names);
}
template <typename T, std::size_t N>
auto exact(const Json& value, const std::array<T, N>& names) -> bool { return exact_range(value, names); }
auto hex(std::string_view value, std::size_t size) -> bool {
    return value.size() == size && std::ranges::all_of(value, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
auto commit(std::string_view value) -> bool { return hex(value, 40) || hex(value, 64); }
auto qid(std::string_view value) -> bool {
    constexpr std::string_view prefix{"MON-007.host-v2."};
    if (!value.starts_with(prefix)) return false;
    value.remove_prefix(prefix.size());
    return !value.empty() && value.size() <= 64 && value.front() != '-' && value.back() != '-' &&
           std::ranges::all_of(value, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
           });
}
auto parse(std::string_view bytes, std::size_t limit) -> std::optional<Json> {
    if (bytes.empty() || bytes.size() > limit || bytes.back() != '\n') return std::nullopt;
    BoundedSax sax;
    if (!Json::sax_parse(bytes.begin(), bytes.end(), &sax)) return std::nullopt;
    auto value = Json::parse(bytes, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return std::nullopt;
    auto canonical = core::canonicalize_json(value);
    if (!canonical || *canonical + "\n" != bytes) return std::nullopt;
    return value;
}
auto positive_u64(const Json& value) -> bool {
    return value.is_number_unsigned() && value.get<std::uint64_t>() != 0;
}
auto u64(const Json& value) -> bool { return value.is_number_unsigned(); }
auto u32(const Json& value) -> bool {
    return u64(value) && value.get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max();
}
auto counters(const Json& value) -> bool {
    return exact(value,{"minor_faults","major_faults"})&&u64(value["minor_faults"])&&u64(value["major_faults"]);
}
auto summary_shape(const Json& value,bool worker) -> bool {
    return worker?exact(value,{"worker_id","count","minimum_elapsed_ns","maximum_elapsed_ns",
      "lower_median_elapsed_ns","minor_equality_count","major_equality_count"}):
      exact(value,{"count","minimum_elapsed_ns","maximum_elapsed_ns","lower_median_elapsed_ns",
      "minor_equality_count","major_equality_count"});
}
auto validate_inventory(const Json& e,const Json& cp) -> bool {
    const auto n=cp["observations_per_worker"].get<std::uint64_t>();
    const auto& workers=e["workers"];if(!workers.is_array()||workers.empty()||workers.size()>512U)return false;
    std::vector<std::uint32_t> ids;ids.reserve(workers.size());
    for(const auto& w:workers){if(!exact(w,{"worker_id","observation_count"})||!u32(w["worker_id"])||
      !positive_u64(w["observation_count"])||w["observation_count"]!=n)return false;
      const auto id=w["worker_id"].get<std::uint32_t>();if(std::ranges::find(ids,id)!=ids.end())return false;ids.push_back(id);}
    if(n>std::numeric_limits<std::uint64_t>::max()/workers.size())return false;
    const auto total=n*workers.size();const auto& observations=e["observations"];
    if(!observations.is_array()||observations.size()!=total)return false;
    std::vector<std::vector<std::uint64_t>> durations(workers.size());std::vector<std::uint64_t> global;global.reserve(observations.size());
    for(std::size_t index=0;index<observations.size();++index){const auto& o=observations[index];
      if(!exact(o,{"cycle_index","worker_id","collection_start_ns","collection_end_ns","elapsed_ns",
        "oracle_before","production","oracle_after","minor_equal","major_equal"})||!u64(o["cycle_index"])||
        !u32(o["worker_id"])||!u64(o["collection_start_ns"])||!u64(o["collection_end_ns"])||!u64(o["elapsed_ns"])||
        !counters(o["oracle_before"])||!counters(o["production"])||!counters(o["oracle_after"])||
        o["minor_equal"]!=true||o["major_equal"]!=true)return false;
      const auto worker=index%workers.size();const auto cycle=index/workers.size();
      const auto start=o["collection_start_ns"].get<std::uint64_t>(),end=o["collection_end_ns"].get<std::uint64_t>();
      if(o["worker_id"]!=ids[worker]||o["cycle_index"]!=cycle||end<start||o["elapsed_ns"]!=end-start||
         o["oracle_before"]!=o["production"]||o["production"]!=o["oracle_after"])return false;
      durations[worker].push_back(end-start);global.push_back(end-start);}
    const auto check_summary=[](const Json& s,std::vector<std::uint64_t> values){std::ranges::sort(values);
      return s["count"]==values.size()&&s["minimum_elapsed_ns"]==values.front()&&s["maximum_elapsed_ns"]==values.back()&&
        s["lower_median_elapsed_ns"]==values[(values.size()-1U)/2U]&&s["minor_equality_count"]==values.size()&&s["major_equality_count"]==values.size();};
    const auto& summaries=e["summaries"];
    if(!exact(summaries,{"workers","global"})||!summaries["workers"].is_array()||summaries["workers"].size()!=workers.size()||
       !summary_shape(summaries["global"],false)||!check_summary(summaries["global"],global))return false;
    for(std::size_t i=0;i<workers.size();++i){const auto& s=summaries["workers"][i];if(!summary_shape(s,true)||s["worker_id"]!=ids[i]||!check_summary(s,durations[i]))return false;}
    return true;
}
auto validate_q1(const Json& v, std::string_view expected_qid, std::string_view profile_sha,
                 std::string_view environment_digest, std::string_view host_sha,
                 std::string_view q0_sha, std::string_view plan_sha) -> std::optional<std::string> {
    constexpr std::array top{"schema", "qualification_id", "tool", "repository", "build", "profile",
        "q0_provenance", "collection_plan", "host_environment", "qualification_plan", "phase",
        "prerequisites", "implementation", "oracle", "workers", "observations", "summaries",
        "source_semantics", "known_event", "finalization", "disposition", "privacy_validation"};
    if (!exact(v, top) || v["schema"] != "epycbench.monitoring-qualification-evidence/v3" ||
        v["qualification_id"] != expected_qid || v["phase"] != "Q1" ||
        v["disposition"] != "pass" || v["privacy_validation"] != "passed" ||
        v["prerequisites"] != Json::array() ||
        v["tool"] != Json{{"identity", "epycbench.monitoring-qualification-tool/v1"}, {"version", "1.0.0"}})
        return std::nullopt;
    if(!exact(v["build"],{"identity"})||!v["build"]["identity"].is_string()||
       !v["build"]["identity"].get_ref<const std::string&>().starts_with("sha256:")||
       !hex(std::string_view{v["build"]["identity"].get_ref<const std::string&>()}.substr(7),64))return std::nullopt;
    const auto& repository = v["repository"];
    if (!exact(repository, {"commit", "dirty"}) || repository["dirty"] != false ||
        !repository["commit"].is_string() || !commit(repository["commit"].get_ref<const std::string&>()))
        return std::nullopt;
    const auto historical = repository["commit"].get<std::string>();
    const auto& profile = v["profile"];
    if (profile != Json{{"schema", "epycbench.monitoring-qualification-profile/v2"},
                        {"identity", "epycbench.monitoring-qualification.host-generic/v2"},
                        {"stage", "final_frozen_profile"}, {"sha256", profile_sha}}) return std::nullopt;
    const auto& qp = v["q0_provenance"];
    if (!exact(qp, {"qualification_id", "manifest_sha256", "checksums_sha256",
                    "host_environment_sha256", "q0b_evidence_sha256"}) ||
        qp["qualification_id"] != expected_qid || qp["host_environment_sha256"] != host_sha ||
        qp["q0b_evidence_sha256"] != q0_sha) return std::nullopt;
    for (auto key : {"manifest_sha256", "checksums_sha256", "host_environment_sha256", "q0b_evidence_sha256"})
        if (!qp[key].is_string() || !hex(qp[key].get_ref<const std::string&>(), 64)) return std::nullopt;
    const auto& cp = v["collection_plan"];
    constexpr std::array collection{"schema", "path", "sha256", "repository_commit",
        "observations_per_worker", "conservative_observation_capacity_per_worker",
        "production_warmup_observations", "sequencing", "stopping", "retry", "discard",
        "execution_deadline_seconds", "success_envelope_bytes", "evidence_artifact_bytes",
        "build_manifest_bytes", "maximum_successor_tree_bytes", "derivation"};
    if (!exact(cp, collection) || cp["schema"] != "epycbench.monitoring-q1-page-fault-collection-plan/v2" ||
        cp["path"] != "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json" ||
        cp["sha256"] != plan_sha || cp["repository_commit"] != historical ||
        cp["production_warmup_observations"] != 0 || cp["sequencing"] != "round_robin_cycle_then_worker_id" ||
        cp["stopping"] != "fixed_inventory" || cp["retry"] != "forbidden" || cp["discard"] != "forbidden" ||
        cp["success_envelope_bytes"] != 10485760 || cp["evidence_artifact_bytes"] != 8388608 ||
        cp["build_manifest_bytes"] != 1048576 || !positive_u64(cp["observations_per_worker"]) ||
        !positive_u64(cp["conservative_observation_capacity_per_worker"]) ||
        !positive_u64(cp["execution_deadline_seconds"]) || !positive_u64(cp["maximum_successor_tree_bytes"]))
        return std::nullopt;
    if (v["host_environment"] != Json{{"schema", "epycbench.monitoring-host-environment/v2"},
                                      {"sha256", environment_digest}} ||
        !exact(v["qualification_plan"], {"schema", "sha256"}) ||
        v["qualification_plan"]["schema"] != "epycbench.monitoring-host-qualification-plan/v2") return std::nullopt;
    if (v["implementation"] != Json{{"source", "linux-proc-task-stat"}, {"fields", Json::array({10, 12})},
          {"counters", "cumulative-uint64"}, {"clock", "clock-monotonic-raw"}} ||
        v["oracle"] != Json{{"source", "linux-getrusage-thread"}, {"scope", "target-worker"},
          {"comparison", "before-production-after-equality"}} ||
        v["known_event"] != Json{{"required", false}, {"disposition", "not_performed"}} ||
        v["finalization"] != Json{{"disposition", "succeeded"}}) return std::nullopt;
    const auto& derivation=cp["derivation"];
    if(!exact(derivation,{"algorithm","mode","target_total_observations","worker_count",
      "conservative_observation_capacity_per_worker","observations_per_worker","total_observations",
      "per_observation_allowance_ms","fixed_reserve_seconds","execution_deadline_seconds"})||
      derivation["algorithm"]!="epycbench.monitoring-q1-plan-derivation/deterministic-v1"||
      (derivation["mode"]!="default"&&derivation["mode"]!="governed_override")||
      derivation["target_total_observations"]!=4096||derivation["worker_count"]!=v["workers"].size()||
      derivation["conservative_observation_capacity_per_worker"]!=cp["conservative_observation_capacity_per_worker"]||
      derivation["observations_per_worker"]!=cp["observations_per_worker"]||
      derivation["execution_deadline_seconds"]!=cp["execution_deadline_seconds"]||
      !positive_u64(derivation["total_observations"])||!positive_u64(derivation["per_observation_allowance_ms"])||
      !positive_u64(derivation["fixed_reserve_seconds"]))return std::nullopt;
    const auto worker_count=static_cast<std::uint64_t>(v["workers"].size());
    const auto n=cp["observations_per_worker"].get<std::uint64_t>();
    const auto capacity=cp["conservative_observation_capacity_per_worker"].get<std::uint64_t>();
    if(n>capacity||worker_count>std::numeric_limits<std::uint64_t>::max()/n||
       derivation["total_observations"]!=worker_count*n)return std::nullopt;
    const auto default_n=std::min(capacity,std::max<std::uint64_t>(1,4096/worker_count));
    const auto allowance=derivation["per_observation_allowance_ms"].get<std::uint64_t>();
    const auto reserve=derivation["fixed_reserve_seconds"].get<std::uint64_t>();
    if(worker_count*n>std::numeric_limits<std::uint64_t>::max()/allowance)return std::nullopt;
    const auto product=worker_count*n*allowance;
    const auto increment=product/1000+(product%1000!=0?1U:0U);
    if(reserve>std::numeric_limits<std::uint64_t>::max()-increment||
       cp["execution_deadline_seconds"]!=reserve+increment)return std::nullopt;
    if((derivation["mode"]=="default"&&(n!=default_n||allowance!=100||reserve!=120))||
       (derivation["mode"]=="governed_override"&&(n>default_n||(n==default_n&&allowance==100&&reserve==120))))return std::nullopt;
    const Json passed={{"availability","passed"},{"readability","passed"},{"field_projection","passed"},
      {"counter_width","passed"},{"cumulative_units","passed"},{"worker_scope","passed"},
      {"worker_tid_association","passed"},{"oracle_agreement","passed"},{"operational_errors","passed"},{"privacy","passed"}};
    if(v["source_semantics"]!=passed||!validate_inventory(v,cp))return std::nullopt;
    return historical;
}
auto profile_core_valid(const Json& p,std::string_view expected_qid)->bool {
    constexpr std::array fields{"schema","identity","version","stage","qualification_contract","tool",
      "evidence_schema","production_authority_schema","configurations","kernel_families","monitoring",
      "perturbation","service_placement","identity_policy","privacy_policy","input_template",
      "q0b_provenance","qualification_stages"};
    if(!exact(p,fields)||p["schema"]!="epycbench.monitoring-qualification-profile/v2"||
      p["identity"]!="epycbench.monitoring-qualification.host-generic/v2"||p["version"]!=2||
      p["stage"]!="final_frozen_profile"||p["qualification_contract"]!="MON-007/v2"||
      p["tool"]!=Json{{"identity","epycbench.monitoring-qualification-tool/v1"},{"version","1.0.0"}}||
      p["evidence_schema"]!="epycbench.monitoring-qualification-evidence/v2"||
      p["production_authority_schema"]!="epycbench.monitoring-production-authority/v2"||
      p["privacy_policy"]!="epycbench.monitoring-qualification-privacy/v1")return false;
    const auto& q0=p["q0b_provenance"];
    constexpr std::array q0fields{"qualification_id","policy","input_template_sha256","host_environment",
      "host_qualification_plan","characterization","q0b_production_inputs","q0b_base_environment",
      "family_production_admissions","repository_commit","build_identity","initial_manifest_sha256",
      "initial_checksums_sha256"};
    if(!exact(q0,q0fields)||q0["qualification_id"]!=expected_qid||!q0["repository_commit"].is_string()||
      !commit(q0["repository_commit"].get_ref<const std::string&>()))return false;
    for(auto key:{"input_template_sha256","build_identity","initial_manifest_sha256","initial_checksums_sha256"})
      if(!q0[key].is_string()||!hex(q0[key].get_ref<const std::string&>(),64))return false;
    return exact(q0["host_environment"],{"schema","digest","artifact_sha256"})&&
      q0["host_environment"]["schema"]=="epycbench.monitoring-host-environment/v2"&&
      exact(q0["host_qualification_plan"],{"schema","digest"})&&
      q0["host_qualification_plan"]["schema"]=="epycbench.monitoring-host-qualification-plan/v2"&&
      exact(q0["characterization"],{"schema","artifact_sha256"})&&
      q0["characterization"]["schema"]=="epycbench.monitoring-work-characterization/v2";
}
auto q2_privacy_safe(const Json& value)->bool {
    constexpr std::string_view qid_pattern{"^MON-007\\.host-v2\\.[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$"};
    if(value.is_string()&&value.get_ref<const std::string&>()==qid_pattern)return true;
    if(value.is_object()){for(const auto& [key,child]:value.items())if(privacy_prohibited(key)||!q2_privacy_safe(child))return false;return true;}
    if(value.is_array()){for(const auto& child:value)if(!q2_privacy_safe(child))return false;return true;}
    return !value.is_string()||!privacy_prohibited(value.get_ref<const std::string&>());
}
auto os_failure(std::string classification, int native) -> core::Error;
using CleanupStock = std::array<std::optional<core::Error>,
    MonitoringQ2CompleteQ1PredecessorFailure::maximum_cleanup_failures>;
auto make_cleanup_stock() -> CleanupStock {
    CleanupStock result;
    for (auto& value : result) {
        value.emplace(failure("snapshot_cleanup"));
        value->context["native_errno"]=0;
    }
    return result;
}
struct CleanupLedger final {
    struct Event final { int native{}; std::size_t stock_slot{}; std::optional<core::Error> upstream; };
    std::array<Event,MonitoringQ2CompleteQ1PredecessorFailure::maximum_cleanup_failures> values{};
    std::size_t count{};
    CleanupStock* stock{};
    std::size_t stock_index{};
    void reserve() noexcept { if(count==values.size())std::terminate(); }
    void append_errno(int native) noexcept {
        reserve();
        if (!stock || stock_index == stock->size() || !(*stock)[stock_index]) std::terminate();
        values[count++]={.native=native,.stock_slot=stock_index++,.upstream=std::nullopt};
    }
    void append(core::Error value) noexcept { reserve();values[count++].upstream.emplace(std::move(value)); }
    void transfer_to(MonitoringQ2CompleteQ1PredecessorFailure& target) noexcept {
        for(std::size_t index{};index<count;++index) {
            if(values[index].upstream){target.append(std::move(*values[index].upstream));continue;}
            try { target.append(os_failure("snapshot_cleanup",values[index].native)); }
            catch (...) {
                if (!stock || values[index].stock_slot == stock->size() ||
                    !(*stock)[values[index].stock_slot]) std::terminate();
                auto fallback=std::move(*(*stock)[values[index].stock_slot]);
                fallback.context["native_errno"]=values[index].native;
                target.append(std::move(fallback));
            }
        }
    }
};
struct HistoricalOwnershipGuard final {
    MonitoringQ2CompleteQ1PredecessorOperations operations;
    bool active{true};
    auto retire() noexcept -> std::expected<void,core::Error> {
        if(!active)return {};
        active=false;
        if(operations.retire)return operations.retire(operations.context);
        return {};
    }
    void release() noexcept { active=false; }
    ~HistoricalOwnershipGuard() noexcept { if(active)static_cast<void>(retire()); }
};
auto os_failure(std::string classification, int native = 0) -> core::Error {
    auto result = failure(std::move(classification));
    if (native != 0) result.os_error = core::OsError::capture(std::error_code{native, std::generic_category()});
    return result;
}
auto snapshot_identity(const struct stat& s)
    -> MonitoringQ2CompleteQ1PredecessorAuthority::SnapshotIdentity {
    return {static_cast<std::uint64_t>(s.st_dev),static_cast<std::uint64_t>(s.st_ino),
      static_cast<std::uint64_t>(s.st_size),static_cast<std::uint32_t>(s.st_mode),
      static_cast<std::uint32_t>(s.st_uid),static_cast<std::uint32_t>(s.st_nlink)};
}
template<typename Operations>
auto read_fd(int fd, std::size_t limit,const Operations& operations) -> std::expected<std::string,core::Error> {
    struct stat s{};if (operations.stat_fd(operations.context,fd,&s)!=0) return std::unexpected{os_failure("snapshot_stat",errno)};
    if (s.st_size<0 || static_cast<std::uint64_t>(s.st_size)>limit)
        return std::unexpected{failure("snapshot_size")};
    std::string bytes(static_cast<std::size_t>(s.st_size),'\0');std::size_t at{};
    while(at<bytes.size()) { const auto got=operations.read_at(operations.context,fd,bytes.data()+at,bytes.size()-at,static_cast<off_t>(at));
      if(got<0&&errno==EINTR)continue;
      if(got<=0)return std::unexpected{os_failure("snapshot_read",got<0?errno:EIO)};
      at+=static_cast<std::size_t>(got); }
    char extra{};ssize_t got{};do{got=operations.read_at(operations.context,fd,&extra,1,static_cast<off_t>(at));}
    while(got<0&&errno==EINTR);
    if(got!=0)return std::unexpected{os_failure("snapshot_growth",got<0?errno:EFBIG)};
    return bytes;
}
auto inventory(int fd) -> std::expected<std::set<std::string>,MonitoringQ2CompleteQ1PredecessorAuthority::InventoryFailure> {
    const int copy=::dup(fd);if(copy<0)return std::unexpected{MonitoringQ2CompleteQ1PredecessorAuthority::InventoryFailure{os_failure("snapshot_inventory",errno),std::nullopt}};
    DIR* raw=::fdopendir(copy);if(!raw){const int saved=errno;MonitoringQ2CompleteQ1PredecessorAuthority::InventoryFailure result{os_failure("snapshot_inventory",saved),std::nullopt};
      if(::close(copy)!=0)result.cleanup.emplace(os_failure("snapshot_inventory_cleanup",errno));
      return std::unexpected{std::move(result)};}
    std::set<std::string> names;::rewinddir(raw);errno=0;while(const auto* entry=::readdir(raw)){
      const std::string_view name{entry->d_name};if(name!="."&&name!="..")names.emplace(name);errno=0;}
    const int saved=errno;const int closed=::closedir(raw);const int close_errno=closed==0?0:errno;
    if(saved!=0){MonitoringQ2CompleteQ1PredecessorAuthority::InventoryFailure result{os_failure("snapshot_inventory",saved),std::nullopt};
      if(close_errno!=0)result.cleanup.emplace(os_failure("snapshot_inventory_cleanup",close_errno));
      return std::unexpected{std::move(result)};}
    if(close_errno!=0)return std::unexpected{MonitoringQ2CompleteQ1PredecessorAuthority::InventoryFailure{os_failure("snapshot_inventory_cleanup",close_errno),std::nullopt}};
    return names;
}
auto native_duplicate(void*,int fd) noexcept->int{return ::fcntl(fd,F_DUPFD_CLOEXEC,0);}
auto native_open(void*,int parent,const char* name,int flags) noexcept->int{return ::openat(parent,name,flags);}
auto native_stat(void*,int fd,struct stat* value) noexcept->int{return ::fstat(fd,value);}
auto native_stat_at(void*,int parent,const char* name,struct stat* value,int flags) noexcept->int{return ::fstatat(parent,name,value,flags);}
auto native_read(void*,int fd,void* data,std::size_t size,off_t offset) noexcept->ssize_t{return ::pread(fd,data,size,offset);}
auto native_inventory(void*,int fd)->std::expected<std::set<std::string>,MonitoringQ2CompleteQ1PredecessorAuthority::InventoryFailure>{return inventory(fd);}
auto native_close(void*,int fd) noexcept->int{return ::close(fd);}
auto native_euid(void*) noexcept->unsigned int{return ::geteuid();}
void native_fault(void*,unsigned int){}
}

MonitoringQ2CompleteQ1PredecessorAuthority::MonitoringQ2CompleteQ1PredecessorAuthority(
    std::string commit_value, std::string qid_value, std::string current_value,
    std::string profile_value, std::string plan_value, std::string environment_value,
    std::string qualification_plan_value, std::string q0_commit_value,
    core::Sha256Digest manifest, core::Sha256Digest checksums,
    core::Sha256Digest q1, MonitoringQ2CompleteQ1PredecessorOperations operations,
    CleanupStock cleanup_stock) noexcept
    : commit_{std::move(commit_value)}, environment_{std::move(environment_value)},
      qualification_plan_{std::move(qualification_plan_value)},
      q0_commit_{std::move(q0_commit_value)},
      qualification_id_{std::move(qid_value)}, current_commit_{std::move(current_value)},
      profile_bytes_{std::move(profile_value)}, q1_plan_bytes_{std::move(plan_value)},
      manifest_{manifest}, checksums_{checksums}, q1_{q1},
      operations_{operations}, cleanup_stock_{std::move(cleanup_stock)} {
      retirement_primary_.emplace(std::move(*cleanup_stock_[0]));
      descriptors_.fill(-1); }
MonitoringQ2CompleteQ1PredecessorAuthority::MonitoringQ2CompleteQ1PredecessorAuthority(
    MonitoringQ2CompleteQ1PredecessorAuthority&& other) noexcept
    : commit_{std::move(other.commit_)}, manifest_{other.manifest_}, checksums_{other.checksums_},
      q1_{other.q1_}, operations_{other.operations_}, active_{std::exchange(other.active_, false)} {
    snapshot_operations_=other.snapshot_operations_;
    cleanup_stock_=std::move(other.cleanup_stock_);
    retirement_primary_=std::move(other.retirement_primary_);
    environment_=std::move(other.environment_);qualification_plan_=std::move(other.qualification_plan_);
    q0_commit_=std::move(other.q0_commit_);
    qualification_id_=std::move(other.qualification_id_);current_commit_=std::move(other.current_commit_);
    profile_bytes_=std::move(other.profile_bytes_);q1_plan_bytes_=std::move(other.q1_plan_bytes_);
    descriptors_=std::exchange(other.descriptors_,std::array<int,11>{});other.descriptors_.fill(-1);
    identities_=other.identities_;retained_bytes_=std::move(other.retained_bytes_);
}
MonitoringQ2CompleteQ1PredecessorAuthority::~MonitoringQ2CompleteQ1PredecessorAuthority() noexcept {
    if (active_) static_cast<void>(retire());
}
auto MonitoringQ2CompleteQ1PredecessorAuthority::retire()
    -> std::expected<void, MonitoringQ2CompleteQ1PredecessorFailure> {
    if (!active_) return std::unexpected{MonitoringQ2CompleteQ1PredecessorFailure{bootstrap_error()}};
    active_ = false;
    CleanupLedger failures{.stock=&cleanup_stock_,.stock_index=1};
    for(auto it=descriptors_.rbegin();it!=descriptors_.rend();++it){const int fd=std::exchange(*it,-1);
      if(fd>=0&&snapshot_operations_.close_fd&&snapshot_operations_.close_fd(snapshot_operations_.context,fd)!=0)failures.append_errno(errno);}
    if (operations_.retire) { auto result=operations_.retire(operations_.context);if(!result)failures.append(std::move(result.error())); }
    if(failures.count!=0){
      if(!retirement_primary_)std::terminate();
      MonitoringQ2CompleteQ1PredecessorFailure result{std::move(*retirement_primary_)};
      failures.transfer_to(result);
      result.primary=std::move(*result.cleanup[0]);
      for(std::size_t index=1;index<result.cleanup_count;++index)result.cleanup[index-1]=std::move(result.cleanup[index]);
      --result.cleanup_count;
      return std::unexpected{std::move(result)};}
    return {};
}
auto MonitoringQ2CompleteQ1PredecessorAuthority::authenticate(
    MonitoringQ2CompleteQ1PredecessorInput input, MonitoringQ2CompleteQ1PredecessorOperations operations,
    CleanupStock& cleanup_stock)
    -> std::expected<MonitoringQ2CompleteQ1PredecessorAuthority,
                     MonitoringQ2CompleteQ1PredecessorFailure> {
    try {
        if (!qid(input.qualification_id) || !commit(input.current_commit))
            return std::unexpected{failure("identity")};
        auto manifest = parse(input.manifest_bytes, 1048576);
        auto q1 = parse(input.q1_evidence_bytes, 8388608);
        if (!manifest || !q1 || !privacy_safe(*manifest) || !privacy_safe(*q1))
            return std::unexpected{failure("canonical_json_privacy")};
        const auto profile_sha = core::sha256(input.profile_bytes).to_hex();
        const auto host_sha = core::sha256(input.host_environment_bytes).to_hex();
        const auto q0_sha = core::sha256(input.q0b_evidence_bytes).to_hex();
        const auto q1_sha = core::sha256(input.q1_evidence_bytes);
        const auto plan_sha = core::sha256(input.q1_plan_bytes).to_hex();
        auto governed_plan=MonitoringQ1PlanV2Value::decode(input.q1_plan_bytes,input.current_commit);
        if(!governed_plan||governed_plan->qualification_id()!=input.qualification_id||
           governed_plan->profile_sha256().to_hex()!=profile_sha)
            return std::unexpected{failure("governed_q1_plan")};
        constexpr std::array paths{"host-environment.json", "q0b/evidence.json", "q1/evidence.json"};
        const std::array digests{host_sha, q0_sha, q1_sha.to_hex()};
        if (!exact(*manifest, {"schema", "qualification_id", "tool", "profile", "environment_digest", "artifacts"}) ||
            (*manifest)["schema"] != "epycbench.monitoring-qualification-manifest/v3" ||
            (*manifest)["qualification_id"] != input.qualification_id ||
            (*manifest)["tool"] != Json{{"identity", "epycbench.monitoring-qualification-tool/v1"}, {"version", "1.0.0"}} ||
            (*manifest)["profile"] != Json{{"identity", "epycbench.monitoring-qualification.host-generic/v2"},
                {"stage", "final_frozen_profile"}, {"sha256", profile_sha}} ||
            !(*manifest)["environment_digest"].is_string() ||
            !(*manifest)["environment_digest"].get_ref<const std::string&>().starts_with("sha256:") ||
            !(*manifest)["artifacts"].is_array() || (*manifest)["artifacts"].size() != paths.size())
            return std::unexpected{failure("manifest")};
        for (std::size_t i = 0; i < paths.size(); ++i)
            if ((*manifest)["artifacts"][i] != Json{{"path", paths[i]}, {"sha256", digests[i]}})
                return std::unexpected{failure("inventory")};
        std::string expected = core::sha256(input.manifest_bytes).to_hex() + "  manifest.json\n";
        for (std::size_t i = 0; i < paths.size(); ++i) expected += digests[i] + "  " + paths[i] + "\n";
        if (input.checksums_bytes != expected) return std::unexpected{failure("checksums")};
        const auto environment = (*manifest)["environment_digest"].get<std::string>();
        auto historical = validate_q1(*q1, input.qualification_id, profile_sha, environment,
                                      host_sha, q0_sha, plan_sha);
        if (!historical) return std::unexpected{failure("q1_evidence")};
        const auto& cp=(*q1)["collection_plan"];
        if(cp["observations_per_worker"]!=governed_plan->observations_per_worker()||
           cp["conservative_observation_capacity_per_worker"]!=governed_plan->capacity()||
           cp["execution_deadline_seconds"]!=governed_plan->execution_deadline_seconds()||
           cp["maximum_successor_tree_bytes"]!=governed_plan->maximum_successor_tree_bytes()||
           (*q1)["qualification_plan"]["sha256"]!="sha256:"+governed_plan->qualification_plan_digest().to_hex())
            return std::unexpected{failure("q1_plan_evidence_binding")};
        if((*q1)["workers"].size()!=governed_plan->worker_ids().size())
            return std::unexpected{failure("q1_plan_worker_binding")};
        for(std::size_t index{};index<governed_plan->worker_ids().size();++index)
            if((*q1)["workers"][index]["worker_id"]!=governed_plan->worker_ids()[index].value())
                return std::unexpected{failure("q1_plan_worker_binding")};
        auto profile = parse(input.profile_bytes, 1U << 20U);
        if (!profile) return std::unexpected{failure("profile_canonical")};
        if (!q2_privacy_safe(*profile)) return std::unexpected{failure("profile_privacy")};
        if (!profile_core_valid(*profile,input.qualification_id)) return std::unexpected{failure("profile_schema")};
        if (!profile->contains("q0b_provenance") ||
            !(*profile)["q0b_provenance"].is_object() ||
            !(*profile)["q0b_provenance"].contains("repository_commit") ||
            !(*profile)["q0b_provenance"]["repository_commit"].is_string() ||
            !commit((*profile)["q0b_provenance"]["repository_commit"].get_ref<const std::string&>()))
            return std::unexpected{failure("profile_provenance")};
        const auto& q0p=(*profile)["q0b_provenance"];
        const auto& q1p=(*q1)["q0_provenance"];
        if(q1p["manifest_sha256"]!=q0p["initial_manifest_sha256"]||
           q1p["checksums_sha256"]!=q0p["initial_checksums_sha256"]||
           q1p["host_environment_sha256"]!=q0p["host_environment"]["artifact_sha256"]||
           q1p["q0b_evidence_sha256"]!=q0p["characterization"]["artifact_sha256"]||
           (*q1)["host_environment"]["sha256"]!=q0p["host_environment"]["digest"]||
           (*q1)["qualification_plan"]["sha256"]!=q0p["host_qualification_plan"]["digest"])
            return std::unexpected{failure("q0_q1_binding")};
        const auto& derivation=cp["derivation"];
        const auto expected_mode=governed_plan->derivation_mode()==MonitoringQ1PlanDerivationMode::default_mode
          ?"default":"governed_override";
        if(derivation["mode"]!=expected_mode||
           derivation["total_observations"]!=governed_plan->total_observations()||
           derivation["per_observation_allowance_ms"]!=governed_plan->per_observation_allowance_ms()||
           derivation["fixed_reserve_seconds"]!=governed_plan->fixed_reserve_seconds())
          return std::unexpected{failure("q1_plan_derivation_binding")};
        const auto qualification_plan = (*q1)["qualification_plan"]["sha256"].get<std::string>();
        if (!qualification_plan.starts_with("sha256:") || !hex(std::string_view{qualification_plan}.substr(7),64))
            return std::unexpected{failure("qualification_plan_digest")};
        if (!operations.authenticate_historical)
            return std::unexpected{failure("historical_operations")};
        auto q0_commit=(*profile)["q0b_provenance"]["repository_commit"].get<std::string>();
        auto history = operations.authenticate_historical(operations.context, *historical,
            input.current_commit, q0_commit, input.profile_bytes, input.q1_plan_bytes);
        if (!history) {
          MonitoringQ2CompleteQ1PredecessorFailure converted{std::move(history.error().primary)};
          if(history.error().cleanup_count>history.error().cleanup.size())
            return std::unexpected{failure("historical_failure_invariant")};
          for(std::size_t index{};index<history.error().cleanup_count;++index) {
            if(!history.error().cleanup[index])return std::unexpected{failure("historical_failure_invariant")};
            converted.append(std::move(*history.error().cleanup[index]));
          }
          return std::unexpected{std::move(converted)};
        }
        MonitoringQ2CompleteQ1PredecessorAuthority result{std::move(*historical),
            std::move(input.qualification_id),std::move(input.current_commit),
            std::move(input.profile_bytes),std::move(input.q1_plan_bytes),
            std::move(environment),std::move(qualification_plan),std::move(q0_commit),
            core::sha256(input.manifest_bytes), core::sha256(input.checksums_bytes), q1_sha, operations,
            std::move(cleanup_stock)};
        return result;
    } catch (...) { return std::unexpected{failure("contained")}; }
}

auto MonitoringQ2CompleteQ1PredecessorAuthority::retain(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ2CompleteQ1PredecessorBinding& binding)
    -> std::expected<MonitoringQ2CompleteQ1PredecessorAuthority,
                     MonitoringQ2CompleteQ1PredecessorFailure> {
    return retain_with(root.descriptor(),binding,{nullptr,native_duplicate,native_open,native_stat,
      native_stat_at,native_read,native_inventory,native_close,native_euid,native_fault});
}
auto MonitoringQ2CompleteQ1PredecessorAuthority::retain_with_fault(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ2CompleteQ1PredecessorBinding& binding,void* context,
    void (*fault)(void*,unsigned int))
    -> std::expected<MonitoringQ2CompleteQ1PredecessorAuthority,
                     MonitoringQ2CompleteQ1PredecessorFailure> {
    return retain_with(root.descriptor(),binding,{context,native_duplicate,native_open,native_stat,
      native_stat_at,native_read,native_inventory,native_close,native_euid,fault});
}
auto MonitoringQ2CompleteQ1PredecessorAuthority::retain_with(
    int root_descriptor,const MonitoringQ2CompleteQ1PredecessorBinding& binding,
    SnapshotOperations snapshot_operations)
    -> std::expected<MonitoringQ2CompleteQ1PredecessorAuthority,
                     MonitoringQ2CompleteQ1PredecessorFailure> {
    HistoricalOwnershipGuard historical_guard{binding.historical};
    std::optional<core::Error> stock_primary;
    try { stock_primary.emplace(failure("cleanup_stock")); }
    catch (...) {
      MonitoringQ2CompleteQ1PredecessorFailure rejected{bootstrap_error()};
      auto retired=historical_guard.retire();if(!retired)rejected.append(std::move(retired.error()));
      return std::unexpected{std::move(rejected)};
    }
    std::optional<CleanupStock> cleanup_stock;
    try { if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,0);
      cleanup_stock.emplace(make_cleanup_stock()); }
    catch (...) {
      MonitoringQ2CompleteQ1PredecessorFailure rejected{std::move(*stock_primary)};
      auto retired=historical_guard.retire();if(!retired)rejected.append(std::move(retired.error()));
      return std::unexpected{std::move(rejected)};
    }
    std::array<int,11> fds;fds.fill(-1);
    const auto close_failure=[&](bool retire_history=true) { CleanupLedger failures{.stock=&*cleanup_stock};
      for(auto it=fds.rbegin();it!=fds.rend();++it){if(*it>=0&&snapshot_operations.close_fd(snapshot_operations.context,std::exchange(*it,-1))!=0)failures.append_errno(errno);}
      if(retire_history&&historical_guard.active){auto retired=historical_guard.retire();if(!retired)failures.append(std::move(retired.error()));}
      return failures; };
    const auto reject=[&](core::Error primary,bool retire_history=true)
      -> std::expected<MonitoringQ2CompleteQ1PredecessorAuthority,MonitoringQ2CompleteQ1PredecessorFailure> {
      MonitoringQ2CompleteQ1PredecessorFailure result{std::move(primary)};auto cleanup=close_failure(retire_history);cleanup.transfer_to(result);
      return std::unexpected{std::move(result)};};
    try {
      if(!qid(binding.qualification_id)||!commit(binding.current_commit)) return reject(failure("identity"));
      if(binding.validated==nullptr) return reject(failure("validated_public_binding"));
      if(binding.validated->qualification_id()!=binding.qualification_id||
          binding.validated->profile_sha256()!=core::sha256(binding.profile_bytes).to_hex()||
          binding.validated->plan().qualification_id()!=binding.qualification_id||
          binding.validated->plan().profile_sha256().to_hex()!=binding.validated->profile_sha256())
        return reject(failure("validated_public_binding"));
      auto decoded_plan=MonitoringQ1PlanV2Value::decode(binding.q1_plan_bytes,binding.current_commit);
      if(!decoded_plan)return reject(failure("validated_plan_decode"));
      const auto& accepted_plan=binding.validated->plan();
      if(decoded_plan->qualification_id()!=accepted_plan.qualification_id()||
         decoded_plan->profile_sha256()!=accepted_plan.profile_sha256()||
         decoded_plan->qualification_plan_digest()!=accepted_plan.qualification_plan_digest()||
         decoded_plan->observations_per_worker()!=accepted_plan.observations_per_worker()||
         decoded_plan->capacity()!=accepted_plan.capacity()||
         decoded_plan->total_observations()!=accepted_plan.total_observations()||
         decoded_plan->execution_deadline_seconds()!=accepted_plan.execution_deadline_seconds()||
         decoded_plan->maximum_successor_tree_bytes()!=accepted_plan.maximum_successor_tree_bytes()||
         decoded_plan->derivation_mode()!=accepted_plan.derivation_mode()||
         decoded_plan->per_observation_allowance_ms()!=accepted_plan.per_observation_allowance_ms()||
         decoded_plan->fixed_reserve_seconds()!=accepted_plan.fixed_reserve_seconds()||
         decoded_plan->worker_ids().size()!=accepted_plan.worker_ids().size()||
         !std::ranges::equal(decoded_plan->worker_ids(),accepted_plan.worker_ids(),{},
            [](const auto& id){return id.value();},[](const auto& id){return id.value();}))
        return reject(failure("validated_plan_binding"));
      auto bound_profile=parse(binding.profile_bytes,1U<<20U);
      if(!bound_profile||!bound_profile->contains("q0b_provenance"))
        return reject(failure("validated_q0_binding"));
      const auto& provenance=(*bound_profile)["q0b_provenance"];
      const auto& accepted_q0=binding.validated->q0();
      if(provenance["repository_commit"]!=accepted_q0.repository_commit||
         provenance["build_identity"]!=accepted_q0.build_identity.to_hex()||
         provenance["host_environment"]["schema"]!=accepted_q0.environment_schema||
         provenance["host_environment"]["digest"]!=accepted_q0.environment_digest||
         provenance["host_environment"]["artifact_sha256"]!=accepted_q0.environment_artifact_sha256.to_hex()||
         provenance["host_qualification_plan"]["schema"]!=accepted_q0.qualification_plan_schema||
         provenance["host_qualification_plan"]["digest"]!=accepted_q0.qualification_plan_digest||
         provenance["characterization"]["schema"]!=accepted_q0.characterization_schema||
         provenance["characterization"]["artifact_sha256"]!=accepted_q0.characterization_artifact_sha256.to_hex()||
         provenance["q0b_production_inputs"]["schema"]!=accepted_q0.production_inputs_schema||
         provenance["q0b_production_inputs"]["sha256"]!=accepted_q0.production_inputs_sha256.to_hex()||
         provenance["q0b_base_environment"]["schema"]!=accepted_q0.base_environment_schema||
         provenance["q0b_base_environment"]["sha256"]!=accepted_q0.base_environment_sha256.to_hex()||
         provenance["initial_manifest_sha256"]!=accepted_q0.manifest_sha256.to_hex()||
         provenance["initial_checksums_sha256"]!=accepted_q0.checksums_sha256.to_hex())
        return reject(failure("validated_q0_binding"));
      if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,1);
      fds[0]=snapshot_operations.duplicate(snapshot_operations.context,root_descriptor);
      constexpr int directory_flags=O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW;
      if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,2);
      if(fds[0]<0||(fds[1]=snapshot_operations.open_at(snapshot_operations.context,fds[0],".local",directory_flags))<0){const int saved=errno;return reject(os_failure("snapshot_open",saved));}
      if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,3);
      if((fds[2]=snapshot_operations.open_at(snapshot_operations.context,fds[1],"monitoring-qualification",directory_flags))<0){const int saved=errno;return reject(os_failure("snapshot_open",saved));}
      if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,4);
      if((fds[3]=snapshot_operations.open_at(snapshot_operations.context,fds[2],std::string{binding.qualification_id}.c_str(),directory_flags))<0){const int saved=errno;return reject(os_failure("snapshot_open",saved));}
      if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,5);
      if((fds[4]=snapshot_operations.open_at(snapshot_operations.context,fds[3],"q0b",directory_flags))<0){const int saved=errno;return reject(os_failure("snapshot_open",saved));}
      if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,6);
      if((fds[5]=snapshot_operations.open_at(snapshot_operations.context,fds[3],"q1",directory_flags))<0){
        const int saved=errno;return reject(os_failure("snapshot_open",saved));}
      constexpr int file_flags=O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK;
      constexpr std::array file_parents{3U,4U,5U,3U,3U};
      constexpr std::array file_names{"host-environment.json","evidence.json","evidence.json","manifest.json","checksums.sha256"};
      for(std::size_t i{};i<5;++i){if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,static_cast<unsigned int>(7U+i));
        fds[6+i]=snapshot_operations.open_at(snapshot_operations.context,fds[file_parents[i]],file_names[i],file_flags);
        if(fds[6+i]<0){const int saved=errno;return reject(os_failure("snapshot_open",saved));}}
      const auto expected_root=std::set<std::string>{"checksums.sha256","host-environment.json","manifest.json","q0b","q1"};
      if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,12);
      auto root_names=snapshot_operations.inventory(snapshot_operations.context,fds[3]);
      if(!root_names){MonitoringQ2CompleteQ1PredecessorFailure result{std::move(root_names.error().primary)};
        if(root_names.error().cleanup)result.append(std::move(*root_names.error().cleanup));
        auto cleanup=close_failure(true);cleanup.transfer_to(result);return std::unexpected{std::move(result)};}
      if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,13);
      auto q0_names=snapshot_operations.inventory(snapshot_operations.context,fds[4]);
      if(!q0_names){MonitoringQ2CompleteQ1PredecessorFailure result{std::move(q0_names.error().primary)};
        if(q0_names.error().cleanup)result.append(std::move(*q0_names.error().cleanup));
        auto cleanup=close_failure(true);cleanup.transfer_to(result);return std::unexpected{std::move(result)};}
      if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,14);
      auto q1_names=snapshot_operations.inventory(snapshot_operations.context,fds[5]);
      if(!q1_names){MonitoringQ2CompleteQ1PredecessorFailure result{std::move(q1_names.error().primary)};
        if(q1_names.error().cleanup)result.append(std::move(*q1_names.error().cleanup));
        auto cleanup=close_failure(true);cleanup.transfer_to(result);return std::unexpected{std::move(result)};}
      if(*root_names!=expected_root||*q0_names!=std::set<std::string>{"evidence.json"}||*q1_names!=std::set<std::string>{"evidence.json"})
        return reject(failure("snapshot_inventory"));
      std::array<SnapshotIdentity,11> ids{};const auto euid=static_cast<std::uint32_t>(snapshot_operations.effective_user(snapshot_operations.context));
      { struct stat root_stat{};if(snapshot_operations.stat_fd(snapshot_operations.context,fds[0],&root_stat)!=0){const int saved=errno;return reject(os_failure("snapshot_stat",saved));}ids[0]=snapshot_identity(root_stat); }
      for(std::size_t i=1;i<fds.size();++i){if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,static_cast<unsigned int>(15U+i));struct stat s{};if(snapshot_operations.stat_fd(snapshot_operations.context,fds[i],&s)!=0){const int saved=errno;return reject(os_failure("snapshot_stat",saved));}
        const bool directory=i<=5;const auto mode=static_cast<std::uint32_t>(s.st_mode);
        if((directory?!S_ISDIR(s.st_mode):!S_ISREG(s.st_mode))||static_cast<std::uint32_t>(s.st_uid)!=euid||
           (!directory&&static_cast<std::uint32_t>(s.st_nlink)!=1U)||
           (mode&07777U)!=(directory?0700U:0600U))return reject(failure("snapshot_predicate"));
        ids[i]=snapshot_identity(s);}
      std::array<std::string,5> bytes;constexpr std::array<std::size_t,5> limits{1U<<20U,8U<<20U,8U<<20U,1U<<20U,1U<<20U};
      for(std::size_t i=0;i<bytes.size();++i){if(snapshot_operations.fault)snapshot_operations.fault(snapshot_operations.context,static_cast<unsigned int>(26U+i));auto read=read_fd(fds[i+6],limits[i],snapshot_operations);if(!read)return reject(std::move(read.error()));bytes[i]=std::move(*read);}
      MonitoringQ2CompleteQ1PredecessorInput input{std::string{binding.qualification_id},std::string{binding.current_commit},
        std::string{binding.profile_bytes},std::string{binding.q1_plan_bytes},bytes[0],bytes[1],bytes[2],bytes[3],bytes[4]};
      auto result=authenticate(std::move(input),binding.historical,*cleanup_stock);if(!result){auto cleanup=close_failure(true);cleanup.transfer_to(result.error());return result;}
      historical_guard.release();
      result->descriptors_=std::exchange(fds,std::array<int,11>{});fds.fill(-1);result->identities_=ids;result->retained_bytes_=std::move(bytes);
      result->snapshot_operations_=snapshot_operations;
      auto final=result->recheck();
      if(!final||!*final){MonitoringQ2CompleteQ1PredecessorFailure rejected=
          final?MonitoringQ2CompleteQ1PredecessorFailure{failure("snapshot_changed")}:std::move(final.error());
        auto retired=result->retire();if(!retired)rejected.append(std::move(retired.error()));
        return std::unexpected{std::move(rejected)};}
      return result;
    } catch(...) { return reject(bootstrap_error()); }
}

auto MonitoringQ2CompleteQ1PredecessorAuthority::recheck() const
    -> std::expected<bool,MonitoringQ2CompleteQ1PredecessorFailure> {
    if(!active_)return std::unexpected{MonitoringQ2CompleteQ1PredecessorFailure{bootstrap_error()}};
    try {
    for(std::size_t i=0;i<descriptors_.size();++i){struct stat s{};if(snapshot_operations_.stat_fd(snapshot_operations_.context,descriptors_[i],&s)!=0)return std::unexpected{os_failure("snapshot_recheck",errno)};
      if(snapshot_identity(s)!=identities_[i])return std::unexpected{failure("snapshot_held_changed")};}
    const std::array<std::size_t,10> parents{0,1,2,3,3,3,4,5,3,3};
    const std::array<const char*,10> names{".local","monitoring-qualification",nullptr,"q0b","q1",
      "host-environment.json","evidence.json","evidence.json","manifest.json","checksums.sha256"};
    const auto& qid_name=qualification_id_;
    for(std::size_t offset=0;offset<parents.size();++offset){struct stat s{};const char* name=names[offset];if(offset==2)name=qid_name.c_str();
      if(snapshot_operations_.stat_at(snapshot_operations_.context,descriptors_[parents[offset]],name,&s,AT_SYMLINK_NOFOLLOW)!=0)return std::unexpected{os_failure("snapshot_named_recheck",errno)};
      if(snapshot_identity(s)!=identities_[offset+1])return std::unexpected{failure("snapshot_named_changed")};}
    auto root_names=snapshot_operations_.inventory(snapshot_operations_.context,descriptors_[3]);
    if(!root_names){MonitoringQ2CompleteQ1PredecessorFailure result{std::move(root_names.error().primary)};if(root_names.error().cleanup)result.append(std::move(*root_names.error().cleanup));return std::unexpected{std::move(result)};}
    auto q0_names=snapshot_operations_.inventory(snapshot_operations_.context,descriptors_[4]);
    if(!q0_names){MonitoringQ2CompleteQ1PredecessorFailure result{std::move(q0_names.error().primary)};if(q0_names.error().cleanup)result.append(std::move(*q0_names.error().cleanup));return std::unexpected{std::move(result)};}
    auto q1_names=snapshot_operations_.inventory(snapshot_operations_.context,descriptors_[5]);
    if(!q1_names){MonitoringQ2CompleteQ1PredecessorFailure result{std::move(q1_names.error().primary)};if(q1_names.error().cleanup)result.append(std::move(*q1_names.error().cleanup));return std::unexpected{std::move(result)};}
    if(*root_names!=std::set<std::string>{"checksums.sha256","host-environment.json","manifest.json","q0b","q1"}||
       *q0_names!=std::set<std::string>{"evidence.json"}||*q1_names!=std::set<std::string>{"evidence.json"})return std::unexpected{failure("snapshot_inventory_changed")};
    constexpr std::array<std::size_t,5> limits{1U<<20U,8U<<20U,8U<<20U,1U<<20U,1U<<20U};
    for(std::size_t i=0;i<retained_bytes_.size();++i){auto bytes=read_fd(descriptors_[i+6],limits[i],snapshot_operations_);if(!bytes)return std::unexpected{std::move(bytes.error())};if(*bytes!=retained_bytes_[i])return std::unexpected{failure("snapshot_bytes_changed")};}
    if(!operations_.authenticate_historical)return std::unexpected{failure("historical_operations")};
    auto history=operations_.authenticate_historical(operations_.context,commit_,current_commit_,q0_commit_,profile_bytes_,q1_plan_bytes_);
    if(!history){MonitoringQ2CompleteQ1PredecessorFailure converted{std::move(history.error().primary)};
      if(history.error().cleanup_count>history.error().cleanup.size())return std::unexpected{failure("historical_failure_invariant")};
      for(std::size_t index{};index<history.error().cleanup_count;++index){if(!history.error().cleanup[index])return std::unexpected{failure("historical_failure_invariant")};converted.append(std::move(*history.error().cleanup[index]));}
      return std::unexpected{std::move(converted)};}
    return true;
    } catch (...) { return std::unexpected{MonitoringQ2CompleteQ1PredecessorFailure{bootstrap_error()}}; }
}
} // namespace epycbench::monitoring::qualification::detail
