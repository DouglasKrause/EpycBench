#pragma once
#include "monitoring/host_q3_temperature_parser_agreement.hpp"
#include "epycbench/platform_linux/hwmon_temperature.hpp"
#include "epycbench/monitoring/temperature.hpp"
#include "platform_linux/hwmon_temperature_access.hpp"
#include <sys/stat.h>
#include <memory>

namespace epycbench::monitoring::qualification::detail {
struct MonitoringQ3TemperatureCycleFailure final {
 MonitoringQ3TemperatureCycleFailure()=default;
 MonitoringQ3TemperatureCycleFailure(core::Error value,std::array<std::optional<core::Error>,4> retired,
  std::size_t count,bool overflow):primary(std::move(value)),cleanup(std::move(retired)),cleanup_count(count),cleanup_overflowed(overflow){}
 MonitoringQ3TemperatureCycleFailure(core::Error value,std::array<std::optional<core::Error>,4> retired,
  std::size_t count,bool overflow,std::optional<MonitoringQ3ParserAgreementStage> stage):primary(std::move(value)),cleanup(std::move(retired)),cleanup_count(count),cleanup_overflowed(overflow),raw_stage(stage){}
 core::Error primary; std::array<std::optional<core::Error>,4> cleanup{};
 std::size_t cleanup_count{}; bool cleanup_overflowed{};
 std::optional<MonitoringQ3ParserAgreementStage> raw_stage;
};
class MonitoringQ3TemperatureCycleAuthority final {
 public:
  struct Operations final {
   void* context{}; int (*open)(void*,const char*,int) noexcept{};
   int (*stat_fd)(void*,int,struct stat*) noexcept{};
   int (*stat_named)(void*,const char*,struct stat*) noexcept{};
   ssize_t (*read)(void*,int,void*,std::size_t,std::int64_t) noexcept{};
   int (*close)(void*,int) noexcept{};
   void (*fault)(void*,int){};
   std::expected<std::int64_t,core::Error> (*production_parser)(void*,std::string_view){};
   std::expected<std::int64_t,core::Error> (*oracle_parser)(void*,std::string_view){};
  };
  MonitoringQ3TemperatureCycleAuthority(const MonitoringQ3TemperatureCycleAuthority&)=delete;
  MonitoringQ3TemperatureCycleAuthority(MonitoringQ3TemperatureCycleAuthority&&) noexcept;
  ~MonitoringQ3TemperatureCycleAuthority() noexcept;
  [[nodiscard]] static auto retain(platform_linux::HwmonTemperatureSource,Operations) noexcept
   ->std::expected<MonitoringQ3TemperatureCycleAuthority,MonitoringQ3TemperatureCycleFailure>;
  [[nodiscard]] static auto discover() noexcept
   ->std::expected<std::optional<MonitoringQ3TemperatureCycleAuthority>,MonitoringQ3TemperatureCycleFailure>;
  [[nodiscard]] auto acquire_raw() noexcept
   ->std::expected<MonitoringQ3TemperatureParserAgreement,MonitoringQ3TemperatureCycleFailure>;
  [[nodiscard]] auto recheck() noexcept->std::expected<bool,MonitoringQ3TemperatureCycleFailure>;
  [[nodiscard]] auto retire() noexcept->std::expected<void,MonitoringQ3TemperatureCycleFailure>;
 private:
  struct Diagnostics;
  MonitoringQ3TemperatureCycleAuthority(platform_linux::HwmonTemperatureSource,std::string,int,
                                         struct stat,Operations,std::unique_ptr<Diagnostics>) noexcept;
  platform_linux::HwmonTemperatureSource source_; std::string input_; int fd_{-1};
  struct stat identity_{}; Operations operations_{};
  std::unique_ptr<Diagnostics> diagnostics_;
  bool terminal_{};
  [[nodiscard]] auto typed_source_for_adapter() noexcept -> ::epycbench::monitoring::TemperatureSource;
  static auto typed_read(void*) noexcept -> std::expected<std::int64_t,core::Error>;
  friend class MonitoringQ3TemperatureProductionAdapter;
};
}
