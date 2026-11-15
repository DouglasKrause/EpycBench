#pragma once
#include "monitoring/host_q3_plan_value.hpp"
#include "epycbench/core/error.hpp"
#include <array>
#include <expected>

namespace epycbench::monitoring::qualification::detail {
enum class MonitoringQ3TemperatureDisposition:unsigned char{pass,unsupported};
enum class MonitoringQ3CollectionStage:unsigned char{source_discovery,oracle,source_read,agreement,clock,deadline,capacity,finalization};
struct MonitoringQ3TemperatureInterval final{std::uint64_t start{},end{};};
struct MonitoringQ3TemperatureCollection final {
 MonitoringQ3TemperatureDisposition disposition{};std::size_t discoveries{},rediscoveries{},agreements{},typed_reads{};
 std::array<std::int64_t,48> agreed_values{},typed_values{};
 std::array<MonitoringQ3TemperatureInterval,3> discovery{};
 std::array<MonitoringQ3TemperatureInterval,48> raw{},typed{};
};
struct MonitoringQ3TemperatureCollectionFailure final{MonitoringQ3CollectionStage stage{MonitoringQ3CollectionStage::finalization};core::Error primary;
 std::array<std::optional<core::Error>,32> cleanup{};std::size_t cleanup_count{};bool cleanup_overflowed{};};
class MonitoringQ3TemperatureCollector final{
 public:
  struct Operations final{void* context{};
   std::expected<std::uint64_t,core::Error>(*now)(void*) noexcept{};
   std::expected<bool,core::Error>(*discover)(void*,std::size_t){};
   std::expected<std::int64_t,core::Error>(*raw)(void*,std::size_t,std::size_t){};
   std::expected<std::int64_t,core::Error>(*typed)(void*,std::size_t,std::size_t){};
   std::expected<bool,core::Error>(*identity)(void*,std::size_t){};
   std::expected<void,core::Error>(*retire)(void*,std::size_t) noexcept{};};
  [[nodiscard]] static auto collect(const MonitoringQ3PlanValue&,Operations) noexcept
   ->std::expected<MonitoringQ3TemperatureCollection,MonitoringQ3TemperatureCollectionFailure>;
};
}
