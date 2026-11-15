#pragma once

#include "epycbench/monitoring/frequency.hpp"
#include "monitoring/host_q4_cpufreq_inventory_authority.hpp"
#include "monitoring/host_q4_cpufreq_semantic_agreement.hpp"

#include <expected>
#include <memory>
#include <optional>

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringQ4PreparedSourceFailureKind : unsigned char {
    inactive,
    cpu_not_admitted,
    inventory,
    ambiguous_policy,
    semantic_overlap,
    semantic_conflict,
    semantic_agreement,
    production_semantics,
    allocation,
    open_attribute,
    held_stat,
    named_stat,
    identity,
    read,
    read_contract,
    byte_limit,
    production_parser,
    oracle_parser,
    parser_disagreement,
    close,
    contained
};

enum class MonitoringQ4PreparedCleanupRole : unsigned char {
    fresh_attribute,
    selected_policy,
    retained_policy
};
struct MonitoringQ4PreparedCleanup final {
    MonitoringQ4PreparedCleanupRole role{};
    std::uint64_t policy_ordinal{};
    std::uint64_t attribute_ordinal{};
    int native_error{};
    bool uncertain{};
};

struct MonitoringQ4PreparedSourceFailure final {
    MonitoringQ4PreparedSourceFailureKind kind{};
    int native_error{};
    bool cleanup_uncertain{};
    int cleanup_native_error{};
    std::array<MonitoringQ4PreparedCleanup, 8> cleanup{};
    std::size_t cleanup_count{};
    bool cleanup_overflowed{};
    std::optional<MonitoringQ4CpuFreqInventoryFailure> inventory;
    std::optional<MonitoringQ4CpuFreqSemanticFailure> semantic;
    MonitoringQ4CapacityReason reason{MonitoringQ4CapacityReason::none};
    MonitoringQ4PreparedSourceFailure(MonitoringQ4PreparedSourceFailureKind value,
                                      int native = 0, bool uncertain = false,
                                      std::optional<MonitoringQ4CpuFreqInventoryFailure> cause =
                                          std::nullopt,
                                      int cleanup_native = 0,
                                      std::optional<MonitoringQ4CpuFreqSemanticFailure>
                                          semantic_cause = std::nullopt,
                                      MonitoringQ4CapacityReason why = MonitoringQ4CapacityReason::none) noexcept
        : kind{value}, native_error{native}, cleanup_uncertain{uncertain},
          cleanup_native_error{cleanup_native}, inventory{std::move(cause)},
          semantic{semantic_cause}, reason{why} {}
    void append_cleanup(MonitoringQ4PreparedCleanup value) noexcept {
        cleanup_uncertain = cleanup_uncertain || value.uncertain;
        if (cleanup_native_error == 0) cleanup_native_error = value.native_error;
        if (cleanup_count < cleanup.size()) cleanup[cleanup_count++] = value;
        else cleanup_overflowed = true;
    }
};

struct MonitoringQ4PreparedSourceUnsupported final {
    MonitoringQ4CpuFreqUnsupportedReason reason{};
};

class MonitoringQ4CpuFreqPreparedSourceAuthorityTestAccess;
class MonitoringQ4CpuFreqCycleAuthority;

class MonitoringQ4CpuFreqPreparedSourceAuthority final {
  public:
    class Workspace;
    MonitoringQ4CpuFreqPreparedSourceAuthority(
        const MonitoringQ4CpuFreqPreparedSourceAuthority&) = delete;
    MonitoringQ4CpuFreqPreparedSourceAuthority(
        MonitoringQ4CpuFreqPreparedSourceAuthority&&) noexcept;
    auto operator=(const MonitoringQ4CpuFreqPreparedSourceAuthority&)
        -> MonitoringQ4CpuFreqPreparedSourceAuthority& = delete;
    auto operator=(MonitoringQ4CpuFreqPreparedSourceAuthority&&)
        -> MonitoringQ4CpuFreqPreparedSourceAuthority& = delete;
    ~MonitoringQ4CpuFreqPreparedSourceAuthority() noexcept;

