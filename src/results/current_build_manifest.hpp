#pragma once

#include "results/build_manifest_owner.hpp"

namespace epycbench::results::build_manifest {
// Retains this image's generated build fields through the supplied owner.
// No acquisition, image authentication or kernel qualification is performed.
[[nodiscard]] auto retain_current(Limits) -> std::expected<Owner, core::Error>;
} // namespace epycbench::results::build_manifest
