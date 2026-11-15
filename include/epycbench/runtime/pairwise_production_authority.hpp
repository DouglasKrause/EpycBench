#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"
#include "epycbench/runtime/ipc/payloads.hpp"
#include "epycbench/runtime/production_admission.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>

namespace epycbench::runtime {

class ControllerProcessProductionAuthority;

namespace detail {
struct ProductionAuthorityTestAccess;
struct PairwiseProductionAdmissionAccess;
struct ProductionRequestIdAccess;
}

class ProductionRequestIdSource final {
  public:
    ProductionRequestIdSource(const ProductionRequestIdSource&) = delete;
    auto operator=(const ProductionRequestIdSource&) -> ProductionRequestIdSource& = delete;
    ProductionRequestIdSource(ProductionRequestIdSource&&) = delete;
    auto operator=(ProductionRequestIdSource&&) -> ProductionRequestIdSource& = delete;

    [[nodiscard]] auto allocate() noexcept -> std::expected<ipc::RequestId, core::Error>;

  private:
    ProductionRequestIdSource() noexcept = default;
    [[nodiscard]] auto allocate_fallible() -> std::expected<ipc::RequestId, core::Error>;

    std::mutex mutex_;
    std::uint64_t next_{1};
    bool exhausted_{false};

    friend class ControllerProcessProductionAuthority;
    friend struct detail::ProductionRequestIdAccess;
    friend struct detail::ProductionAuthorityTestAccess;
};

class PairwiseProductionAdmission final {
  public:
    PairwiseProductionAdmission(const PairwiseProductionAdmission&) = delete;
    auto operator=(const PairwiseProductionAdmission&) -> PairwiseProductionAdmission& = delete;
    PairwiseProductionAdmission(PairwiseProductionAdmission&&) noexcept;
    auto operator=(PairwiseProductionAdmission&&) -> PairwiseProductionAdmission& = delete;
    ~PairwiseProductionAdmission();

    [[nodiscard]] auto first_request() const noexcept -> const ipc::PlacementRequest&;
    [[nodiscard]] auto second_request() const noexcept -> const ipc::PlacementRequest&;
    [[nodiscard]] auto inputs() const noexcept -> const AdmittedPairwiseProductionInputs&;
    [[nodiscard]] auto has_value() const noexcept -> bool;

  private:
    struct State;
    explicit PairwiseProductionAdmission(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend struct detail::PairwiseProductionAdmissionAccess;
};

} // namespace epycbench::runtime
