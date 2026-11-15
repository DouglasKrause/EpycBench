#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringQ4SchemaBundleFailureKind : unsigned char {
    association,
    byte_limit,
    canonical,
    identity,
    reference,
    cycle,
    depth_limit,
    node_limit,
    resolved_byte_limit,
    preflight,
    allocation,
    contained
};

enum class MonitoringQ4SchemaKind : unsigned char {
    plan,
    evidence,
    backend_result,
    backend_error,
    backend_authored
};

struct MonitoringQ4SchemaBundleFailure final {
    MonitoringQ4SchemaBundleFailureKind kind{};
    std::size_t path_index{};
    std::size_t visited_nodes{};
};

struct MonitoringQ4SchemaInput final {
    std::string_view filename;
    std::string_view bytes;
};

class MonitoringQ4ResolvedSchemaBundle final {
  public:
    static constexpr std::size_t schema_count = 5;
    MonitoringQ4ResolvedSchemaBundle(const MonitoringQ4ResolvedSchemaBundle&) = default;
    MonitoringQ4ResolvedSchemaBundle(MonitoringQ4ResolvedSchemaBundle&&) noexcept = default;
    auto operator=(const MonitoringQ4ResolvedSchemaBundle&)
        -> MonitoringQ4ResolvedSchemaBundle& = delete;
    auto operator=(MonitoringQ4ResolvedSchemaBundle&&)
        -> MonitoringQ4ResolvedSchemaBundle& = delete;
    [[nodiscard]] auto plan_schema() const noexcept -> std::string_view { return resolved_[0]; }
    [[nodiscard]] auto evidence_schema() const noexcept -> std::string_view { return resolved_[1]; }
    [[nodiscard]] auto backend_result_schema() const noexcept -> std::string_view {
        return resolved_[2];
    }
    [[nodiscard]] auto backend_error_schema() const noexcept -> std::string_view {
        return resolved_[3];
    }
    [[nodiscard]] auto backend_authored_schema() const noexcept -> std::string_view {
        return resolved_[4];
    }

  private:
    friend auto
    resolve_monitoring_q4_schema_bundle(const std::array<MonitoringQ4SchemaInput, schema_count>&)
        -> std::expected<MonitoringQ4ResolvedSchemaBundle, MonitoringQ4SchemaBundleFailure>;
    explicit MonitoringQ4ResolvedSchemaBundle(std::array<std::string, schema_count> values,
                                              std::array<std::size_t, schema_count> nodes) noexcept
        : resolved_{std::move(values)}, visited_nodes_{nodes} {}
    friend class MonitoringQ4SchemaBundleTestAccess;
    std::array<std::string, schema_count> resolved_;
    std::array<std::size_t, schema_count> visited_nodes_{};
};

[[nodiscard]] auto resolve_monitoring_q4_schema_bundle(
    const std::array<MonitoringQ4SchemaInput, MonitoringQ4ResolvedSchemaBundle::schema_count>&)
    -> std::expected<MonitoringQ4ResolvedSchemaBundle, MonitoringQ4SchemaBundleFailure>;

} // namespace epycbench::monitoring::qualification::detail
