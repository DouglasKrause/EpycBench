#include "monitoring/host_q3_temperature_cycle_authority.hpp"
#include "monitoring/temperature_source_access.hpp"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail { namespace {
auto error(std::string classification,int os=0)->core::Error{
 return {.category=core::ErrorCategory::monitoring,
  .code=core::StableErrorCode{"monitoring.q3.temperature.cycle_authority_failed"},
  .message="Monitoring Q3 temperature cycle authority failed",
  .operation="monitoring_q3_temperature_cycle_authority",
  .os_error=os?std::optional{core::OsError{os,"errno","operation failed"}}:std::nullopt,
  .context={{"classification",std::move(classification)}},.severity=core::ErrorSeverity::error,
  .continuation=core::ContinuationPolicy::abort_command}; }
auto same(const struct stat&a,const struct stat&b) noexcept->bool{
 return a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_uid==b.st_uid&&a.st_mode==b.st_mode&&
  a.st_nlink==b.st_nlink;
}
auto failed(core::Error primary,std::optional<MonitoringQ3ParserAgreementStage> stage={})->MonitoringQ3TemperatureCycleFailure{return {std::move(primary),{},0U,false,stage};}
void append(MonitoringQ3TemperatureCycleFailure& value,core::Error cleanup) noexcept{
 if(value.cleanup_count<value.cleanup.size())value.cleanup[value.cleanup_count++].emplace(std::move(cleanup));
 else value.cleanup_overflowed=true;
}
auto native_open(void*,const char* path,int flags) noexcept->int{return ::open(path,flags);}
auto native_stat_fd(void*,int fd,struct stat* value) noexcept->int{return ::fstat(fd,value);}
auto native_stat_named(void*,const char* path,struct stat* value) noexcept->int{return ::fstatat(AT_FDCWD,path,value,AT_SYMLINK_NOFOLLOW);}
auto native_read(void*,int fd,void* bytes,std::size_t size,std::int64_t offset) noexcept->ssize_t{return ::pread(fd,bytes,size,offset);}
auto native_close(void*,int fd) noexcept->int{return ::close(fd);}
}
struct MonitoringQ3TemperatureCycleAuthority::Diagnostics final {
 core::Error held,named,recheck_contained,before_identity,allocation,read,read_contract,
  raw_byte_cap,after_identity,read_contained,close_explicit,terminal;
};
MonitoringQ3TemperatureCycleAuthority::MonitoringQ3TemperatureCycleAuthority(
 platform_linux::HwmonTemperatureSource source,std::string input,int fd,struct stat identity,
 Operations operations,std::unique_ptr<Diagnostics> diagnostics) noexcept:source_{std::move(source)},
 input_{std::move(input)},fd_{fd},identity_{identity},operations_{operations},
 diagnostics_{std::move(diagnostics)}{}
MonitoringQ3TemperatureCycleAuthority::MonitoringQ3TemperatureCycleAuthority(
 MonitoringQ3TemperatureCycleAuthority&& other) noexcept:source_{std::move(other.source_)},
 input_{std::move(other.input_)},fd_{std::exchange(other.fd_,-1)},identity_{other.identity_},
 operations_{other.operations_},diagnostics_{std::move(other.diagnostics_)},
 terminal_{std::exchange(other.terminal_,true)}{}
MonitoringQ3TemperatureCycleAuthority::~MonitoringQ3TemperatureCycleAuthority() noexcept{
 if(fd_>=0) static_cast<void>(operations_.close(operations_.context,fd_)); }
