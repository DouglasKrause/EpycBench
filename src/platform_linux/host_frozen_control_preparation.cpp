#include "platform_linux/host_frozen_control_preparation.hpp"

#include <cerrno>
#include <exception>
#include <limits>
#include <sys/mman.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace epycbench::platform_linux::host_work {
namespace {
constexpr std::size_t stack_bytes = 4U * 1024U * 1024U;
auto bytes(std::uint64_t value) noexcept -> core::ByteCount {
    return core::ByteCount::from_base_units(value);
}
auto thread_id() noexcept -> std::uint64_t {
    const auto value = ::syscall(SYS_gettid);
    return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}
} // namespace
auto frozen_control_raw_now() noexcept -> std::expected<std::uint64_t, ControlFailure> {
    timespec time{};
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &time) != 0)
        return std::unexpected{ControlFailure{ControlOperation::clock, errno}};
    if (time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1000000000L ||
        static_cast<std::uint64_t>(time.tv_sec) > (UINT64_MAX - static_cast<std::uint64_t>(time.tv_nsec)) / UINT64_C(1000000000))
        return std::unexpected{ControlFailure{ControlOperation::clock, EOVERFLOW}};
    return static_cast<std::uint64_t>(time.tv_sec) * UINT64_C(1000000000) + static_cast<std::uint64_t>(time.tv_nsec);
}
FrozenControlPreparation::~FrozenControlPreparation() noexcept {
    // Destruction cannot certify retirement of borrowed stack backing.
    if (!retired()) std::terminate();
}
auto FrozenControlPreparation::fail(ControlOperation operation, int native) noexcept
    -> std::unexpected<ControlFailure> {
    if (!failure_) failure_ = ControlFailure{operation, native};
    return std::unexpected{*failure_};
}
auto FrozenControlPreparation::on_original_main() const noexcept -> bool {
    return main_id_ && thread_id() == main_id_;
}
auto FrozenControlPreparation::on_control_thread() const noexcept -> bool {
    return control_id_ && thread_id() == control_id_;
}
auto FrozenControlPreparation::control_fail(ControlOperation operation, int native) noexcept
    -> std::unexpected<ControlFailure> {
    if (!control_failure_) control_failure_ = ControlFailure{operation, native};
    return std::unexpected{*control_failure_};
}
void FrozenControlPreparation::cleanup_failure(ControlOperation operation, int native) noexcept {
    if (!failure_) { failure_ = ControlFailure{operation, native}; return; }
    if (cleanup_count_ < cleanup_.size()) cleanup_[cleanup_count_++] = {operation, native};
    else std::terminate(); // The finite owner has at most three fallible retirements per attempt.
}
auto FrozenControlPreparation::capture_original_main(ResourceLedger& ledger, std::size_t capacity) noexcept
    -> std::expected<void, ControlFailure> {
    if (failure_ || captured_ || masks_lease_ || !capacity || capacity > UINT32_MAX)
        return fail(ControlOperation::state);
    main_id_ = thread_id();
    if (!main_id_) return fail(ControlOperation::identity, errno);
    if (main_id_ != static_cast<std::uint64_t>(::getpid())) return fail(ControlOperation::identity);
    // CPU_ALLOC_SIZE rounds to native words. Bound arithmetic before acquisition.
    if (capacity > std::numeric_limits<std::size_t>::max() - 63U)
        return fail(ControlOperation::capacity);
    mask_bytes_ = CPU_ALLOC_SIZE(capacity);
    if (!mask_bytes_ || mask_bytes_ > UINT64_MAX / masks_.size())
        return fail(ControlOperation::capacity);
    Resources request{};
    request.ordinary_bytes = bytes(mask_bytes_ * masks_.size());
    request.address_space_bytes = request.ordinary_bytes;
    auto reserved = ledger.reserve(request);
    if (!reserved) return fail(ControlOperation::reservation);
    masks_lease_.emplace(std::move(*reserved));
    capacity_ = capacity;
    for (auto& mask : masks_) {
        mask = CPU_ALLOC(capacity); // Register before any other fallible operation.
        if (!mask) return fail(ControlOperation::allocation, ENOMEM);
        CPU_ZERO_S(mask_bytes_, mask);
    }
    if (::sched_getaffinity(0, mask_bytes_, masks_[0]) != 0)
        return fail(ControlOperation::affinity_capture, errno);
    if (CPU_COUNT_S(mask_bytes_, masks_[0]) == 0) return fail(ControlOperation::affinity_capture);
    captured_ = true;
    return check_original_main();
}
auto FrozenControlPreparation::check_original_main() noexcept -> std::expected<void, ControlFailure> {
    if (!captured_ || thread_id() != main_id_) return fail(ControlOperation::identity);
    CPU_ZERO_S(mask_bytes_, masks_[1]);
    if (::sched_getaffinity(0, mask_bytes_, masks_[1]) != 0)
        return fail(ControlOperation::affinity_check, errno);
    if (!CPU_EQUAL_S(mask_bytes_, masks_[0], masks_[1]))
        return fail(ControlOperation::affinity_changed);
    return {};
}
auto FrozenControlPreparation::prepare_control(ResourceLedger& ledger, core::LinuxCpuId cpu) noexcept
    -> std::expected<void, ControlFailure> {
    if (failure_ || !captured_ || control_lease_ || cpu.value() >= capacity_ ||
        !CPU_ISSET_S(cpu.value(), mask_bytes_, masks_[0])) return fail(ControlOperation::state);
    const auto page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0 || static_cast<std::uint64_t>(page) > (8388608U - stack_bytes) / 2U)
        return fail(ControlOperation::page_size, errno);
    page_bytes_ = static_cast<std::size_t>(page);
    if (stack_bytes % page_bytes_ != 0) return fail(ControlOperation::page_size);
    mapping_bytes_ = stack_bytes + 2U * page_bytes_;
    Resources request{0U, 1U, 0U, 1U, 0U};
    // This mmap is a stack envelope, charged once in the stack category.
    request.stack_bytes = bytes(mapping_bytes_);
    request.address_space_bytes = request.stack_bytes;
    auto reserved = ledger.reserve(request);
    if (!reserved) return fail(ControlOperation::reservation);
    control_lease_.emplace(std::move(*reserved));
    void* acquired = ::mmap(nullptr, mapping_bytes_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (acquired == MAP_FAILED) return fail(ControlOperation::mapping, errno);
    mapping_ = acquired;
    mapping_owned_ = true;
    auto* usable = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(mapping_) + page_bytes_);
    if (::mprotect(usable, stack_bytes, PROT_READ | PROT_WRITE) != 0)
        return fail(ControlOperation::protection, errno);
    int result = ::pthread_attr_init(&attributes_);
    if (result != 0) return fail(ControlOperation::attr_init, result);
    attributes_owned_ = true;
    result = ::pthread_attr_setdetachstate(&attributes_, PTHREAD_CREATE_JOINABLE);
    if (result != 0) return fail(ControlOperation::attr_joinable, result);
    result = ::pthread_attr_setguardsize(&attributes_, 0U);
    if (result != 0) return fail(ControlOperation::attr_guard, result);
    result = ::pthread_attr_setstack(&attributes_, usable, stack_bytes);
    if (result != 0) return fail(ControlOperation::attr_stack, result);
    control_cpu_ = cpu;
    return {};
}
auto FrozenControlPreparation::attributes() const noexcept -> const pthread_attr_t* {
    return attributes_owned_ && !failure_ ? &attributes_ : nullptr;
}
auto FrozenControlPreparation::pin_and_observe_control() noexcept -> std::expected<void, ControlFailure> {
    if (failure_ || !mapping_owned_ || !attributes_owned_ || control_id_)
        return control_fail(ControlOperation::state);
    control_id_ = thread_id();
    if (!control_id_ || control_id_ == main_id_) return control_fail(ControlOperation::identity);
    CPU_ZERO_S(mask_bytes_, masks_[2]);
    CPU_SET_S(control_cpu_.value(), mask_bytes_, masks_[2]);
    if (::sched_setaffinity(0, mask_bytes_, masks_[2]) != 0)
        return control_fail(ControlOperation::affinity_set, errno);
    // Register the readback attribute owner immediately on a successful acquisition.
    int result = ::pthread_getattr_np(::pthread_self(), &observed_attributes_);
    if (result != 0) return control_fail(ControlOperation::stack_observation, result);
    observed_attributes_owned_ = true;
    void* actual{};
    std::size_t size{};
    int joinability{};
    result = ::pthread_attr_getstack(&observed_attributes_, &actual, &size);
    if (result != 0) return control_fail(ControlOperation::stack_observation, result);
    result = ::pthread_attr_getdetachstate(&observed_attributes_, &joinability);
    if (result != 0) return control_fail(ControlOperation::stack_observation, result);
    if (actual != reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(mapping_) + page_bytes_) || size != stack_bytes ||
        joinability != PTHREAD_CREATE_JOINABLE) return control_fail(ControlOperation::stack_observation);
    result = ::pthread_attr_destroy(&observed_attributes_);
    if (result != 0) return control_fail(ControlOperation::attr_destroy, result);
    observed_attributes_owned_ = false;
    return check_control();
}
auto FrozenControlPreparation::check_control() noexcept -> std::expected<void, ControlFailure> {
    if (control_failure_) return std::unexpected{*control_failure_};
    if (!control_id_ || thread_id() != control_id_) return control_fail(ControlOperation::identity);
    CPU_ZERO_S(mask_bytes_, masks_[2]);
    if (::sched_getaffinity(0, mask_bytes_, masks_[2]) != 0)
        return control_fail(ControlOperation::affinity_check, errno);
    if (CPU_COUNT_S(mask_bytes_, masks_[2]) != 1 ||
        !CPU_ISSET_S(control_cpu_.value(), mask_bytes_, masks_[2]))
        return control_fail(ControlOperation::affinity_changed);
    const int cpu = ::sched_getcpu();
    if (cpu < 0) return control_fail(ControlOperation::current_cpu, errno);
    if (static_cast<std::uint32_t>(cpu) != control_cpu_.value()) return control_fail(ControlOperation::current_cpu);
    return {};
}
auto FrozenControlPreparation::retire() noexcept -> bool {
    if (retired()) return true;
    // Two complete attempts have at most six secondary facts. Exhaustion keeps
    // the remaining native owner and lease; it never attempts another cleanup.
    if (retirement_attempts_ >= 2U) return false;
    ++retirement_attempts_;
    if (observed_attributes_owned_) {
        const int result = ::pthread_attr_destroy(&observed_attributes_);
        if (result != 0) cleanup_failure(ControlOperation::attr_destroy, result);
        else observed_attributes_owned_ = false;
    }
    if (attributes_owned_) {
        const int result = ::pthread_attr_destroy(&attributes_);
        if (result != 0) cleanup_failure(ControlOperation::attr_destroy, result);
        else attributes_owned_ = false;
    }
    if (mapping_owned_) {
        if (::munmap(mapping_, mapping_bytes_) != 0) cleanup_failure(ControlOperation::unmap, errno);
        else { mapping_ = nullptr; mapping_owned_ = false; }
    }
    if (!observed_attributes_owned_ && !attributes_owned_ && !mapping_owned_ && control_lease_) {
        if (!control_lease_->retire()) std::terminate();
        control_lease_.reset();
    }
    for (auto& mask : masks_) { if (mask) CPU_FREE(mask); mask = nullptr; }
    if (masks_lease_) {
        if (!masks_lease_->retire()) std::terminate();
        masks_lease_.reset();
    }
    return retired();
}
auto FrozenControlPreparation::retired() const noexcept -> bool {
    return !observed_attributes_owned_ && !attributes_owned_ && !mapping_owned_ &&
        !control_lease_ && !masks_lease_;
}
} // namespace epycbench::platform_linux::host_work
