#pragma once

#include "epycbench/core/digest.hpp"
#include "platform_linux/process_ipc_operations.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace epycbench::platform_linux::detail {

enum class ImageStage { pid, open, normalization, stat, alias, arguments, recheck, spawn, close, read };
enum class ImageReason {
    invalid_state,
    wrong_process,
    unsupported_access,
    invalid_image,
    metadata_changed,
    native_failure,
    unexpected_native_result,
    size_overflow,
    allocation_failure,
    contained_failure,
    spawn_failure
};
struct ImageCleanupFailure {
    long result{};
    int error_number{};
};
struct ImageFailure {
    ImageStage stage{};
    ImageReason reason{};
    int error_number{};
    std::array<ImageCleanupFailure, 2> cleanup{};
    std::size_t cleanup_count{};
    std::optional<core::Error> lower;
};
class CurrentImageAccess;
class CurrentProcessImage final {
  public:
    [[nodiscard]] static auto acquire() noexcept
        -> std::expected<CurrentProcessImage, ImageFailure>;
    CurrentProcessImage(CurrentProcessImage&&) noexcept = default;
    CurrentProcessImage(const CurrentProcessImage&) = delete;
    auto operator=(CurrentProcessImage&&) -> CurrentProcessImage& = delete;
    auto operator=(const CurrentProcessImage&) -> CurrentProcessImage& = delete;
    ~CurrentProcessImage() = default;
    [[nodiscard]] auto active() const noexcept -> bool;
    [[nodiscard]] auto digest() const -> std::expected<core::Sha256Digest, ImageFailure>;
    [[nodiscard]] auto spawn(std::span<const std::string_view> arguments,
                             const RunnerIpcEndpoints&) noexcept
        -> std::expected<ChildProcess, ImageFailure>;
    [[nodiscard]] auto close() noexcept -> std::expected<void, ImageFailure>;

  private:
    CurrentProcessImage(FileDescriptor&&, int, ImageMetadata, std::array<char, 64>, ImageOperations,
                        SpawnOperations, ProcessOperations) noexcept;
    FileDescriptor descriptor_;
    int process_;
    ImageMetadata metadata_;
    std::array<char, 64> alias_;
    ImageOperations image_;
    SpawnOperations spawn_;
    ProcessOperations process_operations_;
    friend class CurrentImageAccess;
};
class CurrentImageAccess {
  public:
    [[nodiscard]] static auto acquire(ImageOperations, DescriptorOperations, SpawnOperations,
                                      ProcessOperations) noexcept
        -> std::expected<CurrentProcessImage, ImageFailure>;
    [[nodiscard]] static auto render_alias(int process, int descriptor,
                                           std::span<char> output) noexcept
        -> std::expected<std::size_t, ImageFailure>;
    [[nodiscard]] static auto argument_stock(std::size_t count,
                                             std::size_t terminated_bytes) noexcept
        -> std::expected<std::size_t, ImageFailure>;
};
} // namespace epycbench::platform_linux::detail
