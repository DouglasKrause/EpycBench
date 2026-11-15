#pragma once

#include "epycbench/platform_linux/local_ipc.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"

namespace epycbench::runtime::detail {

class EndpointByteReader final : public ipc::ByteReader {
  public:
    explicit EndpointByteReader(platform_linux::LocalIpcEndpoint& endpoint) noexcept;
    EndpointByteReader() = delete;
    EndpointByteReader(const EndpointByteReader&) = delete;
    auto operator=(const EndpointByteReader&) -> EndpointByteReader& = delete;
    EndpointByteReader(EndpointByteReader&&) = delete;
    auto operator=(EndpointByteReader&&) -> EndpointByteReader& = delete;

    auto read(std::span<std::byte> buffer)
        -> std::expected<std::size_t, core::Error> override;

  private:
    platform_linux::LocalIpcEndpoint* endpoint_;
};

class EndpointByteWriter final : public ipc::ByteWriter {
  public:
    explicit EndpointByteWriter(platform_linux::LocalIpcEndpoint& endpoint) noexcept;
    EndpointByteWriter() = delete;
    EndpointByteWriter(const EndpointByteWriter&) = delete;
    auto operator=(const EndpointByteWriter&) -> EndpointByteWriter& = delete;
    EndpointByteWriter(EndpointByteWriter&&) = delete;
    auto operator=(EndpointByteWriter&&) -> EndpointByteWriter& = delete;

    auto write(std::span<const std::byte> buffer)
        -> std::expected<std::size_t, core::Error> override;

  private:
    platform_linux::LocalIpcEndpoint* endpoint_;
};

} // namespace epycbench::runtime::detail
