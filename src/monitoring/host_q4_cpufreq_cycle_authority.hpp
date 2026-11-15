#pragma once
#include "monitoring/host_q4_cpufreq_prepared_source_authority.hpp"
#include <expected>
#include <memory>
#include <optional>
#include <vector>
namespace epycbench::monitoring::qualification::detail {
enum class MonitoringQ4CpuFreqCycleFailureKind : unsigned char {
    inactive,
    allocation,
    capacity,
    initial_inventory,
    initial_semantics,
    preparation,
    metadata,
    raw_read,
    typed_read,
    final_inventory,
    final_semantics,
    semantic_drift,
    retirement,
    contained
};
struct MonitoringQ4CpuFreqCycleRetirementFailure final {
    std::uint64_t admitted_ordinal{};
    MonitoringQ4PreparedSourceFailure failure{MonitoringQ4PreparedSourceFailureKind::inactive};
};
enum class MonitoringQ4CpuFreqCycleCleanupRole : unsigned char {
    attribute = 1,
    policy = 2,
    root = 3,
    fresh_attribute = 4,
    selected_policy = 5,
    retained_policy = 6
};
struct MonitoringQ4CpuFreqCycleCleanupEvent final {
    MonitoringQ4CpuFreqCycleCleanupRole role{};
    std::uint64_t policy_ordinal{}, attribute_ordinal{};
    int native_error{};
    bool uncertain{};
    auto operator==(const MonitoringQ4CpuFreqCycleCleanupEvent&) const noexcept -> bool = default;
};
struct MonitoringQ4CpuFreqCleanupLedgerFailure final {
    MonitoringQ4CpuFreqCycleFailureKind kind;
    MonitoringQ4CapacityReason reason;
};
static_assert(std::is_trivially_copyable_v<MonitoringQ4CpuFreqCleanupLedgerFailure>);
class MonitoringQ4CpuFreqCleanupLedger final {
  public:
    MonitoringQ4CpuFreqCleanupLedger(const MonitoringQ4CpuFreqCleanupLedger&) = delete;
    MonitoringQ4CpuFreqCleanupLedger(MonitoringQ4CpuFreqCleanupLedger&&) noexcept = default;
    [[nodiscard]] static auto create(const MonitoringQ4SourceReady&) noexcept
        -> std::expected<MonitoringQ4CpuFreqCleanupLedger, MonitoringQ4CpuFreqCleanupLedgerFailure>;
    [[nodiscard]] auto events() const noexcept -> std::span<const MonitoringQ4CpuFreqCycleCleanupEvent>;
    [[nodiscard]] auto capacity() const noexcept -> std::size_t;
    [[nodiscard]] auto overflowed() const noexcept -> bool;

