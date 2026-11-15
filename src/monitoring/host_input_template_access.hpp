#pragma once

#include "epycbench/monitoring/host_input_template.hpp"

#include <expected>
#include <optional>
#include <string_view>

namespace epycbench::monitoring::qualification {

class detail_MonitoringHostInputTemplateAccess {
  public:
    // The source caller owns the prepared native reader, its actual token and
    // absolute fence for this entire synchronous borrow. The reader returns
    // genuine governed files; this route never falls back to the public
    // allocating-noexcept filesystem loader. Each fixed file is checked by
    // the same digest/shape/binding algorithm used by the public wrapper.
    struct SourceOperations final {
        void* context{};
        std::expected<core::GovernedRepositoryFile, core::Error> (*read)(
            void*, std::string_view){};
        std::expected<void, core::Error> (*guard)(void*){};
        // Private deterministic projection checkpoint. Production leaves this
        // null; the actual lower cause is already owned before it is called.
        void (*before_projection)(void*){};
    };
    [[nodiscard]] static auto load_source_fallible(
        SourceOperations, std::optional<core::Error>& raw_failure)
        -> std::expected<MonitoringHostInputTemplateAuthority, core::Error>;
    [[nodiscard]] static auto validate_template_json_fallible(
        std::string_view bytes, std::optional<core::Error>* raw_failure = nullptr,
        void* context = nullptr, void (*before_projection)(void*) = nullptr)
        -> std::expected<void, core::Error>;
    [[nodiscard]] static auto validate_template_json(std::string_view bytes) noexcept
        -> std::expected<void, core::Error>;
};

} // namespace epycbench::monitoring::qualification