    struct Preparation;

    [[nodiscard]] static auto prepare_cpu(const MonitoringQ4SourceReady&, core::LinuxCpuId) noexcept
        -> std::expected<Preparation, MonitoringQ4PreparedSourceFailure>;
    [[nodiscard]] auto raw_agreement() noexcept
        -> std::expected<std::uint64_t, MonitoringQ4PreparedSourceFailure>;
    [[nodiscard]] auto typed_read() noexcept
        -> std::expected<std::uint64_t, MonitoringQ4PreparedSourceFailure>;
    [[nodiscard]] auto metadata() const noexcept -> const monitoring::FrequencyPolicyMetadata&;
    [[nodiscard]] auto retire() noexcept
        -> std::expected<void, MonitoringQ4PreparedSourceFailure>;

  private:
    struct State;
    struct Selection;
    explicit MonitoringQ4CpuFreqPreparedSourceAuthority(std::unique_ptr<State>,
                                                         monitoring::FrequencySource) noexcept;
    static auto prepare_with(const MonitoringQ4SourceReady&, core::LinuxCpuId,
                             MonitoringQ4CpuFreqInventoryAuthority::Operations) noexcept
        -> std::expected<Preparation, MonitoringQ4PreparedSourceFailure>;
    static auto build_workspace(const MonitoringQ4SourceReady&, core::LinuxCpuId,
                                MonitoringQ4CpuFreqInventoryAuthority::Operations) noexcept
        -> std::expected<Workspace, MonitoringQ4PreparedSourceFailure>;
    static auto prepare_with_workspace(const MonitoringQ4SourceReady&, Workspace&&) noexcept
        -> std::expected<Preparation, MonitoringQ4PreparedSourceFailure>;
    static auto select_policy(void*, int, std::uint64_t,
                              const MonitoringQ4CpuFreqPolicyInventory&,
                              std::span<const char>) noexcept -> int;
    static auto typed_callback(void*) noexcept -> std::expected<std::uint64_t, core::Error>;
    static auto acquire_state(State&, bool) noexcept
        -> std::expected<std::uint64_t, MonitoringQ4PreparedSourceFailure>;
    auto acquire(bool) noexcept
        -> std::expected<std::uint64_t, MonitoringQ4PreparedSourceFailure>;
    void force_oracle_disagreement() noexcept;
    void force_parser_rejection(bool, bool) noexcept;
    [[nodiscard]] auto parser_calls() const noexcept -> std::array<std::uint64_t, 2>;

    std::unique_ptr<State> state_;
    monitoring::FrequencySource source_;
    friend class MonitoringQ4CpuFreqPreparedSourceAuthorityTestAccess;
    friend class MonitoringQ4CpuFreqCycleAuthority;
};

class MonitoringQ4CpuFreqPreparedSourceAuthority::Workspace final {
  public:
    Workspace(const Workspace&) = delete;
    Workspace(Workspace&&) noexcept;
    ~Workspace() noexcept;
  private:
    Workspace(core::LinuxCpuId cpu, std::unique_ptr<Selection> selection,
              std::unique_ptr<State> state,
              MonitoringQ4CpuFreqSemanticWorkspace semantic,
              MonitoringQ4CpuFreqInventoryAuthority::Workspace inventory) noexcept;
    core::LinuxCpuId cpu_{0};
    std::unique_ptr<Selection> selection_;
    std::unique_ptr<State> state_;
    std::optional<MonitoringQ4CpuFreqSemanticWorkspace> semantic_;
    std::optional<MonitoringQ4CpuFreqInventoryAuthority::Workspace> inventory_;
    friend class MonitoringQ4CpuFreqPreparedSourceAuthority;
    friend class MonitoringQ4CpuFreqCycleAuthority;
};

struct MonitoringQ4CpuFreqPreparedSourceAuthority::Preparation final {
    std::optional<MonitoringQ4CpuFreqPreparedSourceAuthority> source;
    std::optional<MonitoringQ4PreparedSourceUnsupported> unsupported;
};

} // namespace epycbench::monitoring::qualification::detail
