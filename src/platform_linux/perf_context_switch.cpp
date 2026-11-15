#include "epycbench/platform_linux/perf_context_switch.hpp"

#include "platform_linux/perf_context_switch_access.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <linux/perf_event.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux {
namespace {

[[maybe_unused]] constexpr std::array pairwise_epoch_failure_source_anchors{
    "linux.perf_context_switch.epoch_control_failed",
    "linux.perf_context_switch.epoch_read_failed",
    "linux.perf_context_switch.epoch_invalid",
    "linux.perf_context_switch.epoch_loss"};

[[maybe_unused]] constexpr auto pairwise_metadata_capacity_source_anchor =
    "linux.perf_context_switch.metadata_capacity_exceeded";

auto perf_error(std::string_view code, std::string_view operation, std::string_view classification,
                std::optional<int> os = std::nullopt) -> core::Error {
    std::optional<core::OsError> os_error;
    if (os && *os != 0)
        os_error = core::OsError::capture(std::error_code{*os, std::generic_category()});
    return {.category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Linux perf context-switch operation failed",
            .operation = std::string{operation},
            .os_error = std::move(os_error),
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto q2_native_error(std::string_view code, std::string_view operation,
                     std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Linux perf native operation failed",
            .operation = std::string{operation},
            .os_error = core::OsError{0, "linux-errno", "native errno captured"},
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

template <class T> auto read_wrapped(std::span<const std::byte> ring, std::uint64_t position) -> T {
    T value{};
    auto* output = reinterpret_cast<std::byte*>(&value);
    for (std::size_t index = 0; index < sizeof(T); ++index)
        output[index] = ring[(position + index) % ring.size()];
    return value;
}

} // namespace

PerfContextSwitchDrain::PerfContextSwitchDrain(std::vector<PerfContextSwitchRecord> records,
                                               PerfCaptureCompleteness completeness) noexcept
    : records_{std::move(records)}, completeness_{completeness} {}
auto PerfContextSwitchDrain::create(std::vector<PerfContextSwitchRecord> records,
                                    PerfCaptureCompleteness completeness)
    -> PerfContextSwitchDrain {
    return {std::move(records), completeness};
}
auto PerfContextSwitchDrain::records() const noexcept -> std::span<const PerfContextSwitchRecord> {
    return records_;
}
auto PerfContextSwitchDrain::completeness() const noexcept -> PerfCaptureCompleteness {
    return completeness_;
}

struct PerfContextSwitchSource::State {
    int descriptor{-1};
    void* mapping{MAP_FAILED};
    std::size_t mapping_size{};
    std::size_t page_size{};
    detail::PerfContextSwitchOperations operations{};

    ~State() noexcept {
        if (mapping != MAP_FAILED)
            operations.unmap(operations.context, mapping, mapping_size);
        if (descriptor >= 0)
            operations.close(operations.context, descriptor);
    }
};

auto PerfContextSwitchSource::configuration() noexcept -> PerfContextSwitchConfiguration {
    return {.type = PERF_TYPE_SOFTWARE,
            .size = sizeof(perf_event_attr),
            .config = PERF_COUNT_SW_DUMMY,
            .sample_type = PERF_SAMPLE_TIME,
            .clock_id = CLOCK_MONOTONIC_RAW,
            .sample_id_all = true,
            .use_clock_id = true,
            .context_switch = true,
            .disabled = false,
            .inherit = false,
            .write_backward = false};
}

auto PerfContextSwitchSource::prepare(LinuxThreadId thread_id, std::size_t data_pages)
    -> std::expected<PerfContextSwitchSource, core::Error> {
    const auto page_size_value = ::sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0)
        return std::unexpected{perf_error("linux.perf_context_switch.invalid_configuration",
                                          "prepare_perf_context_switch", "ring_pages")};
    const detail::PerfContextSwitchOperations operations{
        nullptr,
        [](void*, const perf_event_attr& attr,
           LinuxThreadId id) noexcept -> std::expected<int, core::Error> {
            const auto descriptor = static_cast<int>(::syscall(SYS_perf_event_open, &attr,
                                                               static_cast<pid_t>(id.value()), -1,
                                                               -1, PERF_FLAG_FD_CLOEXEC));
            if (descriptor < 0)
                return std::unexpected{perf_error("linux.perf_context_switch.open_failed",
                                                  "prepare_perf_context_switch", "open", errno)};
            return descriptor;
        },
        [](void*, int descriptor, std::size_t size) noexcept -> std::expected<void*, core::Error> {
            auto* mapping =
                ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
            if (mapping == MAP_FAILED)
                return std::unexpected{perf_error("linux.perf_context_switch.map_failed",
                                                  "prepare_perf_context_switch", "mmap", errno)};
            return mapping;
        },
        [](void*, void* mapping, std::size_t size) noexcept {
            static_cast<void>(::munmap(mapping, size));
        },
        [](void*, int descriptor) noexcept { static_cast<void>(::close(descriptor)); }};
    return prepare_with_operations(thread_id, data_pages, static_cast<std::size_t>(page_size_value),
                                   operations);
}

auto PerfContextSwitchSource::prepare_with_operations(
    LinuxThreadId thread_id, std::size_t data_pages, std::size_t page_size,
    const detail::PerfContextSwitchOperations& operations)
    -> std::expected<PerfContextSwitchSource, core::Error> {
    if (page_size == 0U || data_pages == 0U || (data_pages & (data_pages - 1U)) != 0U ||
        operations.open == nullptr || operations.map == nullptr || operations.unmap == nullptr ||
        operations.close == nullptr)
        return std::unexpected{perf_error("linux.perf_context_switch.invalid_configuration",
                                          "prepare_perf_context_switch", "ring_pages")};
    perf_event_attr attr{};
    const auto config = configuration();
    attr.type = config.type;
    attr.size = config.size;
    attr.config = config.config;
    attr.sample_type = config.sample_type;
    attr.sample_id_all = 1U;
    attr.use_clockid = 1U;
    attr.clockid = config.clock_id;
    attr.context_switch = 1U;
    auto descriptor_result = operations.open(operations.context, attr, thread_id);
    if (!descriptor_result)
        return std::unexpected{descriptor_result.error()};
    const auto descriptor = *descriptor_result;
    const auto mapping_size = (data_pages + 1U) * page_size;
    auto mapping_result = operations.map(operations.context, descriptor, mapping_size);
    if (!mapping_result) {
        operations.close(operations.context, descriptor);
        return std::unexpected{mapping_result.error()};
    }
    auto state = std::make_unique<State>();
    state->descriptor = descriptor;
    state->mapping = *mapping_result;
    state->mapping_size = mapping_size;
    state->page_size = page_size;
    state->operations = operations;
    return PerfContextSwitchSource{std::move(state)};
}

auto detail::PerfContextSwitchAccess::prepare(LinuxThreadId thread_id, std::size_t data_pages,
                                              std::size_t page_size,
                                              const PerfContextSwitchOperations& operations)
    -> std::expected<PerfContextSwitchSource, core::Error> {
    return PerfContextSwitchSource::prepare_with_operations(thread_id, data_pages, page_size,
                                                            operations);
}

PerfContextSwitchSource::PerfContextSwitchSource(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}
PerfContextSwitchSource::PerfContextSwitchSource(PerfContextSwitchSource&&) noexcept = default;
auto PerfContextSwitchSource::operator=(PerfContextSwitchSource&&) noexcept
    -> PerfContextSwitchSource& = default;
PerfContextSwitchSource::~PerfContextSwitchSource() noexcept = default;

auto detail::PerfContextSwitchAccess::decode(std::span<const std::byte> ring, std::uint64_t tail,
                                             std::uint64_t head)
    -> std::expected<PerfContextSwitchDrain, core::Error> {
    if (ring.empty() || head < tail || head - tail > ring.size())
        return std::unexpected{perf_error("linux.perf_context_switch.ring_corrupt",
                                          "drain_perf_context_switch", "cursor")};
    std::vector<PerfContextSwitchRecord> records;
    auto completeness = PerfCaptureCompleteness::complete;
    while (tail < head) {
        if (head - tail < sizeof(perf_event_header))
            return std::unexpected{perf_error("linux.perf_context_switch.record_malformed",
                                              "drain_perf_context_switch", "truncated_header")};
        const auto header = read_wrapped<perf_event_header>(ring, tail);
        if (header.size < sizeof(perf_event_header) || header.size > head - tail)
            return std::unexpected{perf_error("linux.perf_context_switch.record_malformed",
                                              "drain_perf_context_switch", "record_size")};
        if (header.type == PERF_RECORD_SWITCH) {
            if (header.size != sizeof(perf_event_header) + sizeof(std::uint64_t))
                return std::unexpected{perf_error("linux.perf_context_switch.record_malformed",
                                                  "drain_perf_context_switch", "switch_size")};
            const auto timestamp = read_wrapped<std::uint64_t>(ring, tail + sizeof(header));
            const auto outgoing = (header.misc & PERF_RECORD_MISC_SWITCH_OUT) != 0U;
            if (outgoing) {
                const auto preempt = (header.misc & PERF_RECORD_MISC_SWITCH_OUT_PREEMPT) != 0U;
                records.push_back(PerfContextSwitchRecord::create(
                    RawMonotonicTimestamp::from_nanoseconds(timestamp),
                    preempt ? PerfContextSwitchKind::involuntary_switch_out
                            : PerfContextSwitchKind::voluntary_switch_out));
            }
        } else if (header.type == PERF_RECORD_LOST || header.type == PERF_RECORD_LOST_SAMPLES) {
            completeness = PerfCaptureCompleteness::incomplete;
        } else {
            return std::unexpected{perf_error("linux.perf_context_switch.record_unsupported",
                                              "drain_perf_context_switch", "record_type")};
        }
        tail += header.size;
    }
    return PerfContextSwitchDrain::create(std::move(records), completeness);
}

auto PerfContextSwitchSource::drain() noexcept
    -> std::expected<PerfContextSwitchDrain, core::Error> {
    if (!state_)
        return std::unexpected{perf_error("linux.perf_context_switch.invalid_state",
                                          "drain_perf_context_switch", "moved_from")};
    auto* metadata = static_cast<perf_event_mmap_page*>(state_->mapping);
    const auto head = metadata->data_head;
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto tail = metadata->data_tail;
    const auto ring = std::span<const std::byte>{static_cast<const std::byte*>(state_->mapping) +
                                                     metadata->data_offset,
                                                 static_cast<std::size_t>(metadata->data_size)};
    auto result = detail::PerfContextSwitchAccess::decode(ring, tail, head);
    if (result) {
        std::atomic_thread_fence(std::memory_order_release);
        metadata->data_tail = head;
    }
    return result;
}

auto detail::PerfContextSwitchAccess::drain_bounded(
    PerfContextSwitchSource& source, std::span<PerfContextSwitchRecord> output) noexcept
    -> std::expected<BoundedDrain, core::Error> {
    if (!source.state_)
        return std::unexpected{perf_error("linux.perf_context_switch.invalid_state",
                                          "drain_perf_context_switch", "moved_from")};
    const auto ring_limit = source.state_->mapping_size - source.state_->page_size;
    auto metadata = validate_bounded_perf_metadata(
        source.state_->mapping, source.state_->mapping_size, ring_limit,
        ring_limit / (sizeof(perf_event_header) + sizeof(std::uint64_t)));
    if (!metadata)
        return std::unexpected{perf_error("linux.perf_context_switch.ring_corrupt",
                                          "drain_perf_context_switch", "cursor")};
    auto value = drain_bounded_perf_records(source.state_->mapping, *metadata, output,
                                            nullptr, nullptr);
    if (!value) {
        switch (value.error()) {
        case BoundedPerfFailureKind::metadata_capacity:
        case BoundedPerfFailureKind::cursor:
            return std::unexpected{perf_error("linux.perf_context_switch.ring_corrupt",
                                              "drain_perf_context_switch", "cursor")};
        case BoundedPerfFailureKind::truncated_header:
            return std::unexpected{perf_error("linux.perf_context_switch.record_malformed",
                                              "drain_perf_context_switch", "truncated_header")};
        case BoundedPerfFailureKind::record_size:
            return std::unexpected{perf_error("linux.perf_context_switch.record_malformed",
                                              "drain_perf_context_switch", "record_size")};
        case BoundedPerfFailureKind::switch_size:
            return std::unexpected{perf_error("linux.perf_context_switch.record_malformed",
                                              "drain_perf_context_switch", "switch_size")};
        case BoundedPerfFailureKind::record:
            return std::unexpected{perf_error("linux.perf_context_switch.record_malformed",
                                              "drain_perf_context_switch", "record")};
        case BoundedPerfFailureKind::capacity:
            return std::unexpected{perf_error("linux.perf_context_switch.record_capacity",
                                              "drain_perf_context_switch", "capacity")};
        case BoundedPerfFailureKind::record_type:
            return std::unexpected{perf_error("linux.perf_context_switch.record_unsupported",
                                              "drain_perf_context_switch", "record_type")};
        case BoundedPerfFailureKind::lost:
            return std::unexpected{perf_error("linux.perf_context_switch.capture_incomplete",
                                              "drain_perf_context_switch", "lost")};
        }
    }
    return BoundedDrain{value->record_count, PerfCaptureCompleteness::complete};
}

auto detail::validate_bounded_perf_metadata(
    void* mapping, std::size_t mapping_size, std::size_t ring_limit,
    std::size_t record_capacity) noexcept
    -> std::expected<BoundedPerfMetadata, BoundedPerfFailureKind> {
    if (mapping == nullptr || mapping == MAP_FAILED)
        return std::unexpected{BoundedPerfFailureKind::metadata_capacity};
    const auto* metadata = static_cast<const perf_event_mmap_page*>(mapping);
    const auto offset = metadata->data_offset;
    const auto size = metadata->data_size;
    constexpr auto record_size = sizeof(perf_event_header) + sizeof(std::uint64_t);
    if (offset == 0U || size == 0U || offset > mapping_size ||
        size > mapping_size - offset || size > ring_limit ||
        size / record_size > record_capacity ||
        offset > std::numeric_limits<std::size_t>::max() ||
        size > std::numeric_limits<std::size_t>::max())
        return std::unexpected{BoundedPerfFailureKind::metadata_capacity};
    return BoundedPerfMetadata{static_cast<std::size_t>(offset),
                               static_cast<std::size_t>(size)};
}

auto detail::drain_bounded_perf_records(
    void* mapping, BoundedPerfMetadata layout,
    std::span<PerfContextSwitchRecord> output, void* hook_context,
    void (*before_second_pass)(void*) noexcept, bool discard_lost, bool commit_tail,
    BoundedPerfFirstLoss* first_loss) noexcept
    -> std::expected<BoundedPerfDrain, BoundedPerfFailureKind> {
    auto* metadata = static_cast<perf_event_mmap_page*>(mapping);
    const auto head = metadata->data_head;
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto initial = metadata->data_tail;
    const auto ring = std::span<const std::byte>{
        static_cast<const std::byte*>(mapping) + layout.ring_offset,
        layout.ring_size};
    if (head < initial || head - initial > ring.size())
        return std::unexpected{BoundedPerfFailureKind::cursor};

    auto cursor = initial;
    std::size_t count{};
    bool lost{};
    std::uint64_t fingerprint{1469598103934665603ULL};
    while (cursor < head) {
        if (head - cursor < sizeof(perf_event_header))
            return std::unexpected{BoundedPerfFailureKind::truncated_header};
        const auto header = read_wrapped<perf_event_header>(ring, cursor);
        if (header.size < sizeof(header) || header.size > head - cursor)
            return std::unexpected{BoundedPerfFailureKind::record_size};
        for (std::uint64_t index{}; index < header.size; ++index) {
            fingerprint ^= std::to_integer<unsigned char>(
                ring[(cursor + index) % ring.size()]);
            fingerprint *= 1099511628211ULL;
        }
        if (header.type == PERF_RECORD_SWITCH) {
            if (header.size != sizeof(header) + sizeof(std::uint64_t))
                return std::unexpected{BoundedPerfFailureKind::switch_size};
            if ((header.misc & PERF_RECORD_MISC_SWITCH_OUT) != 0U && ++count > output.size())
                return std::unexpected{BoundedPerfFailureKind::capacity};
        } else if (header.type == PERF_RECORD_LOST ||
                   header.type == PERF_RECORD_LOST_SAMPLES) {
            if (!discard_lost) {
                if (first_loss != nullptr && !first_loss->available) {
                    const auto offset = header.type == PERF_RECORD_LOST
                        ? sizeof(header) + sizeof(std::uint64_t) : sizeof(header);
                    std::optional<std::uint64_t> lost_count;
                    // Generic header/prefix bounds are already validated. The
                    // type-specific count is unavailable for a short body; the
                    // original lost refusal remains unchanged.
                    if (header.size >= offset + sizeof(std::uint64_t))
                        lost_count = read_wrapped<std::uint64_t>(ring, cursor + offset);
                    *first_loss = BoundedPerfFirstLoss{
                        true, header.type, header.size, lost_count, head, initial,
                        static_cast<std::uint64_t>(ring.size())};
                }
                return std::unexpected{BoundedPerfFailureKind::lost};
            }
            lost = true;
        } else {
            return std::unexpected{BoundedPerfFailureKind::record_type};
        }
        cursor += header.size;
    }

    if (before_second_pass != nullptr)
        before_second_pass(hook_context);
    auto observed_head = head;
    const auto cursors_valid = [&]() noexcept {
        const auto current_head = metadata->data_head;
        std::atomic_thread_fence(std::memory_order_acquire);
        // Forward, writable perf rings preserve unread bytes. The producer may
        // append beyond our captured prefix, but cannot regress or overrun it.
        if (current_head < observed_head || current_head - initial > ring.size() ||
            metadata->data_tail != initial)
            return false;
        observed_head = current_head;
        return true;
    };
    if (!cursors_valid())
        return std::unexpected{BoundedPerfFailureKind::record};

    cursor = initial;
    std::size_t committed{};
    auto confirmed = std::uint64_t{1469598103934665603ULL};
    while (cursor < head) {
        if (head - cursor < sizeof(perf_event_header))
            return std::unexpected{BoundedPerfFailureKind::record};
        const auto header = read_wrapped<perf_event_header>(ring, cursor);
        if (header.size < sizeof(header) || header.size > head - cursor)
            return std::unexpected{BoundedPerfFailureKind::record};
        for (std::uint64_t index{}; index < header.size; ++index) {
            confirmed ^= std::to_integer<unsigned char>(
                ring[(cursor + index) % ring.size()]);
            confirmed *= 1099511628211ULL;
        }
        if (header.type == PERF_RECORD_SWITCH) {
            if (header.size != sizeof(header) + sizeof(std::uint64_t))
                return std::unexpected{BoundedPerfFailureKind::record};
            if ((header.misc & PERF_RECORD_MISC_SWITCH_OUT) != 0U) {
                if (committed == output.size())
                    return std::unexpected{BoundedPerfFailureKind::record};
                const auto timestamp =
                    read_wrapped<std::uint64_t>(ring, cursor + sizeof(header));
                output[committed++] = PerfContextSwitchRecord::create(
                    RawMonotonicTimestamp::from_nanoseconds(timestamp),
                    (header.misc & PERF_RECORD_MISC_SWITCH_OUT_PREEMPT) != 0U
                        ? PerfContextSwitchKind::involuntary_switch_out
                        : PerfContextSwitchKind::voluntary_switch_out);
            }
        } else if (header.type == PERF_RECORD_LOST ||
                   header.type == PERF_RECORD_LOST_SAMPLES) {
            if (!discard_lost)
                return std::unexpected{BoundedPerfFailureKind::record};
        } else {
            return std::unexpected{BoundedPerfFailureKind::record};
        }
        cursor += header.size;
    }
    if (confirmed != fingerprint || committed != count || !cursors_valid())
        return std::unexpected{BoundedPerfFailureKind::record};
    // Commit only the captured prefix; later producer appends remain pending.
    if (commit_tail) {
        std::atomic_thread_fence(std::memory_order_release);
        metadata->data_tail = head;
    }
    return BoundedPerfDrain{count, lost};
}

auto detail::production_bounded_perf_context_switch_operations() noexcept
    -> BoundedPerfContextSwitchOperations {
    return bounded_perf_context_switch_operations(q2_perf_production_provider());
}

namespace {
auto open_bounded_perf(void* raw, LinuxThreadId thread, bool capture_epoch) noexcept
    -> detail::BoundedPerfContextSwitchOperations::DescriptorResult {
    auto operations = static_cast<detail::Q2PerfProductionProvider*>(raw)->operations();
    perf_event_attr attributes{};
    const auto configuration = PerfContextSwitchSource::configuration();
    attributes.type = configuration.type;
    attributes.size = configuration.size;
    attributes.config = configuration.config;
    attributes.sample_type = configuration.sample_type;
    attributes.sample_id_all = configuration.sample_id_all ? 1U : 0U;
    attributes.use_clockid = configuration.use_clock_id ? 1U : 0U;
    attributes.clockid = configuration.clock_id;
    attributes.context_switch = configuration.context_switch ? 1U : 0U;
    attributes.disabled = capture_epoch || configuration.disabled ? 1U : 0U;
    attributes.read_format = capture_epoch ? static_cast<std::uint64_t>(PERF_FORMAT_LOST) : 0U;
    attributes.inherit = configuration.inherit ? 1U : 0U;
    attributes.write_backward = configuration.write_backward ? 1U : 0U;
    const auto result = operations.open(operations.context, attributes, thread);
    return {result.descriptor, result.error};
}
} // namespace

auto detail::bounded_perf_context_switch_operations(Q2PerfProductionProvider& provider) noexcept
    -> BoundedPerfContextSwitchOperations {
    auto bundle = BoundedPerfContextSwitchOperations{
        &provider,
        [](void* raw, LinuxThreadId thread) noexcept
            -> BoundedPerfContextSwitchOperations::DescriptorResult {
            return open_bounded_perf(raw, thread, false);
        },
        [](void* raw, int descriptor, std::size_t size) noexcept
            -> BoundedPerfContextSwitchOperations::MappingResult {
            auto operations = static_cast<Q2PerfProductionProvider*>(raw)->operations();
            const auto result = operations.map(operations.context, descriptor, size);
            if (result.mapping == MAP_FAILED || result.mapping == nullptr)
                return {nullptr, result.error, BoundedPerfMapFailure::mmap};
            return {result.mapping, 0, BoundedPerfMapFailure::mmap};
        },
        [](void* raw, void* mapping, std::size_t size) noexcept
            -> BoundedPerfContextSwitchOperations::StatusResult {
            auto operations = static_cast<Q2PerfProductionProvider*>(raw)->operations();
            const auto result = operations.unmap(operations.context, mapping, size);
            return {result == 0 ? 0 : -1, result};
        },
        [](void* raw, int descriptor) noexcept
            -> BoundedPerfContextSwitchOperations::StatusResult {
            auto operations = static_cast<Q2PerfProductionProvider*>(raw)->operations();
            const auto result = operations.close(operations.context, descriptor);
            return {result == 0 ? 0 : -1, result};
        },
        nullptr,
        nullptr,
        [](void* raw, int descriptor) noexcept -> BoundedPerfContextSwitchOperations::StatusResult {
            const auto operations = static_cast<Q2PerfProductionProvider*>(raw)->operations();
            const auto error = operations.enable(operations.context, descriptor);
            return {error == 0 ? 0 : -1, error};
        },
        [](void* raw, int descriptor) noexcept -> BoundedPerfContextSwitchOperations::StatusResult {
            const auto operations = static_cast<Q2PerfProductionProvider*>(raw)->operations();
            const auto error = operations.disable(operations.context, descriptor);
            return {error == 0 ? 0 : -1, error};
        },
        [](void* raw, int descriptor) noexcept -> PerfContextLossReadResult {
            const auto operations = static_cast<Q2PerfProductionProvider*>(raw)->operations();
            return operations.read_loss(operations.context, descriptor);
        },
        [](void* raw, LinuxThreadId thread) noexcept
            -> BoundedPerfContextSwitchOperations::DescriptorResult {
            return open_bounded_perf(raw, thread, true);
        }};
    const auto typed = provider.operations();
    if (typed.open == nullptr) { bundle.open = nullptr; bundle.open_capture = nullptr; }
    if (typed.map == nullptr) bundle.map = nullptr;
    if (typed.unmap == nullptr) bundle.unmap = nullptr;
    if (typed.close == nullptr) bundle.close = nullptr;
    if (typed.enable == nullptr) bundle.enable = nullptr;
    if (typed.disable == nullptr) bundle.disable = nullptr;
    if (typed.read_loss == nullptr) bundle.read_loss = nullptr;
    return bundle;
}

struct detail::Q2PerfContextSwitchState {
    Q2PerfContextSwitchOperations typed{};
    int descriptor{-1};
    void* mapping{MAP_FAILED};
    std::size_t mapping_size{};
    std::size_t page_size{};
    std::optional<PerfContextSwitchSource> source;
    bool active{};
    std::array<std::optional<core::Error>, 6> drain_errors;
    std::array<std::optional<core::Error>, 4> operation_errors;
    bool drain_failed{};
    bool failed_drain_reobserved{};
};
detail::Q2PerfRetiredState::Q2PerfRetiredState(
    std::unique_ptr<Q2PerfContextSwitchState> state) noexcept
    : state_{std::move(state)} {}
detail::Q2PerfRetiredState::Q2PerfRetiredState() noexcept = default;
detail::Q2PerfRetiredState::Q2PerfRetiredState(Q2PerfRetiredState&&) noexcept = default;
detail::Q2PerfRetiredState&
detail::Q2PerfRetiredState::operator=(Q2PerfRetiredState&&) noexcept = default;
detail::Q2PerfRetiredState::~Q2PerfRetiredState() noexcept = default;
detail::Q2PerfContextSwitchSource::Q2PerfContextSwitchSource(
    std::unique_ptr<Q2PerfContextSwitchState> state) noexcept
    : state_{std::move(state)} {}
detail::Q2PerfContextSwitchSource::Q2PerfContextSwitchSource(Q2PerfContextSwitchSource&&) noexcept =
    default;
detail::Q2PerfContextSwitchSource::~Q2PerfContextSwitchSource() noexcept {
    if (state_ && state_->active)
        std::terminate();
}
auto detail::Q2PerfContextSwitchSource::prepare(LinuxThreadId tid, std::size_t pages,
                                                std::size_t page_size,
                                                Q2PerfContextSwitchOperations operations) noexcept
    -> std::expected<Q2PerfContextSwitchSource, Q2PerfPrepareFailure> {
    if (!operations.open || !operations.map || !operations.unmap || !operations.close)
        return std::unexpected{
            Q2PerfPrepareFailure{Q2PerfPrepareFailure::Stage::validation,
                                 perf_error("linux.perf_context_switch.invalid_configuration",
                                            "prepare_perf_context_switch", "operations"),
                                 {},
                                 {}}};
    std::unique_ptr<Q2PerfContextSwitchState> state;
    std::optional<core::Error> construction_error;
    try {
        construction_error.emplace(perf_error("linux.perf_context_switch.construction_failed",
                                              "prepare_perf_context_switch", "allocation"));
        state = std::make_unique<Q2PerfContextSwitchState>();
        state->typed = operations;
        state->page_size = page_size;
        state->drain_errors[0].emplace(perf_error("linux.perf_context_switch.ring_corrupt",
                                                  "drain_perf_context_switch", "cursor"));
        state->drain_errors[1].emplace(perf_error("linux.perf_context_switch.record_malformed",
                                                  "drain_perf_context_switch", "record"));
        state->drain_errors[2].emplace(perf_error("linux.perf_context_switch.record_capacity",
                                                  "drain_perf_context_switch", "capacity"));
        state->drain_errors[3].emplace(perf_error("linux.perf_context_switch.record_unsupported",
                                                  "drain_perf_context_switch", "record_type"));
        state->drain_errors[4].emplace(perf_error("linux.perf_context_switch.capture_incomplete",
                                                  "drain_perf_context_switch", "lost"));
        state->drain_errors[5].emplace(perf_error("linux.perf_context_switch.previous_drain_failed",
                                                  "drain_perf_context_switch", "terminal_drain"));
        state->operation_errors[0].emplace(q2_native_error("linux.perf_context_switch.open_failed",
                                                           "prepare_perf_context_switch", "open"));
        state->operation_errors[1].emplace(q2_native_error("linux.perf_context_switch.map_failed",
                                                           "prepare_perf_context_switch", "map"));
        state->operation_errors[2].emplace(q2_native_error("linux.perf_context_switch.unmap_failed",
                                                           "retire_perf_context_switch", "unmap"));
        state->operation_errors[3].emplace(q2_native_error("linux.perf_context_switch.close_failed",
                                                           "retire_perf_context_switch", "close"));
        const PerfContextSwitchOperations bridge{
            state.get(),
            [](void* raw, const perf_event_attr& attr,
               LinuxThreadId id) noexcept -> std::expected<int, core::Error> {
                auto& s = *static_cast<Q2PerfContextSwitchState*>(raw);
                auto value = s.typed.open(s.typed.context, attr, id);
                if (value.descriptor >= 0) {
                    s.descriptor = value.descriptor;
                    return value.descriptor;
                }
                s.operation_errors[0]->os_error->value = value.error;
                return std::unexpected{std::move(*s.operation_errors[0])};
            },
            [](void* raw, int descriptor,
               std::size_t size) noexcept -> std::expected<void*, core::Error> {
                auto& s = *static_cast<Q2PerfContextSwitchState*>(raw);
                auto value = s.typed.map(s.typed.context, descriptor, size);
                if (value.mapping != MAP_FAILED && value.mapping != nullptr) {
                    s.mapping = value.mapping;
                    s.mapping_size = size;
                    return value.mapping;
                }
                s.operation_errors[1]->os_error->value = value.error;
                return std::unexpected{std::move(*s.operation_errors[1])};
            },
            [](void*, void*, std::size_t) noexcept {}, [](void*, int) noexcept {}};
        auto source = PerfContextSwitchAccess::prepare(tid, pages, page_size, bridge);
        if (!source) {
            std::optional<core::Error> cleanup;
            if (state->descriptor >= 0) {
                const auto error = state->typed.close(state->typed.context, state->descriptor);
                if (error != 0) {
                    state->operation_errors[3]->os_error->value = error;
                    cleanup.emplace(std::move(*state->operation_errors[3]));
                }
            }
            const auto stage = state->descriptor < 0 ? Q2PerfPrepareFailure::Stage::open
                                                     : Q2PerfPrepareFailure::Stage::map;
            return std::unexpected{
                Q2PerfPrepareFailure{stage, std::move(source.error()), std::move(cleanup), {}}};
        }
        if (state->typed.after_map_test)
            state->typed.after_map_test(state->typed.context);
        state->source.emplace(std::move(*source));
        state->active = true;
        return Q2PerfContextSwitchSource{std::move(state)};
    } catch (...) {
        Q2PerfPrepareFailure result{Q2PerfPrepareFailure::Stage::construction,
                                    construction_error ? std::move(*construction_error)
                                                       : core::Error{},
                                    {},
                                    {}};
        if (state && state->mapping != MAP_FAILED) {
            const auto error =
                state->typed.unmap(state->typed.context, state->mapping, state->mapping_size);
            if (error != 0) {
                state->operation_errors[2]->os_error->value = error;
                result.cleanup.emplace(std::move(*state->operation_errors[2]));
            }
        }
        if (state && state->descriptor >= 0) {
            const auto error = state->typed.close(state->typed.context, state->descriptor);
            if (error != 0) {
                state->operation_errors[3]->os_error->value = error;
                if (!result.cleanup)
                    result.cleanup.emplace(std::move(*state->operation_errors[3]));
                else
                    result.cleanup2.emplace(std::move(*state->operation_errors[3]));
            }
        }
        return std::unexpected{std::move(result)};
    }
}
auto detail::Q2PerfContextSwitchSource::drain_bounded(
    std::span<PerfContextSwitchRecord> output) noexcept
    -> std::expected<Q2PerfBoundedDrain, core::Error> {
    return decode_bounded(output, true);
}
auto detail::Q2PerfContextSwitchSource::preview_bounded(
    std::span<PerfContextSwitchRecord> output) noexcept
    -> std::expected<Q2PerfBoundedDrain, core::Error> {
    return decode_bounded(output, false);
}
auto detail::Q2PerfContextSwitchSource::decode_bounded(
    std::span<PerfContextSwitchRecord> output, bool commit_tail) noexcept
    -> std::expected<Q2PerfBoundedDrain, core::Error> {
    if (!state_ || !state_->active || !state_->source || state_->drain_failed)
        std::terminate();
    auto reject =
        [&](std::size_t index) noexcept -> std::expected<Q2PerfBoundedDrain, core::Error> {
        state_->drain_failed = true;
        return std::unexpected{std::move(*state_->drain_errors[index])};
    };
    const auto ring_limit = state_->mapping_size - state_->page_size;
    auto metadata = validate_bounded_perf_metadata(
        state_->mapping, state_->mapping_size, ring_limit,
        ring_limit / (sizeof(perf_event_header) + sizeof(std::uint64_t)));
    if (!metadata)
        return reject(0);
    auto value = drain_bounded_perf_records(state_->mapping, *metadata, output,
                                            nullptr, nullptr, false, commit_tail);
    if (!value) {
        switch (value.error()) {
        case BoundedPerfFailureKind::metadata_capacity:
        case BoundedPerfFailureKind::cursor: return reject(0);
        case BoundedPerfFailureKind::truncated_header:
        case BoundedPerfFailureKind::record_size:
        case BoundedPerfFailureKind::switch_size:
        case BoundedPerfFailureKind::record: return reject(1);
        case BoundedPerfFailureKind::capacity: return reject(2);
        case BoundedPerfFailureKind::record_type: return reject(3);
        case BoundedPerfFailureKind::lost: return reject(4);
        }
    }
    return Q2PerfBoundedDrain{value->record_count,
                              PerfCaptureCompleteness::complete};
}
auto detail::Q2PerfContextSwitchSource::terminal_drain_after_failure(
    std::span<PerfContextSwitchRecord> output) noexcept
    -> std::expected<Q2PerfBoundedDrain, core::Error> {
    if (!state_ || !state_->active || !state_->source || !state_->drain_failed ||
        state_->failed_drain_reobserved)
        std::terminate();
    state_->failed_drain_reobserved = true;
    // This is a distinct terminal observation, not a replay of the scheduled
    // drain: the ordinary cursor remains failed and no successful ordinal can
    // be published.  Still touch/decode the retained ring once so retirement
    // never skips the contractually required final source observation.
    static_cast<void>(detail::PerfContextSwitchAccess::drain_bounded(*state_->source, output));
    return std::unexpected{std::move(*state_->drain_errors[5])};
}
auto detail::Q2PerfContextSwitchSource::retire() noexcept -> Q2PerfRetirement {
    if (!state_ || !state_->active)
        std::terminate();
    state_->source.reset();
    Q2PerfRetirement result;
    if (state_->mapping != MAP_FAILED) {
        const auto error =
            state_->typed.unmap(state_->typed.context, state_->mapping, state_->mapping_size);
        if (error != 0) {
            state_->operation_errors[2]->os_error->value = error;
            result.primary.emplace(std::move(*state_->operation_errors[2]));
        }
    }
    if (state_->descriptor >= 0) {
        const auto error = state_->typed.close(state_->typed.context, state_->descriptor);
        if (error != 0) {
            state_->operation_errors[3]->os_error->value = error;
            if (!result.primary)
                result.primary.emplace(std::move(*state_->operation_errors[3]));
            else
                result.cleanup.emplace(std::move(*state_->operation_errors[3]));
        }
    }
    state_->active = false;
    result.deferred_state = Q2PerfRetiredState{std::move(state_)};
    return result;
}
auto detail::Q2PerfProductionProvider::operations() noexcept -> Q2PerfContextSwitchOperations {
    auto bundle = Q2PerfContextSwitchOperations{
        this,
        [](void* raw, const perf_event_attr& attr,
           LinuxThreadId tid) noexcept -> Q2PerfContextSwitchOperations::OpenOutcome {
            auto& provider = *static_cast<Q2PerfProductionProvider*>(raw);
            const auto value = provider.operations_.perf_open(provider.operations_.context, &attr,
                                                              static_cast<int>(tid.value()), -1, -1,
                                                              PERF_FLAG_FD_CLOEXEC);
            if (value < -static_cast<long>(std::numeric_limits<int>::max()) ||
                value > static_cast<long>(std::numeric_limits<int>::max()))
                return {-1, EOVERFLOW};
            return value < 0
                       ? Q2PerfContextSwitchOperations::OpenOutcome{-1, static_cast<int>(-value)}
                       : Q2PerfContextSwitchOperations::OpenOutcome{static_cast<int>(value), 0};
        },
        [](void* raw, int descriptor,
           std::size_t size) noexcept -> Q2PerfContextSwitchOperations::MapOutcome {
            auto& provider = *static_cast<Q2PerfProductionProvider*>(raw);
            auto* value = provider.operations_.map(provider.operations_.context, size, descriptor);
            return value == MAP_FAILED
                       ? Q2PerfContextSwitchOperations::MapOutcome{MAP_FAILED, errno}
                       : Q2PerfContextSwitchOperations::MapOutcome{value, 0};
        },
        [](void* raw, void* mapping, std::size_t size) noexcept -> int {
            auto& provider = *static_cast<Q2PerfProductionProvider*>(raw);
            const auto result =
                provider.operations_.unmap(provider.operations_.context, mapping, size);
            return result == 0 ? 0 : errno;
        },
        [](void* raw, int descriptor) noexcept -> int {
            auto& provider = *static_cast<Q2PerfProductionProvider*>(raw);
            const auto result =
                provider.operations_.close(provider.operations_.context, descriptor);
            return result == 0 ? 0 : errno;
        },
        nullptr,
        [](void* raw, int descriptor) noexcept -> int {
            auto& provider = *static_cast<Q2PerfProductionProvider*>(raw);
            const auto result = provider.operations_.control(provider.operations_.context, descriptor, true);
            return result == 0 ? 0 : errno;
        },
        [](void* raw, int descriptor) noexcept -> int {
            auto& provider = *static_cast<Q2PerfProductionProvider*>(raw);
            const auto result = provider.operations_.control(provider.operations_.context, descriptor, false);
            return result == 0 ? 0 : errno;
        },
        [](void* raw, int descriptor) noexcept -> PerfContextLossReadResult {
            auto& provider = *static_cast<Q2PerfProductionProvider*>(raw);
            std::array<std::uint64_t, 2> values{};
            static_assert(sizeof(values) == 16U);
            const auto bytes = provider.operations_.read(provider.operations_.context,
                                                        descriptor, values.data(), sizeof(values));
            const auto error = bytes < 0 ? errno : 0;
            return {values[0], values[1], bytes, error};
        }};
    if (operations_.perf_open == nullptr) bundle.open = nullptr;
    if (operations_.map == nullptr) bundle.map = nullptr;
    if (operations_.unmap == nullptr) bundle.unmap = nullptr;
    if (operations_.close == nullptr) bundle.close = nullptr;
    if (operations_.control == nullptr) { bundle.enable = nullptr; bundle.disable = nullptr; }
    if (operations_.read == nullptr) bundle.read_loss = nullptr;
    return bundle;
}
auto detail::q2_perf_production_provider() noexcept -> Q2PerfProductionProvider& {
    static Q2PerfProductionProvider provider{
        {nullptr,
         [](void*, const perf_event_attr* attr, int pid, int cpu, int group,
            unsigned long flags) noexcept -> long {
             const auto result = ::syscall(SYS_perf_event_open, attr, pid, cpu, group, flags);
             return result < 0 ? -errno : result;
         },
         [](void*, std::size_t size, int descriptor) noexcept -> void* {
             return ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
         },
         [](void*, void* mapping, std::size_t size) noexcept -> int {
             return ::munmap(mapping, size);
         },
         [](void*, int descriptor) noexcept -> int { return ::close(descriptor); },
         [](void*, int descriptor, bool enable) noexcept -> int {
             return ::ioctl(descriptor, enable ? PERF_EVENT_IOC_ENABLE : PERF_EVENT_IOC_DISABLE, 0UL);
         },
         [](void*, int descriptor, void* destination, std::size_t bytes) noexcept -> std::int64_t {
             return static_cast<std::int64_t>(::read(descriptor, destination, bytes));
         }}};
    return provider;
}

} // namespace epycbench::platform_linux
