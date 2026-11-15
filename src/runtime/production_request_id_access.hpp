#pragma once

#include "epycbench/runtime/pairwise_production_authority.hpp"

namespace epycbench::runtime::detail {

// Borrows the genuine process source. No source construction or reset authority.
struct ProductionRequestIdAccess final {
    [[nodiscard]] static auto allocate(ProductionRequestIdSource& source)
        -> std::expected<ipc::RequestId, core::Error> {
        return source.allocate_fallible();
    }
};

} // namespace epycbench::runtime::detail
