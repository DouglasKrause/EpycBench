#pragma once
#include "epycbench/monitoring/temperature.hpp"
namespace epycbench::monitoring::detail {
class TemperatureSourceAccess { public:
 static auto create(void* context, TemperatureSource::ReadOperation read) -> TemperatureSource {
   return TemperatureSource{context, read, nullptr};
 }
};
}
