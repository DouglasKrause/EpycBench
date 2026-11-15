#pragma once

#include "epycbench/kernel/plugin.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::platform_linux::kernel_modules {
enum class Error { directory, open, symbol, incompatible, metadata, duplicate_id, io, allocation };
struct Failure {
    Error code;
    std::filesystem::path path;
    std::string detail;
};
struct Metadata {
    std::string kernel_id, implementation_id, version, compiler, build_info, timed_symbol, sha256;
    std::uint32_t family{};
    std::filesystem::path path;
};

// Own the shared object until every borrower of api() has finished. Loading and
// destruction are session preparation/teardown operations, never timed work.
class Module {
  public:
    Module(Module&&) noexcept;
    auto operator=(Module&&) noexcept -> Module&;
    ~Module();
    Module(const Module&) = delete;
    auto operator=(const Module&) -> Module& = delete;

    [[nodiscard]] static auto load(const std::filesystem::path&) -> std::expected<Module, Failure>;
    [[nodiscard]] auto metadata() const noexcept -> const Metadata&;
    [[nodiscard]] auto api() const noexcept -> const EpycBenchKernelV1&;

  private:
    struct State;
    explicit Module(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
};

class Catalog {
  public:
    Catalog() = default;
    Catalog(Catalog&&) noexcept = default;
    auto operator=(Catalog&&) noexcept -> Catalog& = default;
    Catalog(const Catalog&) = delete;
    auto operator=(const Catalog&) -> Catalog& = delete;

    // A missing directory produces an empty catalog. The run caller diagnoses
    // an unavailable selected kernel; help and offline results need no modules.
    [[nodiscard]] static auto discover(const std::filesystem::path&) -> std::expected<Catalog, Failure>;
    [[nodiscard]] auto modules() const noexcept -> std::span<const Module> { return modules_; }
    [[nodiscard]] auto find(std::string_view kernel_id) const noexcept -> const Module*;

  private:
    std::vector<Module> modules_;
};

[[nodiscard]] auto default_directory() -> std::expected<std::filesystem::path, Failure>;
} // namespace epycbench::platform_linux::kernel_modules
