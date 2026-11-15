#pragma once
#include "epycbench/core/error.hpp"
#include "monitoring/host_q1_git_command_owner.hpp"
#include "monitoring/host_q3_authority_session.hpp"
#include "monitoring/host_q3_temperature_collector.hpp"
#include "monitoring/host_q3_temperature_evidence_value.hpp"
#include <array>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>
namespace epycbench::runtime {
class ControllerProcessProductionAuthority;
}
namespace epycbench::monitoring::qualification::detail {
using MonitoringQ3AuthoredDiagnostic = MonitoringGovernedPlanAuthoredDiagnostic;
enum class MonitoringHostQ3BackendPhase : unsigned char {
    repository,
    collection_plan,
    predecessor,
    build_attestation,
    source_discovery,
    oracle,
    source_read,
    agreement,
    clock,
    deadline,
    capacity,
    finalization,
    evidence_construction
};
struct MonitoringHostQ3BackendBytes final {
    std::string canonical_json;
};
struct MonitoringHostQ3BackendAuthored final {
    std::vector<MonitoringGovernedPlanAuthoredDiagnostic> diagnostics;
};
using MonitoringHostQ3BackendResult =
    std::variant<MonitoringHostQ3BackendBytes, MonitoringHostQ3BackendAuthored>;
struct MonitoringHostQ3BackendFailure final {
    MonitoringHostQ3BackendPhase phase{};
    core::Error primary;
    std::array<std::optional<core::Error>, 31> secondary{};
    std::size_t secondary_count{};
    bool cleanup_overflowed{};
    std::array<char, 128> underlying_code{};
    std::size_t underlying_code_size{};
    std::array<char, 32> repository_classification{};
    std::size_t repository_classification_size{};
    bool terminal_containment{};
    bool usage_failure{};
};
class MonitoringHostQ3BackendTestAccess;
class MonitoringHostQ3BackendBody final {
  private:
    enum class AssemblyFailureKind : unsigned char { ordinary_validation, terminal_containment };
    struct AssemblyFailure final {
        AssemblyFailureKind kind{AssemblyFailureKind::terminal_containment};
        std::optional<core::Error> ordinary;
    };
    struct Operations final {
        void* context{};
        std::expected<MonitoringQ3SourceReady, MonitoringQ3AuthorityFailure> (
            *authenticate_and_finalize)(void*, const std::filesystem::path&, std::string_view){};
        std::expected<MonitoringQ3TemperatureCollection, MonitoringQ3TemperatureCollectionFailure> (
            *collect)(void*, const MonitoringQ3SourceReady&){};
        std::expected<MonitoringQ3TemperatureEvidenceValue, core::Error> (*build)(
            void*, const MonitoringQ3SourceReady&, const MonitoringQ3TemperatureCollection&){};
        std::expected<std::string, core::Error> (*encode)(
            void*, const MonitoringQ3TemperatureEvidenceValue&){};
        bool (*validate)(void*, std::string_view){};
        std::expected<std::string, AssemblyFailure> (*assemble)(void*, std::string_view,
                                                                std::string_view){};
        void (*fault)(void*, std::size_t){};
    };
    static auto run(const std::filesystem::path&, std::string_view, Operations) noexcept
        -> std::expected<MonitoringHostQ3BackendResult, MonitoringHostQ3BackendFailure>;
    static auto assemble(std::string_view, std::string_view) noexcept
        -> std::expected<std::string, AssemblyFailure>;
    static auto assemble_with(std::string_view, std::string_view, void*,
                              void (*)(void*, std::size_t)) noexcept
        -> std::expected<std::string, AssemblyFailure>;
    friend class MonitoringHostQ3BackendTestAccess;
    friend auto
    run_monitoring_host_q3_backend(const std::filesystem::path&, std::string_view,
                                   runtime::ControllerProcessProductionAuthority&) noexcept
        -> std::expected<MonitoringHostQ3BackendResult, MonitoringHostQ3BackendFailure>;
};
class MonitoringHostQ3BackendTestAccess final {
  public:
    using Operations = MonitoringHostQ3BackendBody::Operations;
    using AssemblyFailure = MonitoringHostQ3BackendBody::AssemblyFailure;
    using AssemblyFailureKind = MonitoringHostQ3BackendBody::AssemblyFailureKind;
    static auto run(const std::filesystem::path& root, std::string_view qid,
                    Operations operations) noexcept {
        return MonitoringHostQ3BackendBody::run(root, qid, operations);
    }
    static auto assemble(std::string_view build, std::string_view evidence) noexcept {
        return MonitoringHostQ3BackendBody::assemble(build, evidence);
    }
    static auto assemble_with(std::string_view build, std::string_view evidence, void* context,
                              void (*fault)(void*, std::size_t)) noexcept {
        return MonitoringHostQ3BackendBody::assemble_with(build, evidence, context, fault);
    }
    static void set_serialization_fault(void (*)(std::size_t));
    static void reset_serialization_fault() noexcept;
};
[[nodiscard]] auto
run_monitoring_host_q3_backend(const std::filesystem::path&, std::string_view,
                               runtime::ControllerProcessProductionAuthority&) noexcept
    -> std::expected<MonitoringHostQ3BackendResult, MonitoringHostQ3BackendFailure>;
[[nodiscard]] auto validate_monitoring_host_q3_backend_success(std::string_view) noexcept -> bool;
[[nodiscard]] auto
serialize_monitoring_host_q3_backend_failure(const MonitoringHostQ3BackendFailure&) noexcept
    -> std::expected<std::string, core::Error>;
[[nodiscard]] auto monitoring_host_q3_usage_failure() noexcept -> MonitoringHostQ3BackendFailure;
[[nodiscard]] auto
serialize_monitoring_host_q3_authored(const MonitoringHostQ3BackendAuthored&) noexcept
    -> std::expected<std::string, core::Error>;
} // namespace epycbench::monitoring::qualification::detail
