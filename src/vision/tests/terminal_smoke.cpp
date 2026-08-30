#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace
{

struct Invocation
{
    std::string terminal;
    std::string config_root;
    std::vector<char *> command;
};

bool same_terminal_mode(const termios &left, const termios &right)
{
    // PENDIN is a kernel-maintained input-queue state bit, not part of the
    // terminal configuration that a program owns. Sending the quit chord can
    // legitimately leave it set while the line discipline retypes input.
    tcflag_t transient_local_flags = 0;
#ifdef PENDIN
    transient_local_flags |= PENDIN;
#endif
    return left.c_iflag == right.c_iflag && left.c_oflag == right.c_oflag && left.c_cflag == right.c_cflag &&
           (left.c_lflag & ~transient_local_flags) == (right.c_lflag & ~transient_local_flags) &&
           cfgetispeed(&left) == cfgetispeed(&right) &&
           cfgetospeed(&left) == cfgetospeed(&right) &&
           std::equal(std::begin(left.c_cc), std::end(left.c_cc), std::begin(right.c_cc));
}

std::string terminal_mode_summary(const termios &mode)
{
    return "iflag=" + std::to_string(mode.c_iflag) + " oflag=" + std::to_string(mode.c_oflag) +
           " cflag=" + std::to_string(mode.c_cflag) + " lflag=" + std::to_string(mode.c_lflag) +
           " ispeed=" + std::to_string(cfgetispeed(&mode)) + " ospeed=" + std::to_string(cfgetospeed(&mode));
}

void close_fd(int &descriptor) noexcept
{
    if (descriptor >= 0)
        (void)::close(descriptor);
    descriptor = -1;
}

void reap_after_failure(pid_t child) noexcept
{
    if (child <= 0)
        return;
    (void)::kill(child, SIGTERM);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR)
    {
    }
}

bool parse_invocation(int argc, char **argv, Invocation &result)
{
    int index = 1;
    while (index < argc && std::string_view(argv[index]) != "--")
    {
        const std::string_view option(argv[index++]);
        if (index >= argc)
            return false;
        if (option == "--term")
            result.terminal = argv[index++];
        else if (option == "--config-root")
            result.config_root = argv[index++];
        else
            return false;
    }
    if (index >= argc || std::string_view(argv[index++]) != "--" || result.terminal.empty() || result.config_root.empty() ||
        index >= argc)
        return false;

    result.command.assign(argv + index, argv + argc);
    result.command.push_back(nullptr);
    return true;
}

