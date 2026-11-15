#pragma once

#include "epycbench/core/error.hpp"

#include <expected>

namespace epycbench::platform_linux {

namespace detail {
struct RawStatusResult;
class DescriptorAccess;
} // namespace detail

class FileDescriptor {
  public:
    [[nodiscard]] static auto adopt(int descriptor)
        -> std::expected<FileDescriptor, core::Error>;

    FileDescriptor(FileDescriptor&& other) noexcept;
    auto operator=(FileDescriptor&& other) noexcept -> FileDescriptor&;
    FileDescriptor(const FileDescriptor&) = delete;
    auto operator=(const FileDescriptor&) -> FileDescriptor& = delete;
    ~FileDescriptor() noexcept;

    [[nodiscard]] auto native_handle() const noexcept -> int;
    [[nodiscard]] auto active() const noexcept -> bool;
    [[nodiscard]] auto close() -> std::expected<void, core::Error>;

  private:
    using CloseFunction = detail::RawStatusResult (*)(void*, int) noexcept;

    FileDescriptor(int descriptor, void* context, CloseFunction close_function) noexcept;
    void best_effort_close() noexcept;

    int descriptor_{-1};
    void* context_{nullptr};
    CloseFunction close_{nullptr};

    friend class detail::DescriptorAccess;
};

} // namespace epycbench::platform_linux
