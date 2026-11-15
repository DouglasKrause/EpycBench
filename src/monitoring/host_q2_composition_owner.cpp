#include "monitoring/host_q2_composition_owner.hpp"
#include "monitoring/host_q2_complete_q1_predecessor_authority.hpp"
#include "monitoring/host_q2_plan_public_input_authority.hpp"
#include "monitoring/host_running_build_authority.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2CompositionOwner::Impl final { public: MonitoringQ1BoundedRetirement cleanup;
 std::optional<MonitoringQ1PlanV2AuthoritySession> q1;std::optional<MonitoringQ2PlanPublicInputAuthority> input;
 std::optional<MonitoringQ2CompleteQ1PredecessorAuthority> predecessor;std::optional<MonitoringRunningBuildAuthority> build;
 void* q1_test_context{};void (*q1_test_retire)(void*) noexcept{}; };
MonitoringQ2CompositionOwner::MonitoringQ2CompositionOwner(std::unique_ptr<Impl> value) noexcept:impl_{std::move(value)}{}
MonitoringQ2CompositionOwner::MonitoringQ2CompositionOwner(MonitoringQ2CompositionOwner&&) noexcept=default;
MonitoringQ2CompositionOwner::~MonitoringQ2CompositionOwner() noexcept=default;
void MonitoringQ2CompositionOwner::adopt_q1(MonitoringQ1PlanV2AuthoritySession value) noexcept{impl_->q1.emplace(std::move(value));}
void MonitoringQ2CompositionOwner::adopt_q1_test(void* context,void (*retire)(void*) noexcept) noexcept{impl_->q1_test_context=context;impl_->q1_test_retire=retire;}
auto MonitoringQ2CompositionOwner::q1() noexcept->MonitoringQ1PlanV2AuthoritySession&{if(!impl_->q1)std::terminate();return *impl_->q1;}
auto MonitoringQ2CompositionOwner::take_q1() noexcept->MonitoringQ1PlanV2AuthoritySession{auto v=std::move(*impl_->q1);impl_->q1.reset();return v;}
void MonitoringQ2CompositionOwner::adopt_input(MonitoringQ2PlanPublicInputAuthority v) noexcept{impl_->input.emplace(std::move(v));}
void MonitoringQ2CompositionOwner::adopt_predecessor(MonitoringQ2CompleteQ1PredecessorAuthority v) noexcept{impl_->predecessor.emplace(std::move(v));}
void MonitoringQ2CompositionOwner::adopt_build(MonitoringRunningBuildAuthority v) noexcept{impl_->build.emplace(std::move(v));}
auto MonitoringQ2CompositionOwner::input() noexcept->MonitoringQ2PlanPublicInputAuthority&{return *impl_->input;}
auto MonitoringQ2CompositionOwner::predecessor() noexcept->MonitoringQ2CompleteQ1PredecessorAuthority&{return *impl_->predecessor;}
auto MonitoringQ2CompositionOwner::build() noexcept->MonitoringRunningBuildAuthority&{return *impl_->build;}
auto MonitoringQ2CompositionOwner::take_input() noexcept->MonitoringQ2PlanPublicInputAuthority{auto v=std::move(*impl_->input);impl_->input.reset();return v;}
auto MonitoringQ2CompositionOwner::take_predecessor() noexcept->MonitoringQ2CompleteQ1PredecessorAuthority{auto v=std::move(*impl_->predecessor);impl_->predecessor.reset();return v;}
auto MonitoringQ2CompositionOwner::take_build() noexcept->MonitoringRunningBuildAuthority{auto v=std::move(*impl_->build);impl_->build.reset();return v;}
auto MonitoringQ2CompositionOwner::take_cleanup_ledger() noexcept->MonitoringQ1BoundedRetirement{return std::move(impl_->cleanup);}
auto MonitoringQ2CompositionOwner::owns_q1() const noexcept->bool{return impl_&&(impl_->q1.has_value()||impl_->q1_test_retire);}
auto MonitoringQ2CompositionOwner::rollback(core::Error primary) noexcept->MonitoringQ2CompositionFailure{
 MonitoringQ2CompositionFailure result{std::move(primary),std::move(impl_->cleanup)};
 const auto retain=[&](core::Error value) noexcept {if(result.cleanup.count<result.cleanup.failures.size())result.cleanup.failures[result.cleanup.count++].emplace(std::move(value));else result.cleanup.overflowed=true;};
 if(impl_->build){auto r=impl_->build->retire();if(!r)retain(std::move(r.error()));impl_->build.reset();}
 if(impl_->predecessor){auto r=impl_->predecessor->retire();if(!r){retain(std::move(r.error().primary));for(std::size_t i{};i<r.error().cleanup_count;++i)retain(std::move(*r.error().cleanup[i]));}impl_->predecessor.reset();}
 if(impl_->input){auto r=impl_->input->retire();if(!r){retain(std::move(r.error().primary));for(auto& e:r.error().cleanup)retain(std::move(e));}impl_->input.reset();}
 if(impl_->q1){auto cleanup=impl_->q1->retire_before_binding();for(std::size_t index{};index<cleanup.count;++index)retain(std::move(*cleanup.failures[index]));result.cleanup.overflowed=result.cleanup.overflowed||cleanup.overflowed;impl_->q1.reset();}
 if(impl_->q1_test_retire){impl_->q1_test_retire(impl_->q1_test_context);impl_->q1_test_retire=nullptr;impl_->q1_test_context=nullptr;}
 return result;
}
auto MonitoringQ2CompositionOwner::rollback(MonitoringQ1GitCommandFailure value) noexcept->MonitoringQ2CompositionFailure{
 for(auto& item:value.cleanup){if(impl_->cleanup.count<impl_->cleanup.failures.size())impl_->cleanup.failures[impl_->cleanup.count++].emplace(std::move(item));else impl_->cleanup.overflowed=true;}
 return rollback(std::move(value.primary));
}
void MonitoringQ2CompositionOwnerTestAccess::seed_cleanup(MonitoringQ2CompositionOwner& owner,core::Error value) noexcept {
 auto& cleanup=owner.impl_->cleanup;if(cleanup.count<cleanup.failures.size())cleanup.failures[cleanup.count++].emplace(std::move(value));else cleanup.overflowed=true;
}
void MonitoringQ2CompositionOwnerTestAccess::note_cleanup_overflow(MonitoringQ2CompositionOwner& owner) noexcept {owner.impl_->cleanup.overflowed=true;}
auto MonitoringQ2CompositionOwner::create() noexcept
 ->std::expected<MonitoringQ2CompositionOwner,MonitoringQ1GitCommandFailure>{
 try{auto value=std::make_unique<Impl>();return MonitoringQ2CompositionOwner{std::move(value)};}
 catch(...){core::Error error{};error.category=core::ErrorCategory::monitoring;
  error.continuation=core::ContinuationPolicy::abort_command;return std::unexpected{MonitoringQ1GitCommandFailure{std::move(error),{}}};}
}
}
