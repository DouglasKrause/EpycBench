#pragma once
#include "monitoring/host_q4_governed_input_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4GovernedInputAuthorityTestAccess final {
  public:
    using Operations = MonitoringQ4GovernedInputAuthority::Operations;
    static auto authenticate(int root, MonitoringCurrentRevisionTrackedCapture&& capture,
                             Operations operations) {
        return MonitoringQ4GovernedInputAuthority::authenticate_with(
            root, std::move(capture), operations);
    }
    static auto active(const MonitoringQ4GovernedInputAuthority& value) noexcept {
        return value.core_.has_value();
    }
    static auto prepare(MonitoringQ4GovernedInputAuthority& value) {
        return value.prepare_candidate();
    }
    static auto finalize(MonitoringQ4GovernedInputAuthority& value,
                         MonitoringQ4GovernedInputCandidate&& candidate) {
        return value.finalize(std::move(candidate));
    }
    static auto candidate_valid(const MonitoringQ4GovernedInputCandidate& value) noexcept {
        return value.valid_;
    }
    static void replace_candidate_head(MonitoringQ4GovernedInputCandidate& value,
                                       std::string head) {
        value.head_ = std::move(head);
    }
    static auto make_candidate(std::array<std::string, 18> bytes,
                               std::string head,
                               std::shared_ptr<const void> binding) {
        MonitoringQ4GovernedArtifactSlots slots;
        for (std::size_t index{}; index < bytes.size(); ++index)
            slots[index].emplace(MonitoringQ4GovernedArtifact{
                bytes[index], core::sha256(bytes[index])});
        return MonitoringQ4GovernedInputCandidate{
            std::move(slots), std::move(head), std::move(binding)};
    }
    static void replace_candidate_artifact(MonitoringQ4GovernedInputCandidate& value,
                                           std::size_t slot, std::string bytes,
                                           bool recompute_digest) {
        value.records_.at(slot)->bytes = std::move(bytes);
        if (recompute_digest)
            value.records_.at(slot)->sha256 = core::sha256(value.records_.at(slot)->bytes);
    }
    static auto record_count(const MonitoringQ4GovernedInputValue& value) noexcept {
        std::size_t count{};
        for (const auto& record : value.records_)
            count += record.has_value() ? 1U : 0U;
        return count;
    }
};
} // namespace epycbench::monitoring::qualification::detail
