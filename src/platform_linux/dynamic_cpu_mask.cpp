#include "epycbench/platform_linux/dynamic_cpu_mask.hpp"

#include "platform_linux/cpu_mask_allocator.hpp"

#include <climits>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::platform_linux {
namespace {

constexpr auto core_capacity_limit =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;

[[nodiscard]] auto mask_error(std::string_view code, std::string_view message,
                              std::string_view operation, std::string_view classification)
    -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{std::string{code}},
        .message = std::string{message},
        .operation = std::string{operation},
        .os_error = std::nullopt,
        .context = nlohmann::json{{"classification", classification}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto size_overflow(std::string_view operation, std::string_view classification)
    -> core::Error {
    return mask_error("linux.affinity_mask.size_overflow",
                      "Dynamic CPU mask size cannot be represented", operation, classification);
}

[[nodiscard]] auto allocation_failed(std::string_view operation) -> core::Error {
    return mask_error("linux.affinity_mask.allocation_failed",
                      "Dynamic CPU mask allocation failed", operation, "allocation_failure");
}

[[nodiscard]] auto cpu_out_of_range(std::string_view operation) -> core::Error {
    return mask_error("linux.affinity_mask.cpu_out_of_range",
                      "Dynamic CPU mask capacity is outside the Core CPU identity domain",
                      operation, "capacity_outside_core_domain");
}

[[nodiscard]] auto conversion_failed(std::string_view operation,
                                     std::string_view classification) -> core::Error {
    return mask_error("linux.affinity_mask.conversion_failed",
                      "Dynamic CPU mask conversion failed", operation, classification);
}

[[nodiscard]] auto contained_exception(std::string_view operation) -> core::Error {
    return mask_error("linux.affinity_mask.contained_exception",
                      "Dynamic CPU mask operation contained an exception", operation,
                      "contained_exception");
}

[[nodiscard]] auto contained_unknown_failure(std::string_view operation) -> core::Error {
    return mask_error("linux.affinity_mask.contained_unknown_failure",
                      "Dynamic CPU mask operation contained an unknown failure", operation,
                      "contained_unknown_failure");
}

[[nodiscard]] auto requested_capacity(const core::CpuSet& cpus)
    -> std::expected<std::size_t, core::Error> {
    if (cpus.empty()) {
        return std::size_t{1};
    }

    auto highest = std::uint32_t{0};
    for (const auto cpu : cpus) {
        highest = cpu.value();
    }
    const auto required = static_cast<std::uint64_t>(highest) + 1U;
    if (required > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{size_overflow("from_cpu_set", "highest_id_overflow")};
    }
    return static_cast<std::size_t>(required);
}

} // namespace

namespace detail {
namespace {

[[nodiscard]] auto production_allocate(void*, std::size_t capacity) -> cpu_set_t* {
    return CPU_ALLOC(capacity);
}

void production_release(void*, cpu_set_t* data) noexcept { CPU_FREE(data); }

[[nodiscard]] auto production_allocation_size(void*, std::size_t capacity) -> std::size_t {
    return CPU_ALLOC_SIZE(capacity);
}

} // namespace

auto production_cpu_mask_allocator() noexcept -> CpuMaskAllocator {
    return CpuMaskAllocator{
        .context = nullptr,
        .allocate = production_allocate,
        .release = production_release,
        .allocation_size = production_allocation_size,
    };
}

auto DynamicCpuMaskAccess::with_capacity(std::size_t minimum_capacity,
                                         const CpuMaskAllocator& allocator)
    -> std::expected<DynamicCpuMask, core::Error> {
    constexpr std::string_view operation = "with_capacity";
    const auto requested = minimum_capacity == 0 ? std::size_t{1} : minimum_capacity;
    if (static_cast<std::uint64_t>(requested) > core_capacity_limit) {
        return std::unexpected{cpu_out_of_range(operation)};
    }

    try {
        const auto native_size = allocator.allocation_size(allocator.context, requested);
        if (native_size == 0) {
            return std::unexpected{size_overflow(operation, "native_size_overflow")};
        }
        if (native_size > std::numeric_limits<std::size_t>::max() / CHAR_BIT) {
            return std::unexpected{size_overflow(operation, "native_size_overflow")};
        }
        const auto capacity = native_size * CHAR_BIT;
        if (capacity < requested) {
            return std::unexpected{size_overflow(operation, "native_size_overflow")};
        }
        if (static_cast<std::uint64_t>(capacity) > core_capacity_limit) {
            return std::unexpected{cpu_out_of_range(operation)};
        }

        auto* data = allocator.allocate(allocator.context, requested);
        if (data == nullptr) {
            return std::unexpected{allocation_failed(operation)};
        }
        DynamicCpuMask result{data, native_size, capacity, allocator.context, allocator.release};
        result.zero();
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected{allocation_failed(operation)};
    } catch (const std::exception&) {
        return std::unexpected{contained_exception(operation)};
    } catch (...) {
        return std::unexpected{contained_unknown_failure(operation)};
    }
}

} // namespace detail

DynamicCpuMask::DynamicCpuMask(cpu_set_t* data, std::size_t native_size, std::size_t capacity,
                               void* allocator_context,
                               ReleaseFunction release_function) noexcept
    : data_{data}, native_size_{native_size}, capacity_{capacity},
      allocator_context_{allocator_context}, release_{release_function} {}

DynamicCpuMask::DynamicCpuMask(DynamicCpuMask&& other) noexcept
    : data_{std::exchange(other.data_, nullptr)},
      native_size_{std::exchange(other.native_size_, 0)},
      capacity_{std::exchange(other.capacity_, 0)},
      allocator_context_{std::exchange(other.allocator_context_, nullptr)},
      release_{std::exchange(other.release_, nullptr)} {}

auto DynamicCpuMask::operator=(DynamicCpuMask&& other) noexcept -> DynamicCpuMask& {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        native_size_ = std::exchange(other.native_size_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
        allocator_context_ = std::exchange(other.allocator_context_, nullptr);
        release_ = std::exchange(other.release_, nullptr);
    }
    return *this;
}

DynamicCpuMask::~DynamicCpuMask() noexcept { release(); }

auto DynamicCpuMask::from_cpu_set(const core::CpuSet& cpus)
    -> std::expected<DynamicCpuMask, core::Error> {
    try {
        auto required = requested_capacity(cpus);
        if (!required) {
            return std::unexpected{std::move(required.error())};
        }
        auto result = with_capacity(*required);
        if (!result) {
            return std::unexpected{std::move(result.error())};
        }
        for (const auto cpu : cpus) {
            if (!result->set(cpu) || !result->contains(cpu)) {
                return std::unexpected{conversion_failed("from_cpu_set", "insertion_failed")};
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected{allocation_failed("from_cpu_set")};
    } catch (const std::exception&) {
        return std::unexpected{contained_exception("from_cpu_set")};
    } catch (...) {
        return std::unexpected{contained_unknown_failure("from_cpu_set")};
    }
}

auto DynamicCpuMask::with_capacity(std::size_t minimum_capacity)
    -> std::expected<DynamicCpuMask, core::Error> {
    return detail::DynamicCpuMaskAccess::with_capacity(
        minimum_capacity, detail::production_cpu_mask_allocator());
}

void DynamicCpuMask::zero() noexcept {
    if (data_ != nullptr) {
        CPU_ZERO_S(native_size_, data_);
    }
}

auto DynamicCpuMask::set(core::LinuxCpuId cpu) noexcept -> bool {
    const auto value = static_cast<std::size_t>(cpu.value());
    if (data_ == nullptr || value >= capacity_) {
        return false;
    }
    CPU_SET_S(value, native_size_, data_);
    return true;
}

auto DynamicCpuMask::clear(core::LinuxCpuId cpu) noexcept -> bool {
    const auto value = static_cast<std::size_t>(cpu.value());
    if (data_ == nullptr || value >= capacity_) {
        return false;
    }
    CPU_CLR_S(value, native_size_, data_);
    return true;
}

auto DynamicCpuMask::contains(core::LinuxCpuId cpu) const noexcept -> bool {
    const auto value = static_cast<std::size_t>(cpu.value());
    return data_ != nullptr && value < capacity_ && CPU_ISSET_S(value, native_size_, data_);
}

auto DynamicCpuMask::to_cpu_set() const -> std::expected<core::CpuSet, core::Error> {
    constexpr std::string_view operation = "to_cpu_set";
    if (data_ == nullptr || capacity_ == 0) {
        return core::CpuSet{};
    }
    if (static_cast<std::uint64_t>(capacity_) > core_capacity_limit) {
        return std::unexpected{cpu_out_of_range(operation)};
    }

    try {
        std::string ranges;
        std::optional<std::uint32_t> first;
        std::uint32_t last = 0;
        const auto flush = [&]() {
            if (!first) {
                return;
            }
            if (!ranges.empty()) {
                ranges.push_back(',');
            }
            ranges += std::to_string(*first);
            if (*first != last) {
                ranges.push_back('-');
                ranges += std::to_string(last);
            }
            first.reset();
        };

        for (std::size_t index = 0; index < capacity_; ++index) {
            if (CPU_ISSET_S(index, native_size_, data_)) {
                const auto value = static_cast<std::uint32_t>(index);
                if (!first) {
                    first = value;
                }
                last = value;
            } else {
                flush();
            }
        }
        flush();

        if (ranges.empty()) {
            return core::CpuSet{};
        }
        auto converted = core::CpuSet::parse(ranges);
        if (!converted) {
            return std::unexpected{conversion_failed(operation, "conversion_failed")};
        }
        return std::move(*converted);
    } catch (const std::bad_alloc&) {
        return std::unexpected{allocation_failed(operation)};
    } catch (const std::exception&) {
        return std::unexpected{contained_exception(operation)};
    } catch (...) {
        return std::unexpected{contained_unknown_failure(operation)};
    }
}

auto DynamicCpuMask::native_view() noexcept -> NativeCpuMaskView {
    return NativeCpuMaskView{.data = data_, .size = native_size_};
}

auto DynamicCpuMask::native_view() const noexcept -> ConstNativeCpuMaskView {
    return ConstNativeCpuMaskView{.data = data_, .size = native_size_};
}

auto DynamicCpuMask::native_size() const noexcept -> std::size_t { return native_size_; }

auto DynamicCpuMask::capacity() const noexcept -> std::size_t { return capacity_; }

void DynamicCpuMask::release() noexcept {
    if (data_ != nullptr && release_ != nullptr) {
        release_(allocator_context_, data_);
    }
    data_ = nullptr;
    native_size_ = 0;
    capacity_ = 0;
    allocator_context_ = nullptr;
    release_ = nullptr;
}

} // namespace epycbench::platform_linux
