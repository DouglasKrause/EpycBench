#pragma once
#include "monitoring/host_q1_git_command_owner.hpp"
#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
namespace epycbench::monitoring::qualification::detail {
struct MonitoringQ1GitPollEvent final {
    int descriptor;
    short revents;
};
struct MonitoringQ1GitBoundedChildOperations final {
    void* context{};
    int (*poll)(void*, MonitoringQ1GitPollEvent*, std::size_t) noexcept {};
    long (*read)(void*, int, void*, std::size_t) noexcept {};
    int (*kill)(void*, int, int) noexcept {};
    int (*wait)(void*, int, int*) noexcept {};
    void (*await_outer_termination)(void*) noexcept {};
    int (*close)(void*, int) noexcept {};
    void (*adopt)(void*) noexcept {};
    void* adopt_context{};
};
struct MonitoringQ1GitBoundedOutput final {
    std::string standard_output, standard_error;
};
struct MonitoringQ1GitInitialCloseFailures final {
    std::array<int, 4> errors{};
    std::size_t count{};
};
class MonitoringQ1GitBoundedChild final {
  private:
    class Prepared final {
      public:
        Prepared(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        auto operator=(const Prepared&) -> Prepared& = delete;
        auto operator=(Prepared&&) -> Prepared& = delete;

      private:
        struct State;
        explicit Prepared(std::unique_ptr<State>) noexcept;
        std::unique_ptr<State> state_;
        friend class MonitoringQ1GitBoundedChild;
        friend class MonitoringQ1GitSpawn;
    };
    friend class MonitoringQ1GitBoundedChildTestAccess;
    friend class MonitoringQ1GitSpawn;
    static auto prepare(std::size_t stdout_limit = 1U << 20U) -> Prepared;
    static auto collect(Prepared&&, std::array<int, 4>,
                        const MonitoringQ1GitBoundedChildOperations&,
                        MonitoringQ1GitInitialCloseFailures = {})
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
};
} // namespace epycbench::monitoring::qualification::detail
