#include "results/authored_source.hpp"
#include "core/json_ownership.hpp"
#include "results/authored_source_codes.hpp"
#include "results/authored_source_plan.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace epycbench::results::authored_source {
namespace {
auto error(std::string_view code, std::string_view operation) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.authored_source." + std::string{code}},
            .message = "Authored source ownership rejected",
            .operation = std::string{operation},
            .os_error = std::nullopt,
            .context = nlohmann::json::object()};
}
void valid_limits(Limits l) {
    const auto max = std::string{}.max_size();
    for (auto n : {l.source_bytes, l.name_bytes, l.override_count, l.override_bytes, l.diagnostics,
                   l.canonical_bytes, l.nodes, l.total_bytes})
        if (n > max)
            throw std::length_error{"limits"};
}
bool equal_plan(const scenario::ScenarioDigestedPlan& a, const scenario::ScenarioDigestedPlan& b) {
    const auto& x = detail::authored(a);
    const auto& y = detail::authored(b);
    return a.scenario_digest() == b.scenario_digest() &&
           a.resolved_scenario_plan_digest() == b.resolved_scenario_plan_digest() &&
           a.canonical_scenario_json() == b.canonical_scenario_json() &&
           a.canonical_resolved_scenario_plan_json() == b.canonical_resolved_scenario_plan_json() &&
           x.value() == y.value() && x.source_name() == y.source_name() &&
           x.locations() == y.locations();
}
} // namespace
struct SourceBundle::Storage {
    Availability availability;
    std::string bytes, name;
    scenario::ScenarioFormat format;
    std::vector<std::string> overrides;
    std::vector<std::string_view> views;
    std::optional<FailureReference> failure;
    std::size_t logical;
    Storage() = default;
    Storage(const Storage& s)
        : availability(s.availability), bytes(s.bytes), name(s.name), format(s.format),
          overrides(s.overrides), failure(s.failure), logical(s.logical) {
        views.reserve(overrides.size());
        for (const auto& v : overrides)
            views.push_back(v);
    }
};
SourceBundle::SourceBundle(std::unique_ptr<Storage> s) : storage_(std::move(s)) {}
SourceBundle::SourceBundle(const SourceBundle& s)
    : storage_(s.storage_ ? std::make_unique<Storage>(*s.storage_) : nullptr) {}
