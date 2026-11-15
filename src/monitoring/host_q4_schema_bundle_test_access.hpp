#pragma once
#include <cstddef>
#include <optional>
#include <string_view>
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4ResolvedSchemaBundle;
enum class MonitoringQ4SchemaKind : unsigned char;
class MonitoringQ4SchemaBundleTestAccess final {
  public:
    enum class Fault : unsigned char {
        none,
        parse_allocation,
        resolve_allocation,
        encode_allocation,
        contained
    };
    static void set_fault(Fault) noexcept;
    [[nodiscard]] static auto resolved_bytes(const MonitoringQ4ResolvedSchemaBundle&,
                                             MonitoringQ4SchemaKind) noexcept
        -> std::optional<std::string_view>;
    [[nodiscard]] static auto visited_nodes(const MonitoringQ4ResolvedSchemaBundle&,
                                            MonitoringQ4SchemaKind) noexcept
        -> std::optional<std::size_t>;
};
} // namespace epycbench::monitoring::qualification::detail