  private:
    struct Storage final {
        std::vector<MonitoringQ4CpuFreqCycleCleanupEvent> events;
        std::size_t limit{};
        bool overflowed{};
    };
    explicit MonitoringQ4CpuFreqCleanupLedger(std::unique_ptr<Storage> storage) noexcept
        : storage_{std::move(storage)} {}
    std::unique_ptr<Storage> storage_;
    friend class MonitoringQ4CpuFreqCycleAuthority;
    friend class MonitoringQ4CpuFreqCycleAuthorityTestAccess;
    friend class MonitoringQ4CpuFreqCollector;
};
struct MonitoringQ4CpuFreqCycleFailure final {
    MonitoringQ4CpuFreqCycleFailureKind kind{};
    std::uint64_t admitted_ordinal{}, round_ordinal{}, domain_ordinal{};
    std::optional<MonitoringQ4CpuFreqInventoryFailure> inventory;
    std::optional<MonitoringQ4CpuFreqSemanticFailure> semantic;
    std::optional<MonitoringQ4PreparedSourceFailure> source;
    std::vector<MonitoringQ4CpuFreqCycleRetirementFailure> cleanup;
    bool cleanup_overflowed{};
    std::size_t cleanup_begin{}, cleanup_end{};
    MonitoringQ4CapacityReason reason{MonitoringQ4CapacityReason::none};
    MonitoringQ4CpuFreqCycleFailure(
        MonitoringQ4CpuFreqCycleFailureKind value, std::uint64_t admitted = 0,
        std::uint64_t round = 0, std::uint64_t domain = 0,
        std::optional<MonitoringQ4CpuFreqInventoryFailure> inventory_cause = {},
        std::optional<MonitoringQ4CpuFreqSemanticFailure> semantic_cause = {},
        std::optional<MonitoringQ4PreparedSourceFailure> source_cause = {},
        MonitoringQ4CapacityReason why = MonitoringQ4CapacityReason::none) noexcept
        : kind{value}, admitted_ordinal{admitted}, round_ordinal{round}, domain_ordinal{domain},
          inventory{std::move(inventory_cause)}, semantic{std::move(semantic_cause)},
          source{std::move(source_cause)}, reason{why} {}
};
struct MonitoringQ4CpuFreqCyclePreparationFact final {
    core::LinuxCpuId cpu{0};
    std::optional<std::uint64_t> domain_ordinal;
    std::optional<MonitoringQ4CpuFreqUnsupportedReason> unsupported;
};
struct MonitoringQ4CpuFreqCycleReadFact final {
    std::uint64_t round_ordinal{}, domain_ordinal{}, value_hz{};
};
class MonitoringQ4CpuFreqCycleResult final {
  public:
    MonitoringQ4CpuFreqCycleResult(const MonitoringQ4CpuFreqCycleResult&) = delete;
    MonitoringQ4CpuFreqCycleResult(MonitoringQ4CpuFreqCycleResult&&) noexcept = default;
    [[nodiscard]] auto unsupported() const noexcept { return unsupported_; }
    [[nodiscard]] auto initial() const noexcept -> const MonitoringQ4CpuFreqSemanticSnapshot& {
        return initial_;
    }
    [[nodiscard]] auto preparations() const noexcept { return std::span{preparations_}; }
    [[nodiscard]] auto raw_reads() const noexcept { return std::span{raw_reads_}; }
    [[nodiscard]] auto typed_reads() const noexcept { return std::span{typed_reads_}; }
    [[nodiscard]] auto cleanup_begin() const noexcept { return cleanup_begin_; }
    [[nodiscard]] auto cleanup_end() const noexcept { return cleanup_end_; }