auto SourceBundle::operator=(const SourceBundle& s) -> SourceBundle& {
    SourceBundle copy{s};
    storage_.swap(copy.storage_);
    return *this;
}
SourceBundle::SourceBundle(SourceBundle&&) noexcept = default;
auto SourceBundle::operator=(SourceBundle&&) noexcept -> SourceBundle& = default;
SourceBundle::~SourceBundle() = default;
auto SourceBundle::input() const noexcept -> Input {
    if (!storage_)
        return {Availability::not_attempted,    {}, {},
                scenario::ScenarioFormat::json, {}, std::nullopt};
    return {storage_->availability, storage_->bytes, storage_->name,
            storage_->format,       storage_->views, storage_->failure};
}
auto SourceBundle::logical_bytes() const noexcept -> std::size_t {
    return storage_ ? storage_->logical : 0;
}
auto retain(Input in, Limits limits) -> std::expected<SourceBundle, core::Error> {
    try {
        valid_limits(limits);
    } catch (const std::length_error&) {
        return std::unexpected{error("invalid_limits", "retain_authored_source")};
    }
    if (in.format != scenario::ScenarioFormat::json && in.format != scenario::ScenarioFormat::yaml)
        return std::unexpected{error("invalid_format", "retain_authored_source")};
    if ((in.availability != Availability::available &&
         in.availability != Availability::not_attempted &&
         in.availability != Availability::not_obtained) ||
        (in.availability != Availability::available &&
         (!in.bytes.empty() || !in.name.empty() || !in.rejected_overrides.empty())) ||
        ((in.availability == Availability::not_obtained) != in.acquisition_failure.has_value()))
        return std::unexpected{error("internal_failure", "retain_authored_source")};
    try {
        if (in.bytes.size() > limits.source_bytes || in.name.size() > limits.name_bytes ||
            in.rejected_overrides.size() > limits.override_count)
            throw detail::LimitFailure{};
        detail::Counter c{limits};
        c.add(sizeof(SourceBundle));
        c.add(sizeof(SourceBundle::Storage));
        c.text(in.bytes);
        c.text(in.name);
        c.elements(in.rejected_overrides.size(), sizeof(std::string) + sizeof(std::string_view));
        std::size_t override_bytes = 0;
        for (auto v : in.rejected_overrides) {
            if (v.size() > limits.override_bytes - override_bytes)
                throw detail::LimitFailure{};
            override_bytes += v.size();
            c.text(v);
        }
        auto s = std::make_unique<SourceBundle::Storage>();
        s->availability = in.availability;
        s->bytes = in.bytes;
        s->name = in.name;
        s->format = in.format;
        s->failure = in.acquisition_failure;
        s->logical = c.used.bytes;
        s->overrides.reserve(in.rejected_overrides.size());
        s->views.reserve(in.rejected_overrides.size());
        for (auto v : in.rejected_overrides)
            s->overrides.emplace_back(v);
        for (const auto& v : s->overrides)
            s->views.push_back(v);
        return SourceBundle{std::move(s)};
    } catch (const detail::LimitFailure&) {
        return std::unexpected{error("capacity", "retain_authored_source")};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::length_error&) {
        return std::unexpected{error("resource_failure", "retain_authored_source")};
    }
}
struct ResolutionRecord::Storage {
    SourceBundle source;
    State state{State::not_attempted};
    std::optional<scenario::ScenarioDigestedPlan> plan;
    std::vector<SafeDiagnostic> diagnostics;
    std::optional<private_error::Owner> upstream;
    Usage usage{};
    explicit Storage(SourceBundle s) : source(std::move(s)) {}
};
ResolutionRecord::ResolutionRecord(std::unique_ptr<Storage> s) : storage_(std::move(s)) {}
ResolutionRecord::ResolutionRecord(ResolutionRecord&&) noexcept = default;
auto ResolutionRecord::operator=(ResolutionRecord&&) noexcept -> ResolutionRecord& = default;
ResolutionRecord::~ResolutionRecord() = default;
auto ResolutionRecord::source() const noexcept -> const SourceBundle* {
    return storage_ ? &storage_->source : nullptr;
}
auto ResolutionRecord::state() const noexcept -> State {
    return storage_ ? storage_->state : State::not_attempted;
}
auto ResolutionRecord::plan() const noexcept -> const scenario::ScenarioDigestedPlan* {
    return storage_ && storage_->plan ? &*storage_->plan : nullptr;
}
auto ResolutionRecord::diagnostics() const noexcept -> std::span<const SafeDiagnostic> {
    return storage_ ? std::span<const SafeDiagnostic>{storage_->diagnostics}
                    : std::span<const SafeDiagnostic>{};
}
auto ResolutionRecord::upstream_error() const noexcept -> const private_error::Owner* {
    return storage_ && storage_->upstream ? &*storage_->upstream : nullptr;
}
auto ResolutionRecord::usage() const noexcept -> Usage {
    return storage_ ? storage_->usage : Usage{};
}
auto resolve(SourceBundle source, const scenario::ResourceResolutionContext& context, Limits limits,
             std::optional<scenario::ScenarioResolutionLimits> resolution_limits,
             const scenario::ScenarioDigestedPlan* expected)
    -> std::expected<ResolutionRecord, core::Error> {
    try {
        valid_limits(limits);
    } catch (const std::length_error&) {
        return std::unexpected{error("invalid_limits", "resolve_retained_source")};
    }
    try {
        const auto retained = source.input();
        if (retained.bytes.size() > limits.source_bytes ||
            retained.name.size() > limits.name_bytes ||
            retained.rejected_overrides.size() > limits.override_count)
            throw detail::LimitFailure{};
        std::size_t override_bytes = 0;
        for (auto token : retained.rejected_overrides) {
            if (token.size() > limits.override_bytes - override_bytes)
                throw detail::LimitFailure{};
            override_bytes += token.size();
        }
        detail::Counter c{limits};
        c.add(sizeof(ResolutionRecord));
        c.add(sizeof(ResolutionRecord::Storage));
        c.add(source.logical_bytes());
        if (expected) {
            auto expected_count = c;
            detail::count_plan(expected_count, *expected);
        }
        auto out = std::make_unique<ResolutionRecord::Storage>(std::move(source));
        const auto in = out->source.input();
        auto diagnostic = [&](std::string_view code) {
            if (out->diagnostics.size() == limits.diagnostics)
                throw detail::LimitFailure{};
            c.add(sizeof(SafeDiagnostic));
            out->diagnostics.push_back({detail::safe_code(code)});
        };
        if (in.availability == Availability::not_attempted)
            out->state = State::not_attempted;
        else if (in.availability == Availability::not_obtained)
            out->state = State::source_unavailable;
        else if (!in.rejected_overrides.empty())
            out->state = State::override_rejected;
        else {
            if (limits.nodes == 0)
                throw detail::LimitFailure{};
            auto result = resolution_limits
                ? scenario::resolve_scenario({in.name, in.bytes, in.format}, context, *resolution_limits)
                : scenario::resolve_scenario({in.name, in.bytes, in.format}, context);
            core::detail::ExpectedErrorCleanup result_cleanup{result};
            if (!result) {
                diagnostic(result.error().code.value());
                auto available = limits.total_bytes - c.used.bytes;
                if (!private_error::detail::checked_add(available, sizeof(private_error::Owner)))
                    throw detail::LimitFailure{};
                const private_error::Limits capture_limits{
                    std::min(limits.nodes - c.used.nodes, nlohmann::json::array_t{}.max_size()),
                    64,
                    std::min(available, std::string{}.max_size()),
                    std::min(available, nlohmann::json::binary_t{}.max_size()),
                    available};
                auto captured = private_error::retain(result.error(), capture_limits);
                core::detail::ExpectedErrorCleanup captured_cleanup{captured};
                if (!captured) {
                    if (captured.error().code.value() == "results.private_error.capacity")
                        throw detail::LimitFailure{};
                    return std::unexpected{error("internal_failure", "resolve_retained_source")};
                }
                const auto stock = captured->usage();
                if (stock.retained_bytes < sizeof(private_error::Owner) ||
                    stock.nodes > limits.nodes - c.used.nodes)
                    throw detail::LimitFailure{};
                c.add(stock.retained_bytes - sizeof(private_error::Owner));
                c.used.nodes += stock.nodes;
                out->upstream.emplace(std::move(*captured));
                out->state = State::resolution_failed;
            } else {
                if (!result->scenario) {
                    out->state = State::scenario_rejected;
                    for (const auto& d : result->diagnostics)
                        diagnostic(d.code.value());
                } else if (expected && !equal_plan(*result->scenario, *expected))
                    out->state = State::expected_plan_mismatch;
                else {
                    detail::count_plan(c, *result->scenario);
                    out->plan = std::move(result->scenario);
                    result->scenario.reset();
                    out->state = State::resolved;
                }
            }
        }
        out->usage = c.used;
        return ResolutionRecord{std::move(out)};
    } catch (const detail::LimitFailure&) {
        return std::unexpected{error("capacity", "resolve_retained_source")};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::length_error&) {
        return std::unexpected{error("resource_failure", "resolve_retained_source")};
    } catch (...) {
        return std::unexpected{error("internal_failure", "resolve_retained_source")};
    }
}
auto resolve(SourceBundle source, const scenario::ResourceResolutionContext& context, Limits limits,
             const scenario::ScenarioDigestedPlan* expected)
    -> std::expected<ResolutionRecord, core::Error> {
    return resolve(std::move(source), context, limits, std::nullopt, expected);
}

} // namespace epycbench::results::authored_source
