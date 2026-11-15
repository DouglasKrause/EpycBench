#pragma once

#include "epycbench/monitoring/frequency.hpp"

namespace epycbench::monitoring::detail {
class FrequencySourceAccess {
  public:
    [[nodiscard]] static auto create(FrequencyPolicyMetadata metadata, void* context,
                                     FrequencySource::ReadOperation read) -> FrequencySource {
        return FrequencySource{std::move(metadata), context, read, nullptr};
    }
    [[nodiscard]] static auto read_value(FrequencySource& source) noexcept
        -> std::expected<std::uint64_t, core::Error> {
        return source.read_(source.context_);
    }
};
} // namespace epycbench::monitoring::detail
