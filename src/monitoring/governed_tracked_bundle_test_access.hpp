#pragma once
#include "monitoring/governed_tracked_bundle.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringGovernedTrackedBundleTestAccess final {
  public:
    using Operations = MonitoringGovernedTrackedBundleCore::Operations;
    static auto retain(int root, MonitoringCurrentRevisionTrackedCapture&& capture,
                       Operations operations) {
        return MonitoringGovernedTrackedBundleCore::retain(root, std::move(capture), operations);
    }
    static auto bytes(const MonitoringGovernedTrackedBundleCore& value, std::size_t slot) {
        return std::string_view{value.bytes_.at(slot)};
    }
    static auto digest(const MonitoringGovernedTrackedBundleCore& value, std::size_t slot) {
        return value.digests_.at(slot);
    }
    static auto identity(const MonitoringGovernedTrackedBundleCore& value, std::size_t role) {
        return value.identities_.at(role);
    }
    static auto head(const MonitoringGovernedTrackedBundleCore& value) {
        return std::string_view{value.head_};
    }
    static auto active(const MonitoringGovernedTrackedBundleCore& value) { return value.active_; }
    static auto confirmed(const MonitoringGovernedTrackedBundleCore& value) {
        return value.confirmed_;
    }
    static auto uncertain(const MonitoringGovernedTrackedBundleCore& value) {
        return value.uncertain_;
    }
    static auto make_retirement_only(std::array<int, 22> descriptors, Operations operations) {
        std::vector<core::Sha256Digest> digests;
        digests.reserve(18);
        for (std::size_t slot{}; slot < 18U; ++slot)
            digests.push_back(core::sha256(""));
        return MonitoringGovernedTrackedBundleCore{
            descriptors, {}, {}, std::move(digests), {}, {}, operations, {}, {}};
    }
};
} // namespace epycbench::monitoring::qualification::detail
