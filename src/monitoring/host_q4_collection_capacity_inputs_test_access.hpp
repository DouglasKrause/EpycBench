#pragma once
#include "monitoring/host_q4_collection_capacity_inputs.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CollectionCapacityInputsTestAccess final {
  public:
    enum class Fault : unsigned char { none, order, reserve, push };
    static auto allocate(const MonitoringQ4CollectionCapacityInputs& inputs,
                         std::span<const MonitoringQ4MultiplicityCategory> categories,
                         const std::array<std::uint64_t, 10>& buckets,
                         Fault fault = Fault::none) noexcept {
        return MonitoringQ4CollectionCapacityInputs::allocate_multiplicity_categories_with(
            categories, buckets, inputs.structural_.admitted_cpu_count().value,
            static_cast<MonitoringQ4CollectionCapacityInputs::AllocationFault>(fault));
    }
};
} // namespace epycbench::monitoring::qualification::detail