auto MonitoringQ3TemperatureCycleAuthority::retain(platform_linux::HwmonTemperatureSource source,
 Operations operations) noexcept->std::expected<MonitoringQ3TemperatureCycleAuthority,MonitoringQ3TemperatureCycleFailure>{
 struct Guard final {Operations* operations{};int fd{-1};bool active{};
  ~Guard() noexcept{if(active){active=false;static_cast<void>(operations->close(operations->context,fd));}}
  auto close_once() noexcept->std::pair<int,int>{active=false;const int result=operations->close(operations->context,fd);return {result,errno};}
  auto take() noexcept->int{active=false;return fd;}
 };
 try { std::string input{platform_linux::detail::HwmonTemperatureAccess::input(source)};
  auto diagnostics=std::make_unique<Diagnostics>(Diagnostics{error("held_recheck",1),
   error("named_recheck",1),error("recheck_contained"),error("before_identity"),
   error("allocation"),error("read",1),error("read_contract"),error("raw_byte_cap"),
   error("after_identity"),error("read_contained"),error("close",1),error("terminal")});
  struct Templates final {core::Error held,named,identity,contained,close_rejection,close_containment;};
  Templates templates{error("held_stat",1),error("named_stat",1),error("identity"),
   error("contained"),error("close_after_rejection",1),error("close_after_containment",1)};
  const int fd=operations.open(operations.context,input.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
  if(fd<0)return std::unexpected{failed(error("open",errno))};
  Guard guard{&operations,fd,true};
  const auto reject=[&](core::Error primary,int os){
   if(primary.os_error) primary.os_error->value=os;
   auto result=failed(std::move(primary));
   const auto [closed,close_os]=guard.close_once();
   if(closed!=0){templates.close_rejection.os_error->value=close_os;
    append(result,std::move(templates.close_rejection));}
   return std::expected<MonitoringQ3TemperatureCycleAuthority,MonitoringQ3TemperatureCycleFailure>{std::unexpected{std::move(result)}};};
  try {if(operations.fault)operations.fault(operations.context,1);
   struct stat held{},named{};
   if(operations.stat_fd(operations.context,fd,&held)!=0)return reject(std::move(templates.held),errno);
   if(operations.stat_named(operations.context,input.c_str(),&named)!=0)return reject(std::move(templates.named),errno);
   if(!S_ISREG(held.st_mode)||held.st_nlink!=1||!same(held,named))return reject(std::move(templates.identity),0);
   if(operations.fault)operations.fault(operations.context,2);
   MonitoringQ3TemperatureCycleAuthority result{std::move(source),std::move(input),guard.take(),held,
                                                 operations,std::move(diagnostics)};
   return result;
  }catch(...){auto result=failed(std::move(templates.contained));const auto [closed,close_os]=guard.close_once();
   if(closed!=0){templates.close_containment.os_error->value=close_os;append(result,std::move(templates.close_containment));}
   return std::unexpected{std::move(result)};}
 }catch(...){return std::unexpected{failed(error("preownership_contained"))};}}
auto MonitoringQ3TemperatureCycleAuthority::discover() noexcept
 ->std::expected<std::optional<MonitoringQ3TemperatureCycleAuthority>,MonitoringQ3TemperatureCycleFailure>{
 try {auto source=platform_linux::HwmonTemperatureSource::discover();
  if(!source)return std::unexpected{failed(std::move(source.error()))};
  if(!*source)return std::optional<MonitoringQ3TemperatureCycleAuthority>{};
  auto retained=retain(std::move(**source),{nullptr,native_open,native_stat_fd,native_stat_named,native_read,native_close,nullptr});
  if(!retained)return std::unexpected{std::move(retained.error())};
  return std::optional<MonitoringQ3TemperatureCycleAuthority>{std::move(*retained)};
 }catch(...){return std::unexpected{failed(error("discovery_contained"))};}}
auto MonitoringQ3TemperatureCycleAuthority::recheck() noexcept->std::expected<bool,MonitoringQ3TemperatureCycleFailure>{
 const auto reject=[&](core::Error value,int os=0){terminal_=true;if(value.os_error)value.os_error->value=os;
  return std::expected<bool,MonitoringQ3TemperatureCycleFailure>{std::unexpected{failed(std::move(value))}};};
 if(terminal_) return reject(std::move(diagnostics_->terminal));
 try {if(operations_.fault)operations_.fault(operations_.context,5);struct stat held{},named{};if(operations_.stat_fd(operations_.context,fd_,&held)!=0)
  return reject(std::move(diagnostics_->held),errno);
 if(operations_.stat_named(operations_.context,input_.c_str(),&named)!=0)
  return reject(std::move(diagnostics_->named),errno);
 return same(identity_,held)&&same(identity_,named)&&S_ISREG(held.st_mode)&&held.st_nlink==1;
 }catch(...){return reject(std::move(diagnostics_->recheck_contained));}}
auto MonitoringQ3TemperatureCycleAuthority::acquire_raw() noexcept
 ->std::expected<MonitoringQ3TemperatureParserAgreement,MonitoringQ3TemperatureCycleFailure>{
 const auto reject=[&](core::Error value,int os=0){terminal_=true;if(value.os_error)value.os_error->value=os;
  return std::expected<MonitoringQ3TemperatureParserAgreement,MonitoringQ3TemperatureCycleFailure>{
   std::unexpected{failed(std::move(value),MonitoringQ3ParserAgreementStage::oracle)}};};
 try {
 auto before=recheck();if(!before||!*before){
  if(before)return reject(std::move(diagnostics_->before_identity));
  auto failure=std::move(before.error());failure.raw_stage=MonitoringQ3ParserAgreementStage::oracle;
  return std::unexpected{std::move(failure)};
 }
 if(operations_.fault)operations_.fault(operations_.context,3);
 std::string bytes;try{bytes.resize(129U);}catch(...){return reject(std::move(diagnostics_->allocation));}
 std::size_t used{};for(;;){const auto got=operations_.read(operations_.context,fd_,bytes.data()+used,bytes.size()-used,static_cast<std::int64_t>(used));
  if(got<0){if(errno==EINTR)continue;return reject(std::move(diagnostics_->read),errno);}if(got==0)break;
  if(static_cast<std::size_t>(got)>bytes.size()-used)return reject(std::move(diagnostics_->read_contract));
  used+=static_cast<std::size_t>(got);if(used>128U)return reject(std::move(diagnostics_->raw_byte_cap));}
 bytes.resize(used);auto after=recheck();if(!after||!*after){
  if(after)return reject(std::move(diagnostics_->after_identity));
  auto failure=std::move(after.error());failure.raw_stage=MonitoringQ3ParserAgreementStage::oracle;
  return std::unexpected{std::move(failure)};
 }
 if(operations_.fault)operations_.fault(operations_.context,4);
 auto agreed=(operations_.production_parser&&operations_.oracle_parser)
  ? MonitoringQ3TemperatureParserAgreement::authenticate_staged_with(std::move(bytes),
      {operations_.context,operations_.production_parser,operations_.oracle_parser})
  : MonitoringQ3TemperatureParserAgreement::authenticate_staged(std::move(bytes));
 if(!agreed){terminal_=true;return std::unexpected{failed(std::move(agreed.error().primary),agreed.error().stage)};}
 return std::move(*agreed);
 }catch(...){return reject(std::move(diagnostics_->read_contained));}
}
auto MonitoringQ3TemperatureCycleAuthority::retire() noexcept->std::expected<void,MonitoringQ3TemperatureCycleFailure>{
 if(fd_<0) return {};
 const int held=std::exchange(fd_,-1);
 if(operations_.close(operations_.context,held)!=0){diagnostics_->close_explicit.os_error->value=errno;
  return std::unexpected{failed(std::move(diagnostics_->close_explicit))};}
 return {};
}
auto MonitoringQ3TemperatureCycleAuthority::typed_read(void* raw) noexcept
 ->std::expected<std::int64_t,core::Error>{return static_cast<MonitoringQ3TemperatureCycleAuthority*>(raw)->source_.read();}
auto MonitoringQ3TemperatureCycleAuthority::typed_source_for_adapter() noexcept
 ->::epycbench::monitoring::TemperatureSource{return ::epycbench::monitoring::detail::TemperatureSourceAccess::create(this,typed_read);}
}