  private:
    MonitoringQ4CpuFreqCycleResult(bool u, MonitoringQ4CpuFreqSemanticSnapshot i,
                                   std::vector<MonitoringQ4CpuFreqCyclePreparationFact> p,
                                   std::vector<MonitoringQ4CpuFreqCycleReadFact> r,
                                   std::vector<MonitoringQ4CpuFreqCycleReadFact> t,
                                   std::size_t cleanup_begin, std::size_t cleanup_end) noexcept
        : unsupported_{u}, initial_{std::move(i)}, preparations_{std::move(p)},
          raw_reads_{std::move(r)}, typed_reads_{std::move(t)}, cleanup_begin_{cleanup_begin},
          cleanup_end_{cleanup_end} {}
    bool unsupported_{};
    MonitoringQ4CpuFreqSemanticSnapshot initial_;
    std::vector<MonitoringQ4CpuFreqCyclePreparationFact> preparations_;
    std::vector<MonitoringQ4CpuFreqCycleReadFact> raw_reads_, typed_reads_;
    std::size_t cleanup_begin_{}, cleanup_end_{};
    friend class MonitoringQ4CpuFreqCycleAuthority;
};
class MonitoringQ4CpuFreqCycleAuthorityTestAccess;
class MonitoringQ4CpuFreqCycleAuthority final {
  public:
    MonitoringQ4CpuFreqCycleAuthority(const MonitoringQ4CpuFreqCycleAuthority&) = delete;
    MonitoringQ4CpuFreqCycleAuthority(MonitoringQ4CpuFreqCycleAuthority&&) noexcept;
    ~MonitoringQ4CpuFreqCycleAuthority() noexcept;
    [[nodiscard]] static auto create(const MonitoringQ4SourceReady&,
                                     MonitoringQ4CpuFreqCleanupLedger&) noexcept
        -> std::expected<MonitoringQ4CpuFreqCycleAuthority, MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] auto initial_discovery() noexcept
        -> std::expected<void, MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] auto initial_snapshot() const noexcept
        -> const MonitoringQ4CpuFreqSemanticSnapshot* { return initial_ ? &*initial_ : nullptr; }
    [[nodiscard]] auto prepare_next() noexcept
        -> std::expected<MonitoringQ4CpuFreqCyclePreparationFact, MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] auto finish_preparations() noexcept
        -> std::expected<bool, MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] auto raw_next() noexcept
        -> std::expected<MonitoringQ4CpuFreqCycleReadFact, MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] auto typed_next() noexcept
        -> std::expected<MonitoringQ4CpuFreqCycleReadFact, MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] auto final_discovery_and_compare() noexcept
        -> std::expected<void, MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] auto retire_next() noexcept
        -> std::expected<bool, MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] auto take_result() noexcept
        -> std::expected<MonitoringQ4CpuFreqCycleResult, MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] auto abort() noexcept
        -> std::optional<MonitoringQ4CpuFreqCycleFailure>;
    [[nodiscard]] static auto execute(const MonitoringQ4SourceReady&,
                                      MonitoringQ4CpuFreqCleanupLedger&) noexcept
        -> std::expected<MonitoringQ4CpuFreqCycleResult, MonitoringQ4CpuFreqCycleFailure>;

  private:
    using Operations = MonitoringQ4CpuFreqInventoryAuthority::Operations;
    enum class Stage : unsigned char {
        created,
        initial,
        preparing,
        reads,
        final,
        retiring,
        complete,
        terminal
    };
    MonitoringQ4CpuFreqCycleAuthority(const MonitoringQ4SourceReady&,
                                      MonitoringQ4CpuFreqCleanupLedger::Storage*, Operations) noexcept;
    static auto create_with(const MonitoringQ4SourceReady&, MonitoringQ4CpuFreqCleanupLedger&,
                            Operations) noexcept
        -> std::expected<MonitoringQ4CpuFreqCycleAuthority, MonitoringQ4CpuFreqCycleFailure>;
    static auto execute_with(const MonitoringQ4SourceReady&, MonitoringQ4CpuFreqCleanupLedger&,
                             Operations) noexcept
        -> std::expected<MonitoringQ4CpuFreqCycleResult, MonitoringQ4CpuFreqCycleFailure>;
    auto discover(std::optional<MonitoringQ4CpuFreqSemanticWorkspace>&,
                  std::optional<MonitoringQ4CpuFreqInventoryAuthority::Workspace>&,
                  MonitoringQ4CpuFreqCycleFailureKind, MonitoringQ4CpuFreqCycleFailureKind) noexcept
        -> std::expected<MonitoringQ4CpuFreqSemanticSnapshot, MonitoringQ4CpuFreqCycleFailure>;
    auto terminate(MonitoringQ4CpuFreqCycleFailure) noexcept
        -> std::expected<void, MonitoringQ4CpuFreqCycleFailure>;
    void retire_source(std::size_t, MonitoringQ4CpuFreqCycleFailure*) noexcept;
    auto guard_cleanup_capacity() noexcept -> std::expected<void, MonitoringQ4CpuFreqCycleFailure>;
    static void observe_cleanup(void*, unsigned, std::uint64_t, std::uint64_t, int, bool) noexcept;
    const MonitoringQ4SourceReady* ready_{};
    Operations operations_{};
    Stage stage_{Stage::terminal};
    std::optional<MonitoringQ4CpuFreqSemanticWorkspace> initial_workspace_, final_workspace_;
    std::optional<MonitoringQ4CpuFreqInventoryAuthority::Workspace> initial_inventory_,
        final_inventory_;
    std::vector<std::optional<MonitoringQ4CpuFreqPreparedSourceAuthority::Workspace>> workspaces_;
    std::vector<std::optional<MonitoringQ4CpuFreqPreparedSourceAuthority>> sources_;
    std::vector<MonitoringQ4CpuFreqCyclePreparationFact> preparations_;
    std::vector<std::size_t> canonical_, read_order_;
    std::vector<MonitoringQ4CpuFreqCycleReadFact> raw_, typed_;
    std::vector<MonitoringQ4CpuFreqCycleRetirementFailure> cleanup_;
    std::size_t cleanup_limit_{};
    MonitoringQ4CpuFreqCleanupLedger::Storage* cleanup_ledger_{};
    std::size_t cleanup_begin_{};
    std::optional<MonitoringQ4CpuFreqSemanticSnapshot> initial_;
    std::size_t prepare_index_{}, read_domain_{}, retire_index_{};
    std::uint64_t round_{1}, rounds_{};
    bool expect_typed_{}, unsupported_{}, cleanup_overflowed_{};
    friend class MonitoringQ4CpuFreqCycleAuthorityTestAccess;
    friend class MonitoringQ4CpuFreqCollector;
};
} // namespace epycbench::monitoring::qualification::detail
