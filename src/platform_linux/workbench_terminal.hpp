#pragma once
#include <cstdio>
#include <expected>
#include <signal.h>
#include <string>
#include <string_view>
#include <system_error>
#include <termios.h>

namespace epycbench::platform_linux {
// Keep process-directed interrupts pending through fullscreen teardown. All
// FTXUI listeners inherit the mask. Restoration delivers pending signals to
// the caller's disposition before the next coordinator can reserve a trial.
class WorkbenchScreenSignals {
  public:
    WorkbenchScreenSignals();
    WorkbenchScreenSignals(const WorkbenchScreenSignals&) = delete;
    WorkbenchScreenSignals& operator=(const WorkbenchScreenSignals&) = delete;
    ~WorkbenchScreenSignals();
    [[nodiscard]] bool exit_requested() const;

  private:
    sigset_t original_{};
};

// Only installed after the FTXUI loop and its listeners have retired.
class WorkbenchAttemptTerminal {
  public:
    WorkbenchAttemptTerminal() = default;
    WorkbenchAttemptTerminal(const WorkbenchAttemptTerminal&) = delete;
    WorkbenchAttemptTerminal& operator=(const WorkbenchAttemptTerminal&) = delete;
    ~WorkbenchAttemptTerminal();
    // False means a pause/interrupt was already pending at the UI handoff.
    std::expected<bool, std::string> open(std::string_view notice);
    // Read pending keyboard input once, after the coordinator returns.
    bool pause_requested();
    [[nodiscard]] std::error_code restore() noexcept;
    std::string diagnostic_tail();

  private:
    termios before_{};
    bool term_active_{};
    std::FILE* capture_{};
    int out_{-1}, err_{-1};
};
} // namespace epycbench::platform_linux