int report_failure(std::string_view message)
{
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv)
{
    Invocation invocation;
    if (!parse_invocation(argc, argv, invocation))
        return report_failure("Usage: ck_vision_terminal_smoke --term TERM --config-root PATH -- EXECUTABLE [ARGS...]");

    int master = -1;
    int slave = -1;
    winsize initial_size{};
    initial_size.ws_row = 24;
    initial_size.ws_col = 80;
    if (::openpty(&master, &slave, nullptr, nullptr, &initial_size) != 0)
        return report_failure("Could not allocate a pseudo-terminal.");

    int mode_probe = ::dup(slave);
    if (mode_probe < 0)
    {
        close_fd(master);
        close_fd(slave);
        return report_failure("Could not retain a pseudo-terminal mode probe.");
    }

    termios original_mode{};
    if (::tcgetattr(mode_probe, &original_mode) != 0)
    {
        close_fd(master);
        close_fd(slave);
        close_fd(mode_probe);
        return report_failure("Could not read the pseudo-terminal mode before startup.");
    }

    // Keep a child helper alive as the session leader while the product runs.
    // Otherwise macOS resets the PTY device as the product's controlling
    // session disappears, so a post-exit tcgetattr would observe the reset
    // device rather than the mode restored by ckVision. Forking first also
    // guarantees setsid can succeed: a process-group leader may not call it.
    const pid_t session_owner = ::fork();
    if (session_owner < 0)
    {
        close_fd(master);
        close_fd(slave);
        close_fd(mode_probe);
        return report_failure("Could not create the pseudo-terminal control session.");
    }
    if (session_owner > 0)
    {
        close_fd(master);
        close_fd(slave);
        close_fd(mode_probe);
        int owner_status = 0;
        pid_t waited = -1;
        do
        {
            waited = ::waitpid(session_owner, &owner_status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != session_owner)
            return report_failure("Could not collect the pseudo-terminal control session.");
        if (WIFSIGNALED(owner_status))
            return report_failure("The pseudo-terminal control session was terminated by signal " +
                                  std::to_string(WTERMSIG(owner_status)) + ".");
        if (!WIFEXITED(owner_status) || WEXITSTATUS(owner_status) != EXIT_SUCCESS)
            return report_failure("The pseudo-terminal control session did not complete successfully.");
        return EXIT_SUCCESS;
    }

    if (::setsid() < 0 || ::ioctl(slave, TIOCSCTTY, 0) != 0 || ::signal(SIGHUP, SIG_IGN) == SIG_ERR)
    {
        close_fd(master);
        close_fd(slave);
        close_fd(mode_probe);
        return report_failure("Could not establish the pseudo-terminal control session.");
    }

    const pid_t child = ::fork();
    if (child < 0)
    {
        close_fd(master);
        close_fd(slave);
        close_fd(mode_probe);
        return report_failure("Could not fork the terminal-profile child.");
    }
    if (child == 0)
    {
        close_fd(master);
        close_fd(mode_probe);
        (void)::signal(SIGHUP, SIG_DFL);
        if (::dup2(slave, STDIN_FILENO) < 0 || ::dup2(slave, STDOUT_FILENO) < 0 ||
            ::dup2(slave, STDERR_FILENO) < 0 ||
            ::setenv("TERM", invocation.terminal.c_str(), 1) != 0 ||
            ::setenv("XDG_CONFIG_HOME", invocation.config_root.c_str(), 1) != 0)
            _exit(127);
        if (slave > STDERR_FILENO)
            (void)::close(slave);
        ::execv(invocation.command.front(), invocation.command.data());
        _exit(127);
    }

    close_fd(slave);
    const int original_flags = ::fcntl(master, F_GETFL);
    if (original_flags < 0 || ::fcntl(master, F_SETFL, original_flags | O_NONBLOCK) != 0)
    {
        reap_after_failure(child);
        close_fd(master);
        close_fd(mode_probe);
        return report_failure("Could not make the pseudo-terminal drain nonblocking.");
    }

    const auto started = std::chrono::steady_clock::now();
    bool resized = false;
    bool quit_sent = false;
    int child_status = 0;
    bool exited = false;
    while (!exited)
    {
        const pid_t waited = ::waitpid(child, &child_status, WNOHANG);
        if (waited == child)
        {
            exited = true;
            break;
        }
        if (waited < 0 && errno != EINTR)
        {
            close_fd(master);
            close_fd(mode_probe);
            return report_failure("Could not collect the terminal-profile child.");
        }

        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (!resized && elapsed >= std::chrono::milliseconds(250))
        {
            winsize resized_size{};
            resized_size.ws_row = 30;
            resized_size.ws_col = 100;
            if (::ioctl(master, TIOCSWINSZ, &resized_size) != 0 || ::kill(child, SIGWINCH) != 0)
            {
                reap_after_failure(child);
                close_fd(master);
                close_fd(mode_probe);
                return report_failure("Could not deliver the pseudo-terminal resize.");
            }
            resized = true;
        }

        if (!quit_sent && elapsed >= std::chrono::milliseconds(450))
        {
            constexpr std::array<char, 2> quit = {'\x1b', 'x'};
            const ssize_t written = ::write(master, quit.data(), quit.size());
            if (written != static_cast<ssize_t>(quit.size()))
            {
                reap_after_failure(child);
                close_fd(master);
                close_fd(mode_probe);
                return report_failure("Could not send the standard Alt+X quit chord.");
            }
            quit_sent = true;
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(master, &readable);
        timeval timeout{.tv_sec = 0, .tv_usec = 50'000};
        const int ready = ::select(master + 1, &readable, nullptr, nullptr, &timeout);
        if (ready > 0 && FD_ISSET(master, &readable))
        {
            std::array<char, 16 * 1024> discard{};
            while (::read(master, discard.data(), discard.size()) > 0)
            {
            }
        }
        else if (ready < 0 && errno != EINTR)
        {
            reap_after_failure(child);
            close_fd(master);
            close_fd(mode_probe);
            return report_failure("Could not drain the pseudo-terminal output.");
        }

        if (elapsed >= std::chrono::seconds(10))
        {
            reap_after_failure(child);
            close_fd(master);
            close_fd(mode_probe);
            return report_failure("The terminal-profile child did not exit after the standard quit chord.");
        }
    }

    termios restored_mode{};
    const bool restored = ::tcgetattr(mode_probe, &restored_mode) == 0 && same_terminal_mode(original_mode, restored_mode);
    close_fd(master);
    close_fd(mode_probe);

    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != EXIT_SUCCESS)
        return report_failure("The terminal-profile child did not exit successfully.");
    if (!resized || !quit_sent)
        return report_failure("The terminal-profile child exited before resize and standard-quit verification.");
    if (!restored)
        return report_failure("The terminal-profile child did not restore the pseudo-terminal mode. Before: " +
                              terminal_mode_summary(original_mode) + "; after: " + terminal_mode_summary(restored_mode));
    return EXIT_SUCCESS;
}
