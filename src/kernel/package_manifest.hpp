#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::kernel::packages {

inline constexpr std::uint64_t maximum_binary_bytes = 64U * 1024U * 1024U;

enum class ParameterType { uint64, int64, float64, boolean, enumeration };
enum class MappingResource { none, first_touch, allocation };

struct ParameterDefinition {
    ParameterType type;
    std::optional<nlohmann::json> default_value, minimum, maximum;
    std::vector<std::string> values;
};

using ParameterDefinitions = std::map<std::string, ParameterDefinition, std::less<>>;
using PortDefinitions = std::map<std::string, std::string, std::less<>>;
using AcceptedValues = std::map<std::string, std::vector<nlohmann::json>, std::less<>>;

struct TaskPorts {
    PortDefinitions inputs, outputs;
};

struct RankConstraints {
    std::uint32_t minimum{1};
    std::uint32_t maximum{std::numeric_limits<std::uint32_t>::max()};
    bool distinct_cores{};
};

struct TaskManifest {
    std::string id;
    ParameterDefinitions parameters;
    TaskPorts ports;
    RankConstraints ranks;
    bool collective{};
    bool collective_finalizer{};
    bool shared_cpu_local{};
    bool per_worker_input{};
    MappingResource mapping{};
    std::vector<std::string> control_inputs{};
    std::vector<std::string> partition_extent_parameters{};
    std::string work_unit, work_metric;
    std::filesystem::path path, oracle_path;
    std::string contents;
    core::Sha256Digest sha256, oracle_sha256;
};

// Internal error context carried by installed package layout inspection.
inline constexpr std::string_view layout_related_settings = "related_settings";

struct ImplementationManifest {
    std::string id;
    std::shared_ptr<const TaskManifest> task;
    ParameterDefinitions tuning;
    AcceptedValues accepts;
    std::filesystem::path path, module_path;
    std::string contents;
    core::Sha256Digest sha256, module_sha256;
};

[[nodiscard]] nlohmann::json package_identity(const ImplementationManifest&);

class Catalog {
  public:
    using LayoutInspector = std::expected<nlohmann::json,core::Error> (*)(
        const Catalog&,const nlohmann::json&,std::uint64_t);
    // The host supplies binary admission for installed packages. Offline catalogs
    // have no native callbacks and use the geometry retained by the Controller.
    LayoutInspector inspect_layouts{};

    // Discover *.kernel.yaml and the *.task.yaml files they reference without loading code.
    [[nodiscard]] static auto discover(const std::filesystem::path& directory)
        -> std::expected<Catalog, core::Error>;
    [[nodiscard]] static auto load_manifests(std::span<const std::filesystem::path>)
        -> std::expected<Catalog, core::Error>;

    // Restore a metadata-only catalog from Result bindings. Restored entries have no file paths.
    [[nodiscard]] static auto from_bindings(const nlohmann::json& bindings)
        -> std::expected<Catalog, core::Error>;

    [[nodiscard]] auto implementations() const noexcept -> std::span<const ImplementationManifest> {
        return implementations_;
    }

    [[nodiscard]] auto find(std::string_view task_id, std::string_view implementation_id)
        const noexcept -> const ImplementationManifest*;

  private:
    std::vector<ImplementationManifest> implementations_;
};

// The task parameters and implementation tuning use separate definitions and maps.
// Defaults and supplied values share the same type, bounds and enum validation.
[[nodiscard]] auto resolve_parameters(const ParameterDefinitions& definitions,
                                      const nlohmann::json& supplied, std::string_view field)
    -> std::expected<nlohmann::json, core::Error>;

[[nodiscard]] auto validate_implementation_inputs(const AcceptedValues&, const nlohmann::json& resolved,
                                                  std::string_view field,
                                                  std::string* rejected_parameter = nullptr)
    -> std::expected<void, core::Error>;

// Validate retained manifests and fully resolved values without consulting installed files.
[[nodiscard]] auto validate_saved_values(std::string_view task_yaml, std::string_view implementation_yaml,
    std::string_view task_id, std::string_view implementation_id, const nlohmann::json& effective_parameters,
    const nlohmann::json& effective_tuning) -> std::expected<void, core::Error>;

} // namespace epycbench::kernel::packages
