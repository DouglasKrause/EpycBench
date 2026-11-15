#include "monitoring/host_q4_cpufreq_cycle_authority.hpp"
#include <algorithm>
#include <limits>
#include <utility>
namespace epycbench::monitoring::qualification::detail {
namespace {
using Reason = MonitoringQ4CapacityReason;
auto metadata_equal(const monitoring::FrequencyPolicyMetadata& v,
                    const MonitoringQ4CpuFreqDomain& e) noexcept -> bool {
    return std::ranges::equal(v.domain_id().cpus(), e.cpus) && v.source_kind() == e.source_kind &&
           v.scaling_driver().value() == e.driver && v.scaling_governor().value() == e.governor &&
           v.scaling_minimum().value() == e.minimum_hz &&
           v.scaling_maximum().value() == e.maximum_hz;
}
auto snapshots_equal(const MonitoringQ4CpuFreqSemanticSnapshot& a,
                     const MonitoringQ4CpuFreqSemanticSnapshot& b) noexcept -> bool {
    if (a.policy_count() != b.policy_count() || a.cpus().size() != b.cpus().size() ||
        a.domains().size() != b.domains().size() || !std::ranges::equal(a.cpus(), b.cpus()))
        return false;
    for (std::size_t i{}; i < a.domains().size(); ++i) {
        const auto& x = a.domains()[i];
        const auto& y = b.domains()[i];
        if (!std::ranges::equal(x.cpus, y.cpus) || x.source_kind != y.source_kind ||
            x.driver != y.driver || x.governor != y.governor || x.minimum_hz != y.minimum_hz ||
            x.maximum_hz != y.maximum_hz)
            return false;
    }
    return true;
}
} // namespace
auto MonitoringQ4CpuFreqCleanupLedger::create(const MonitoringQ4SourceReady& ready) noexcept
    -> std::expected<MonitoringQ4CpuFreqCleanupLedger, MonitoringQ4CpuFreqCleanupLedgerFailure> {
    auto allocation_reason = Reason::ledger_storage_allocation;
    try {
        const auto capacity = ready.inputs().structural().cleanup_events().value;
        if (capacity > std::numeric_limits<std::size_t>::max())
            return std::unexpected{MonitoringQ4CpuFreqCleanupLedgerFailure{MonitoringQ4CpuFreqCycleFailureKind::capacity, Reason::ledger_size}};
        auto storage = std::make_unique<Storage>();
        storage->limit = static_cast<std::size_t>(capacity);
        allocation_reason = Reason::ledger_events_allocation;
        storage->events.reserve(storage->limit);
        return MonitoringQ4CpuFreqCleanupLedger{std::move(storage)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{MonitoringQ4CpuFreqCleanupLedgerFailure{MonitoringQ4CpuFreqCycleFailureKind::allocation, allocation_reason}};
    } catch (...) {
        return std::unexpected{MonitoringQ4CpuFreqCleanupLedgerFailure{MonitoringQ4CpuFreqCycleFailureKind::contained, Reason::none}};
    }
}
auto MonitoringQ4CpuFreqCleanupLedger::events() const noexcept
    -> std::span<const MonitoringQ4CpuFreqCycleCleanupEvent> {
    return storage_ ? std::span<const MonitoringQ4CpuFreqCycleCleanupEvent>{storage_->events}
                    : std::span<const MonitoringQ4CpuFreqCycleCleanupEvent>{};
}
auto MonitoringQ4CpuFreqCleanupLedger::capacity() const noexcept -> std::size_t {
    return storage_ ? storage_->limit : 0;
}
auto MonitoringQ4CpuFreqCleanupLedger::overflowed() const noexcept -> bool {
    return storage_ && storage_->overflowed;
}
MonitoringQ4CpuFreqCycleAuthority::MonitoringQ4CpuFreqCycleAuthority(
    const MonitoringQ4SourceReady& r, MonitoringQ4CpuFreqCleanupLedger::Storage* ledger,
    Operations o) noexcept
    : ready_{&r}, operations_{o}, stage_{Stage::created}, cleanup_ledger_{ledger},
      cleanup_begin_{ledger ? ledger->events.size() : 0} {}
MonitoringQ4CpuFreqCycleAuthority::MonitoringQ4CpuFreqCycleAuthority(
    MonitoringQ4CpuFreqCycleAuthority&& o) noexcept
    : ready_{std::exchange(o.ready_, nullptr)}, operations_{o.operations_},
      stage_{std::exchange(o.stage_, Stage::terminal)},
      initial_workspace_{std::move(o.initial_workspace_)},
      final_workspace_{std::move(o.final_workspace_)},
      initial_inventory_{std::move(o.initial_inventory_)},
      final_inventory_{std::move(o.final_inventory_)}, workspaces_{std::move(o.workspaces_)},
      sources_{std::move(o.sources_)}, preparations_{std::move(o.preparations_)},
      canonical_{std::move(o.canonical_)}, read_order_{std::move(o.read_order_)},
      raw_{std::move(o.raw_)}, typed_{std::move(o.typed_)}, cleanup_{std::move(o.cleanup_)},
      cleanup_limit_{o.cleanup_limit_},
      cleanup_ledger_{std::exchange(o.cleanup_ledger_, nullptr)},
      cleanup_begin_{o.cleanup_begin_}, initial_{std::move(o.initial_)},
      prepare_index_{o.prepare_index_}, read_domain_{o.read_domain_},
      retire_index_{o.retire_index_}, round_{o.round_}, rounds_{o.rounds_},
      expect_typed_{o.expect_typed_}, unsupported_{o.unsupported_},
      cleanup_overflowed_{o.cleanup_overflowed_} {
    operations_.cleanup_context = cleanup_ledger_;
}
MonitoringQ4CpuFreqCycleAuthority::~MonitoringQ4CpuFreqCycleAuthority() noexcept {
    MonitoringQ4CpuFreqCycleFailure ignored{MonitoringQ4CpuFreqCycleFailureKind::retirement};
    for (auto i = sources_.size(); i > 0; --i)
        retire_source(i - 1, &ignored);
}
auto MonitoringQ4CpuFreqCycleAuthority::create(const MonitoringQ4SourceReady& r,
                                                MonitoringQ4CpuFreqCleanupLedger& ledger) noexcept
    -> std::expected<MonitoringQ4CpuFreqCycleAuthority, MonitoringQ4CpuFreqCycleFailure> {
    return create_with(r, ledger, MonitoringQ4CpuFreqInventoryAuthority::native_operations());
}
auto MonitoringQ4CpuFreqCycleAuthority::create_with(const MonitoringQ4SourceReady& r,
                                                    MonitoringQ4CpuFreqCleanupLedger& ledger,
                                                    Operations o) noexcept
    -> std::expected<MonitoringQ4CpuFreqCycleAuthority, MonitoringQ4CpuFreqCycleFailure> {
    auto allocation_reason = Reason::cycle_workspaces_allocation;
    try {
        if (!ledger.storage_)
            return std::unexpected{MonitoringQ4CpuFreqCycleFailure{
                MonitoringQ4CpuFreqCycleFailureKind::inactive}};
        MonitoringQ4CpuFreqCycleAuthority v{r, ledger.storage_.get(), o};
        const auto admitted = r.inputs().admitted_cpus();
        v.rounds_ = r.authority().governed().plan().agreement_rounds_per_cycle().value;
        v.operations_.cleanup_context = v.cleanup_ledger_;
        v.operations_.observe_cleanup = observe_cleanup;
        auto iw = MonitoringQ4CpuFreqSemanticWorkspace::prepare(r);
        if (!iw)
            return std::unexpected{
                MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::allocation,
                                                0,
                                                0,
                                                0,
                                                {},
                                                std::move(iw.error())}};
        v.initial_workspace_.emplace(std::move(*iw));
        auto fw = MonitoringQ4CpuFreqSemanticWorkspace::prepare(r);
        if (!fw)
            return std::unexpected{
                MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::allocation,
                                                0,
                                                0,
                                                0,
                                                {},
                                                std::move(fw.error())}};
        v.final_workspace_.emplace(std::move(*fw));
        auto ii =
            MonitoringQ4CpuFreqInventoryAuthority::prepare_workspace(r.inputs(), v.operations_);
        if (!ii)
            return std::unexpected{MonitoringQ4CpuFreqCycleFailure{
                MonitoringQ4CpuFreqCycleFailureKind::allocation, 0, 0, 0, std::move(ii.error())}};
        v.initial_inventory_.emplace(std::move(*ii));
        auto fi =
            MonitoringQ4CpuFreqInventoryAuthority::prepare_workspace(r.inputs(), v.operations_);
        if (!fi)
            return std::unexpected{MonitoringQ4CpuFreqCycleFailure{
                MonitoringQ4CpuFreqCycleFailureKind::allocation, 0, 0, 0, std::move(fi.error())}};
        v.final_inventory_.emplace(std::move(*fi));
        allocation_reason = Reason::cycle_workspaces_allocation;
        v.workspaces_.reserve(admitted.size());
        allocation_reason = Reason::cycle_sources_allocation;
        v.sources_.resize(admitted.size());
        allocation_reason = Reason::cycle_preparations_allocation;
        v.preparations_.reserve(admitted.size());
        allocation_reason = Reason::cycle_canonical_allocation;
        v.canonical_.assign(admitted.size(), std::numeric_limits<std::size_t>::max());
        allocation_reason = Reason::cycle_read_order_allocation;
        v.read_order_.reserve(admitted.size());
        if (v.rounds_ > std::numeric_limits<std::size_t>::max())
            return std::unexpected{MonitoringQ4CpuFreqCycleFailure{
                MonitoringQ4CpuFreqCycleFailureKind::capacity, 0, 0, 0, {}, {}, {}, Reason::cycle_rounds_size}};
        if (v.rounds_ != 0 && admitted.size() > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(v.rounds_))
            return std::unexpected{MonitoringQ4CpuFreqCycleFailure{
                MonitoringQ4CpuFreqCycleFailureKind::capacity, 0, 0, 0, {}, {}, {}, Reason::cycle_read_count}};
        const auto reads = admitted.size() * static_cast<std::size_t>(v.rounds_);
        allocation_reason = Reason::cycle_raw_allocation;
        v.raw_.reserve(reads);
        allocation_reason = Reason::cycle_typed_allocation;
        v.typed_.reserve(reads);
        allocation_reason = Reason::cycle_cleanup_allocation;
        v.cleanup_.reserve(admitted.size());
        v.cleanup_limit_ = admitted.size();
        for (const auto cpu : admitted) {
            auto w =
                MonitoringQ4CpuFreqPreparedSourceAuthority::build_workspace(r, cpu, v.operations_);
            if (!w)
                return std::unexpected{MonitoringQ4CpuFreqCycleFailure{
                    MonitoringQ4CpuFreqCycleFailureKind::allocation,
                    static_cast<std::uint64_t>(v.workspaces_.size() + 1U),
                    0,
                    0,
                    {},
                    {},
                    std::move(w.error())}};
            allocation_reason = Reason::cycle_source_append_allocation;
            v.workspaces_.emplace_back(std::move(*w));
        }
        return v;
    } catch (const std::bad_alloc&) {
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::allocation, 0, 0, 0, {}, {}, {}, allocation_reason}};
    } catch (...) {
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::contained}};
    }
}
auto MonitoringQ4CpuFreqCycleAuthority::discover(
    std::optional<MonitoringQ4CpuFreqSemanticWorkspace>& sw,
    std::optional<MonitoringQ4CpuFreqInventoryAuthority::Workspace>& iw,
    MonitoringQ4CpuFreqCycleFailureKind ik, MonitoringQ4CpuFreqCycleFailureKind sk) noexcept
    -> std::expected<MonitoringQ4CpuFreqSemanticSnapshot, MonitoringQ4CpuFreqCycleFailure> {
    auto a = MonitoringQ4CpuFreqInventoryAuthority::retain_prepared(ready_->inputs(), operations_,
                                                                    std::move(*iw));
    iw.reset();
    if (!a)
        return std::unexpected{MonitoringQ4CpuFreqCycleFailure{ik, 0, 0, 0, std::move(a.error())}};
    MonitoringQ4CpuFreqInventoryAuthority::bind_ready(*a, *ready_);
    auto i = a->capture_and_retire();
    if (!i)
        return std::unexpected{MonitoringQ4CpuFreqCycleFailure{ik, 0, 0, 0, std::move(i.error())}};
    auto s = MonitoringQ4CpuFreqSemanticAgreement::evaluate(std::move(*sw), *i);
    sw.reset();
    if (!s)
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{sk, 0, 0, 0, {}, std::move(s.error())}};
    return std::move(*s);
}
void MonitoringQ4CpuFreqCycleAuthority::retire_source(
    std::size_t i, MonitoringQ4CpuFreqCycleFailure* primary) noexcept {
    if (!sources_[i])
        return;
    auto r = sources_[i]->retire();
    sources_[i].reset();
    if (!r) {
        if (primary && primary->kind != MonitoringQ4CpuFreqCycleFailureKind::retirement) {
            if (cleanup_.size() < cleanup_limit_)
                cleanup_.push_back({i + 1U, std::move(r.error())});
            else
                cleanup_overflowed_ = true;
        } else if (primary && !primary->source) {
            primary->kind = MonitoringQ4CpuFreqCycleFailureKind::retirement;
            primary->admitted_ordinal = i + 1U;
            primary->source.emplace(std::move(r.error()));
        } else if (cleanup_.size() < cleanup_limit_)
            cleanup_.push_back({i + 1U, std::move(r.error())});
        else
            cleanup_overflowed_ = true;
    }
}
void MonitoringQ4CpuFreqCycleAuthority::observe_cleanup(void* raw, unsigned role,
                                                        std::uint64_t policy,
                                                        std::uint64_t attribute, int native,
                                                        bool uncertain) noexcept {
    auto& l = *static_cast<MonitoringQ4CpuFreqCleanupLedger::Storage*>(raw);
    if (l.events.size() < l.limit)
        l.events.push_back({static_cast<MonitoringQ4CpuFreqCycleCleanupRole>(role), policy,
                            attribute, native, uncertain});
    else
        l.overflowed = true;
}
auto MonitoringQ4CpuFreqCycleAuthority::terminate(MonitoringQ4CpuFreqCycleFailure f) noexcept
    -> std::expected<void, MonitoringQ4CpuFreqCycleFailure> {
    for (auto i = sources_.size(); i > 0; --i)
        retire_source(i - 1, &f);
    stage_ = Stage::terminal;
    f.cleanup = std::move(cleanup_);
    f.cleanup_overflowed = cleanup_overflowed_ || (cleanup_ledger_ && cleanup_ledger_->overflowed);
    f.cleanup_begin = cleanup_begin_;
    f.cleanup_end = cleanup_ledger_ ? cleanup_ledger_->events.size() : cleanup_begin_;
    return std::unexpected{std::move(f)};
}
auto MonitoringQ4CpuFreqCycleAuthority::guard_cleanup_capacity() noexcept
    -> std::expected<void, MonitoringQ4CpuFreqCycleFailure> {
    if (cleanup_ledger_ && cleanup_ledger_->overflowed)
        return terminate({MonitoringQ4CpuFreqCycleFailureKind::capacity});
    return {};
}
auto MonitoringQ4CpuFreqCycleAuthority::initial_discovery() noexcept
    -> std::expected<void, MonitoringQ4CpuFreqCycleFailure> {
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    if (stage_ != Stage::created)
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::inactive}};
    cleanup_begin_ = cleanup_ledger_ ? cleanup_ledger_->events.size() : 0U;
    auto s = discover(initial_workspace_, initial_inventory_,
                      MonitoringQ4CpuFreqCycleFailureKind::initial_inventory,
                      MonitoringQ4CpuFreqCycleFailureKind::initial_semantics);
    if (!s) {
        auto x = terminate(std::move(s.error()));
        return std::unexpected{std::move(x.error())};
    }
    initial_.emplace(std::move(*s));
    stage_ = Stage::preparing;
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    return {};
}
auto MonitoringQ4CpuFreqCycleAuthority::prepare_next() noexcept
    -> std::expected<MonitoringQ4CpuFreqCyclePreparationFact, MonitoringQ4CpuFreqCycleFailure> {
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    if (stage_ != Stage::preparing || prepare_index_ >= workspaces_.size())
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::inactive}};
    const auto index = prepare_index_++;
    auto p = MonitoringQ4CpuFreqPreparedSourceAuthority::prepare_with_workspace(
        *ready_, std::move(*workspaces_[index]));
    workspaces_[index].reset();
    if (!p) {
        auto x = terminate({MonitoringQ4CpuFreqCycleFailureKind::preparation,
                            index + 1U,
                            0,
                            0,
                            {},
                            {},
                            std::move(p.error())});
        return std::unexpected{std::move(x.error())};
    }
    const auto& o = initial_->cpus()[index];
    MonitoringQ4CpuFreqCyclePreparationFact fact{ready_->inputs().admitted_cpus()[index], {}, {}};
    if (p->unsupported) {
        unsupported_ = true;
        if (!o.unsupported || *o.unsupported != p->unsupported->reason) {
            auto x = terminate({MonitoringQ4CpuFreqCycleFailureKind::metadata, index + 1U});
            return std::unexpected{std::move(x.error())};
        }
        fact.unsupported = p->unsupported->reason;
    } else {
        if (p->source)
            sources_[index].emplace(std::move(*p->source));
        if (!sources_[index] || !o.domain_ordinal || *o.domain_ordinal == 0 ||
            *o.domain_ordinal > initial_->domains().size() ||
            !metadata_equal(sources_[index]->metadata(),
                            initial_->domains()[*o.domain_ordinal - 1U])) {
            auto x = terminate({MonitoringQ4CpuFreqCycleFailureKind::metadata, index + 1U});
            return std::unexpected{std::move(x.error())};
        }
        fact.domain_ordinal = *o.domain_ordinal;
    }
    preparations_.push_back(fact);
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    return fact;
}
auto MonitoringQ4CpuFreqCycleAuthority::finish_preparations() noexcept
    -> std::expected<bool, MonitoringQ4CpuFreqCycleFailure> {
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    if (stage_ != Stage::preparing || prepare_index_ != workspaces_.size())
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::inactive}};
    for (std::size_t i{}; i < preparations_.size(); ++i)
        if (preparations_[i].domain_ordinal) {
            const auto d = static_cast<std::size_t>(*preparations_[i].domain_ordinal - 1U);
            if (canonical_[d] == std::numeric_limits<std::size_t>::max())
                canonical_[d] = i;
        }
    MonitoringQ4CpuFreqCycleFailure retirement{MonitoringQ4CpuFreqCycleFailureKind::retirement};
    for (auto i = sources_.size(); i > 0; --i) {
        const auto n = i - 1;
        if (sources_[n] && canonical_[*preparations_[n].domain_ordinal - 1U] != n)
            retire_source(n, &retirement);
    }
    if (retirement.source) {
        auto x = terminate(std::move(retirement));
        return std::unexpected{std::move(x.error())};
    }
    for (std::size_t d{}; d < initial_->domains().size(); ++d)
        if (canonical_[d] != std::numeric_limits<std::size_t>::max())
            read_order_.push_back(canonical_[d]);
    if (unsupported_) {
        for (auto i = sources_.size(); i > 0; --i)
            retire_source(i - 1, &retirement);
        if (retirement.source) {
            auto x = terminate(std::move(retirement));
            return std::unexpected{std::move(x.error())};
        }
        stage_ = Stage::complete;
        if (auto guard = guard_cleanup_capacity(); !guard)
            return std::unexpected{std::move(guard.error())};
        return true;
    }
    stage_ = Stage::reads;
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    return false;
}
auto MonitoringQ4CpuFreqCycleAuthority::raw_next() noexcept
    -> std::expected<MonitoringQ4CpuFreqCycleReadFact, MonitoringQ4CpuFreqCycleFailure> {
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    if (stage_ != Stage::reads || expect_typed_ || round_ > rounds_ ||
        read_domain_ >= read_order_.size())
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::inactive}};
    const auto i = read_order_[read_domain_];
    auto v = sources_[i]->raw_agreement();
    if (!v) {
        auto x = terminate({MonitoringQ4CpuFreqCycleFailureKind::raw_read,
                            i + 1U,
                            round_,
                            read_domain_ + 1U,
                            {},
                            {},
                            std::move(v.error())});
        return std::unexpected{std::move(x.error())};
    }
    MonitoringQ4CpuFreqCycleReadFact f{round_, read_domain_ + 1U, *v};
    raw_.push_back(f);
    expect_typed_ = true;
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    return f;
}
auto MonitoringQ4CpuFreqCycleAuthority::typed_next() noexcept
    -> std::expected<MonitoringQ4CpuFreqCycleReadFact, MonitoringQ4CpuFreqCycleFailure> {
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    if (stage_ != Stage::reads || !expect_typed_)
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::inactive}};
    const auto i = read_order_[read_domain_];
    auto v = sources_[i]->typed_read();
    if (!v) {
        auto x = terminate({MonitoringQ4CpuFreqCycleFailureKind::typed_read,
                            i + 1U,
                            round_,
                            read_domain_ + 1U,
                            {},
                            {},
                            std::move(v.error())});
        return std::unexpected{std::move(x.error())};
    }
    MonitoringQ4CpuFreqCycleReadFact f{round_, read_domain_ + 1U, *v};
    typed_.push_back(f);
    expect_typed_ = false;
    if (++read_domain_ == read_order_.size()) {
        read_domain_ = 0;
        if (++round_ > rounds_)
            stage_ = Stage::final;
    }
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    return f;
}
auto MonitoringQ4CpuFreqCycleAuthority::final_discovery_and_compare() noexcept
    -> std::expected<void, MonitoringQ4CpuFreqCycleFailure> {
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    if (stage_ != Stage::final)
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::inactive}};
    auto s = discover(final_workspace_, final_inventory_,
                      MonitoringQ4CpuFreqCycleFailureKind::final_inventory,
                      MonitoringQ4CpuFreqCycleFailureKind::final_semantics);
    if (!s) {
        auto x = terminate(std::move(s.error()));
        return std::unexpected{std::move(x.error())};
    }
    if (!snapshots_equal(*initial_, *s)) {
        auto x = terminate({MonitoringQ4CpuFreqCycleFailureKind::semantic_drift});
        return std::unexpected{std::move(x.error())};
    }
    stage_ = Stage::retiring;
    retire_index_ = sources_.size();
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    return {};
}
auto MonitoringQ4CpuFreqCycleAuthority::retire_next() noexcept
    -> std::expected<bool, MonitoringQ4CpuFreqCycleFailure> {
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    if (stage_ != Stage::retiring || retire_index_ == 0)
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::inactive}};
    MonitoringQ4CpuFreqCycleFailure f{MonitoringQ4CpuFreqCycleFailureKind::retirement};
    do {
        --retire_index_;
        if (sources_[retire_index_]) {
            retire_source(retire_index_, &f);
            break;
        }
    } while (retire_index_ != 0);
    if (f.source) {
        auto x = terminate(std::move(f));
        return std::unexpected{std::move(x.error())};
    }
    while (retire_index_ != 0 && !sources_[retire_index_ - 1U])
        --retire_index_;
    if (retire_index_ == 0)
        stage_ = Stage::complete;
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    return stage_ == Stage::complete;
}
auto MonitoringQ4CpuFreqCycleAuthority::take_result() noexcept
    -> std::expected<MonitoringQ4CpuFreqCycleResult, MonitoringQ4CpuFreqCycleFailure> {
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::unexpected{std::move(guard.error())};
    if (stage_ != Stage::complete || !initial_)
        return std::unexpected{
            MonitoringQ4CpuFreqCycleFailure{MonitoringQ4CpuFreqCycleFailureKind::inactive}};
    stage_ = Stage::terminal;
    return MonitoringQ4CpuFreqCycleResult{unsupported_,
                                          std::move(*initial_),
                                          std::move(preparations_),
                                          std::move(raw_),
                                          std::move(typed_),
                                          cleanup_begin_,
                                          cleanup_ledger_ ? cleanup_ledger_->events.size()
                                                          : cleanup_begin_};
}
auto MonitoringQ4CpuFreqCycleAuthority::abort() noexcept
    -> std::optional<MonitoringQ4CpuFreqCycleFailure> {
    if (stage_ == Stage::terminal || stage_ == Stage::complete)
        return {};
    MonitoringQ4CpuFreqCycleFailure failure{MonitoringQ4CpuFreqCycleFailureKind::retirement};
    for (std::size_t index = sources_.size(); index != 0; --index)
        retire_source(index - 1U, &failure);
    stage_ = Stage::terminal;
    if (auto guard = guard_cleanup_capacity(); !guard)
        return std::move(guard.error());
    if (failure.source || !failure.cleanup.empty() || failure.cleanup_overflowed)
        return failure;
    return {};
}
auto MonitoringQ4CpuFreqCycleAuthority::execute(const MonitoringQ4SourceReady& r,
                                                MonitoringQ4CpuFreqCleanupLedger& ledger) noexcept
    -> std::expected<MonitoringQ4CpuFreqCycleResult, MonitoringQ4CpuFreqCycleFailure> {
    return execute_with(r, ledger, MonitoringQ4CpuFreqInventoryAuthority::native_operations());
}
auto MonitoringQ4CpuFreqCycleAuthority::execute_with(const MonitoringQ4SourceReady& r,
                                                     MonitoringQ4CpuFreqCleanupLedger& ledger,
                                                     Operations o) noexcept
    -> std::expected<MonitoringQ4CpuFreqCycleResult, MonitoringQ4CpuFreqCycleFailure> {
    auto c = create_with(r, ledger, o);
    if (!c)
        return std::unexpected{std::move(c.error())};
    auto i = c->initial_discovery();
    if (!i)
        return std::unexpected{std::move(i.error())};
    for (std::size_t n{}; n < r.inputs().admitted_cpus().size(); ++n) {
        auto p = c->prepare_next();
        if (!p)
            return std::unexpected{std::move(p.error())};
    }
    auto u = c->finish_preparations();
    if (!u)
        return std::unexpected{std::move(u.error())};
    if (!*u) {
        while (c->stage_ == Stage::reads) {
            auto a = c->raw_next();
            if (!a)
                return std::unexpected{std::move(a.error())};
            auto b = c->typed_next();
            if (!b)
                return std::unexpected{std::move(b.error())};
        }
        auto f = c->final_discovery_and_compare();
        if (!f)
            return std::unexpected{std::move(f.error())};
        while (c->stage_ == Stage::retiring) {
            auto x = c->retire_next();
            if (!x)
                return std::unexpected{std::move(x.error())};
        }
    }
    return c->take_result();
}
} // namespace epycbench::monitoring::qualification::detail
