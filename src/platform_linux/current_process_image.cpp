#include "platform_linux/current_process_image.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <utility>

namespace epycbench::platform_linux::detail {
namespace {
auto failure(ImageStage stage, ImageReason reason, int number = 0) noexcept -> ImageFailure {
    return {stage, reason, number, {}, 0, std::nullopt};
}
auto failed(RawStatusResult value) noexcept -> bool {
    return value.value != 0 || value.error_number != 0;
}
auto native_failure(ImageStage stage, RawStatusResult value) noexcept -> ImageFailure {
    const bool unsupported =
        value.value == -1 && (value.error_number == EACCES || value.error_number == EPERM ||
                              value.error_number == ENOENT || value.error_number == ENOTDIR ||
                              value.error_number == ENOSYS);
    return failure(stage,
                   unsupported ? ImageReason::unsupported_access
                               : (value.value == -1 && value.error_number != 0
                                      ? ImageReason::native_failure
                                      : ImageReason::unexpected_native_result),
                   value.error_number);
}
auto descriptor_value(long value) noexcept -> bool {
    return value >= 0 && value <= std::numeric_limits<int>::max();
}
auto admitted(const ImageMetadata& value) noexcept -> bool {
    return (value.mode & S_IFMT) == S_IFREG && value.size > 0 &&
           (value.mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 && value.modified_nanoseconds >= 0 &&
           value.modified_nanoseconds < 1000000000;
}
auto same(const ImageMetadata& left, const ImageMetadata& right) noexcept -> bool {
    return left.device == right.device && left.inode == right.inode && left.mode == right.mode &&
           left.uid == right.uid && left.gid == right.gid && left.size == right.size &&
           left.modified_seconds == right.modified_seconds &&
           left.modified_nanoseconds == right.modified_nanoseconds;
}
void retire(std::optional<FileDescriptor>& descriptor, ImageFailure& primary) noexcept {
    if (!descriptor || !descriptor->active())
        return;
    const auto result = DescriptorAccess::close_raw(*descriptor);
    if (failed(result) && primary.cleanup_count < primary.cleanup.size())
        primary.cleanup[primary.cleanup_count++] = {result.value, result.error_number};
}
auto own(int descriptor, DescriptorOperations operations) -> FileDescriptor {
    // Positive native descriptors and complete callbacks were checked first.
    return std::move(*DescriptorAccess::adopt(descriptor, operations));
}
} // namespace

auto CurrentImageAccess::render_alias(int process, int descriptor, std::span<char> output) noexcept
    -> std::expected<std::size_t, ImageFailure> {
    if (process <= 0 || descriptor < 5)
        return std::unexpected{failure(ImageStage::alias, ImageReason::invalid_state)};
    std::array<char, 64> scratch{};
    constexpr std::string_view prefix = "/proc/", middle = "/fd/";
    auto* next = std::copy(prefix.begin(), prefix.end(), scratch.data());
    const auto pid = std::to_chars(next, scratch.data() + scratch.size(), process);
    if (pid.ec != std::errc{})
        return std::unexpected{failure(ImageStage::alias, ImageReason::size_overflow)};
    next = std::copy(middle.begin(), middle.end(), pid.ptr);
    const auto fd = std::to_chars(next, scratch.data() + scratch.size(), descriptor);
    if (fd.ec != std::errc{} || fd.ptr == scratch.data() + scratch.size())
        return std::unexpected{failure(ImageStage::alias, ImageReason::size_overflow)};
    *fd.ptr = '\0';
    const auto size = static_cast<std::size_t>(fd.ptr - scratch.data()) + 1U;
    if (output.size() < size)
        return std::unexpected{failure(ImageStage::alias, ImageReason::size_overflow)};
    std::copy_n(scratch.data(), size, output.data());
    return size;
}
auto CurrentImageAccess::argument_stock(std::size_t count, std::size_t bytes) noexcept
    -> std::expected<std::size_t, ImageFailure> {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (count == maximum || count > maximum / sizeof(std::string) ||
        count + 1U > maximum / sizeof(char*))
        return std::unexpected{failure(ImageStage::arguments, ImageReason::size_overflow)};
    const auto strings = count * sizeof(std::string);
    const auto pointers = (count + 1U) * sizeof(char*);
    if (strings > maximum - pointers || bytes > maximum - strings - pointers ||
        maximum - strings - pointers - bytes < 64U)
        return std::unexpected{failure(ImageStage::arguments, ImageReason::size_overflow)};
    return strings + pointers + bytes + 64U;
}
auto CurrentImageAccess::acquire(ImageOperations image, DescriptorOperations descriptors,
                                 SpawnOperations spawn, ProcessOperations process) noexcept
    -> std::expected<CurrentProcessImage, ImageFailure> {
    std::optional<FileDescriptor> original, normalized;
    auto stage = ImageStage::pid;
    auto reject = [&](ImageFailure error) -> std::expected<CurrentProcessImage, ImageFailure> {
        retire(normalized, error);
        retire(original, error);
        return std::unexpected{std::move(error)};
    };
    try {
        if (!image.current_pid || !image.open_image || !image.stat_image || !descriptors.close ||
            !descriptors.duplicate_cloexec || !spawn.initialize || !spawn.add_dup2 ||
            !spawn.add_close_from || !spawn.destroy || !spawn.spawn || !process.wait_pid ||
            !process.send_signal)
            return reject(failure(ImageStage::pid, ImageReason::invalid_state));
        const auto pid = image.current_pid(image.context);
        if (pid.error_number || pid.value <= 0 || pid.value > std::numeric_limits<int>::max())
            return reject(native_failure(ImageStage::pid, pid));
        stage = ImageStage::open;
        const auto opened = image.open_image(image.context, "/proc/self/exe", O_PATH | O_CLOEXEC);
        if (descriptor_value(opened.value))
            original.emplace(own(static_cast<int>(opened.value), descriptors));
        if (!original || opened.error_number)
            return reject(native_failure(ImageStage::open, opened));
        if (original->native_handle() < 5) {
            stage = ImageStage::normalization;
            const auto duplicate =
                descriptors.duplicate_cloexec(descriptors.context, original->native_handle(), 5);
            // Even a malformed successful return owns a native descriptor. Never
            // lose it because it is too low or accompanied by a spurious errno.
            if (descriptor_value(duplicate.value) && duplicate.value != original->native_handle())
                normalized.emplace(own(static_cast<int>(duplicate.value), descriptors));
            if (!normalized || duplicate.value < 5 || duplicate.error_number)
                return reject(native_failure(ImageStage::normalization, duplicate));
            const auto closed = DescriptorAccess::close_raw(*original);
            if (failed(closed))
                return reject(native_failure(ImageStage::normalization, closed));
        } else {
            normalized.emplace(std::move(*original));
        }
        stage = ImageStage::stat;
        ImageMetadata metadata{};
        const auto result = image.stat_image(image.context, normalized->native_handle(), &metadata);
        if (failed(result))
            return reject(native_failure(ImageStage::stat, result));
        if (!admitted(metadata))
            return reject(failure(ImageStage::stat, ImageReason::invalid_image));
        stage = ImageStage::alias;
        std::array<char, 64> alias{};
        auto rendered =
            render_alias(static_cast<int>(pid.value), normalized->native_handle(), alias);
        if (!rendered)
            return reject(std::move(rendered.error()));
        return CurrentProcessImage{std::move(*normalized),
                                   static_cast<int>(pid.value),
                                   metadata,
                                   alias,
                                   image,
                                   spawn,
                                   process};
    } catch (const std::bad_alloc&) {
        return reject(failure(stage, ImageReason::allocation_failure));
    } catch (...) {
        return reject(failure(stage, ImageReason::contained_failure));
    }
}
CurrentProcessImage::CurrentProcessImage(FileDescriptor&& descriptor, int process,
                                         ImageMetadata metadata, std::array<char, 64> alias,
                                         ImageOperations image, SpawnOperations spawn,
                                         ProcessOperations operations) noexcept
    : descriptor_(std::move(descriptor)), process_(process), metadata_(metadata), alias_(alias),
      image_(image), spawn_(spawn), process_operations_(operations) {}
auto CurrentProcessImage::acquire() noexcept -> std::expected<CurrentProcessImage, ImageFailure> {
    return CurrentImageAccess::acquire(
        production_image_operations(), production_descriptor_operations(),
        production_spawn_operations(), production_process_operations());
}
auto CurrentProcessImage::active() const noexcept -> bool { return descriptor_.active(); }
auto CurrentProcessImage::close() noexcept -> std::expected<void, ImageFailure> {
    const auto result = DescriptorAccess::close_raw(descriptor_);
    if (failed(result))
        return std::unexpected{native_failure(ImageStage::close, result)};
    return {};
}
auto CurrentProcessImage::spawn(std::span<const std::string_view> arguments,
                                const RunnerIpcEndpoints& endpoints) noexcept
    -> std::expected<ChildProcess, ImageFailure> {
    auto stage = ImageStage::pid;
    try {
        if (!active())
            return std::unexpected{failure(ImageStage::recheck, ImageReason::invalid_state)};
        const auto pid = image_.current_pid(image_.context);
        if (pid.error_number || pid.value <= 0 || pid.value > std::numeric_limits<int>::max())
            return std::unexpected{native_failure(ImageStage::pid, pid)};
        if (pid.value != process_)
            return std::unexpected{failure(ImageStage::recheck, ImageReason::wrong_process)};
        stage = ImageStage::arguments;
        std::size_t bytes{};
        for (auto argument : arguments) {
            if (argument.size() == SIZE_MAX || argument.size() + 1U > SIZE_MAX - bytes)
                return std::unexpected{failure(ImageStage::arguments, ImageReason::size_overflow)};
            bytes += argument.size() + 1U;
        }
        auto stock = CurrentImageAccess::argument_stock(arguments.size(), bytes);
        if (!stock)
            return std::unexpected{std::move(stock.error())};
        stage = ImageStage::recheck;
        ImageMetadata current{};
        const auto result =
            image_.stat_image(image_.context, descriptor_.native_handle(), &current);
        if (failed(result))
            return std::unexpected{native_failure(ImageStage::recheck, result)};
        if (!admitted(current) || !same(metadata_, current))
            return std::unexpected{failure(ImageStage::recheck, ImageReason::metadata_changed)};
        stage = ImageStage::arguments;
        ChildSpawnRequest request{std::string{alias_.data()}, {}};
        request.arguments.reserve(arguments.size());
        for (auto argument : arguments)
            request.arguments.emplace_back(argument);
        stage = ImageStage::spawn;
        auto child = ProcessAccess::spawn(request, endpoints, spawn_, process_operations_);
        if (!child) {
            auto error = failure(ImageStage::spawn, ImageReason::spawn_failure);
            if (child.error().os_error)
                error.error_number = child.error().os_error->value;
            if (child.error().code.value() == "linux.process.spawn_failed" &&
                (error.error_number == EACCES || error.error_number == EPERM ||
                 error.error_number == ENOENT || error.error_number == ENOTDIR ||
                 error.error_number == ENOSYS))
                error.reason = ImageReason::unsupported_access;
            error.lower.emplace(std::move(child.error()));
            return std::unexpected{std::move(error)};
        }
        return std::move(*child);
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(stage, ImageReason::allocation_failure)};
    } catch (...) {
        return std::unexpected{failure(stage, ImageReason::contained_failure)};
    }
}
} // namespace epycbench::platform_linux::detail
