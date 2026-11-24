#include "platform_linux/workbench_terminal.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace epycbench::platform_linux {
namespace {
// The coordinator's interrupt handler is active during terminal handoff.
int set_mode(const termios& mode) noexcept {
    int result;
    do {
        result = ::tcsetattr(STDIN_FILENO, TCSANOW, &mode);
    } while (result < 0 && errno == EINTR);
    return result;
}
int replace_descriptor(int source, int destination) noexcept {
    int result;
    do {
        result = ::dup2(source, destination);
    } while (result < 0 && errno == EINTR);
    return result;
}
} // namespace

WorkbenchScreenSignals::WorkbenchScreenSignals() {
    sigset_t exits;
    ::sigemptyset(&exits);
    ::sigaddset(&exits, SIGINT);
    ::sigaddset(&exits, SIGTERM);
    const int failure = ::pthread_sigmask(SIG_BLOCK, &exits, &original_);
    if (failure)
        throw std::system_error(failure, std::generic_category(), "Mask fullscreen interrupts");
}
bool WorkbenchScreenSignals::exit_requested() const {
    sigset_t pending;
    if (::sigpending(&pending) != 0)
        throw std::system_error(errno, std::generic_category(), "Read fullscreen interrupts");
    for (int number : {SIGINT, SIGTERM})
        if (::sigismember(&pending, number) == 1 && ::sigismember(&original_, number) == 0)
            return true;
    return false;
}
WorkbenchScreenSignals::~WorkbenchScreenSignals() {
    ::pthread_sigmask(SIG_SETMASK, &original_, nullptr);
}

std::expected<bool, std::string> WorkbenchAttemptTerminal::open(std::string_view notice) {
    if (::tcgetattr(STDIN_FILENO, &before_) != 0)
        return std::unexpected{std::string(std::strerror(errno))};
    auto mode = before_;
    mode.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    mode.c_lflag |= ISIG;
    mode.c_cc[VMIN] = 0;
    mode.c_cc[VTIME] = 0;
    if (set_mode(mode) != 0)
        return std::unexpected{std::string(std::strerror(errno))};
    term_active_ = true;
    if (pause_requested())
        return false;
    std::fwrite(notice.data(), 1, notice.size(), stdout);
    std::fflush(stdout);
    // Ordinary child commands inherit stdout/stderr. A regular anonymous file
    // needs no draining thread and cannot block dispatch on a full pipe.
    capture_ = std::tmpfile();
    if (!capture_)
        return std::unexpected{std::string(std::strerror(errno))};
    out_ = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    err_ = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    if (out_ < 0 || err_ < 0 || replace_descriptor(::fileno(capture_), STDOUT_FILENO) < 0 ||
        replace_descriptor(::fileno(capture_), STDERR_FILENO) < 0)
        return std::unexpected{std::string(std::strerror(errno))};
    return true;
}
bool WorkbenchAttemptTerminal::pause_requested() {
    std::array<char, 512> buffer{};
    bool pause = false;
    ssize_t count{};
    while ((count = ::read(STDIN_FILENO, buffer.data(), buffer.size())) > 0) {
        pause |= std::any_of(buffer.begin(), buffer.begin() + count,
                             [](char c) { return c == 'p' || c == 'P' || c == '\x03'; });
    }
    return pause;
}
std::string WorkbenchAttemptTerminal::diagnostic_tail() {
    if (!capture_)
        return {};
    std::array<char, 2048> bytes{};
    auto length = ::lseek(::fileno(capture_), 0, SEEK_END);
    if (length <= 0)
        return {};
    auto n = ::pread(::fileno(capture_), bytes.data(), bytes.size(),
                     std::max<off_t>(0, length - static_cast<off_t>(bytes.size())));
    return n > 0 ? std::string(bytes.data(), static_cast<std::size_t>(n)) : std::string{};
}
std::error_code WorkbenchAttemptTerminal::restore() noexcept {
    std::error_code first_error;
    auto restore_fd = [&first_error](int& saved, int target) {
        if (saved < 0)
            return;
        if (replace_descriptor(saved, target) < 0) {
            if (!first_error)
                first_error = {errno, std::generic_category()};
            return;
        }
        ::close(saved);
        saved = -1;
    };
    restore_fd(out_, STDOUT_FILENO);
    restore_fd(err_, STDERR_FILENO);
    if (term_active_) {
        if (set_mode(before_) == 0)
            term_active_ = false;
        else if (!first_error)
            first_error = {errno, std::generic_category()};
    }
    return first_error;
}
WorkbenchAttemptTerminal::~WorkbenchAttemptTerminal() {
    static_cast<void>(restore());
    if (out_ >= 0)
        ::close(out_);
    if (err_ >= 0)
        ::close(err_);
    if (capture_)
        std::fclose(capture_);
}
} // namespace epycbench::platform_linux
