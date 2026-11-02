#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
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
    bool files_input_contract = false;
    std::vector<char *> command;
};

struct ContractFiles
{
    std::vector<std::filesystem::path> paths;
    bool owns_cleanup = true;

    ~ContractFiles()
    {
        if (!owns_cleanup)
            return;
        for (const auto &path : paths)
        {
            std::error_code error;
            (void)std::filesystem::remove(path, error);
        }
    }
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
        if (option == "--files-input-contract")
            result.files_input_contract = true;
        else if (option == "--term" || option == "--config-root")
        {
            if (index >= argc)
                return false;
            if (option == "--term")
                result.terminal = argv[index++];
            else
                result.config_root = argv[index++];
        }
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

bool prepare_contract_files(const Invocation &invocation, ContractFiles &files, std::string &failure)
{
    if (!invocation.files_input_contract)
        return true;
    if (invocation.command.size() < 4)
    {
        failure = "The files input contract requires an executable and left/right directory arguments.";
        return false;
    }

    const std::filesystem::path left(invocation.command[1]);
    const std::filesystem::path right(invocation.command[2]);
    for (const auto &directory : {left, right})
    {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error))
        {
            failure = "The files input contract requires existing left/right directories.";
            return false;
        }
    }

    const std::array<std::filesystem::path, 2> targets = {
        left / "a-mouse-target.txt",
        left / "b-mouse-target.txt",
    };
    const std::array<std::string_view, 2> contents = {"a\n", "b\n"};
    for (std::size_t index = 0; index < targets.size(); ++index)
    {
        const std::string path = targets[index].string();
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0)
        {
            failure = "The files input contract refused to overwrite an existing fixture path: " + path;
            return false;
        }
        const std::string_view content = contents[index];
        const ssize_t written = ::write(descriptor, content.data(), content.size());
        const int close_result = ::close(descriptor);
        if (written != static_cast<ssize_t>(content.size()) || close_result != 0)
        {
            failure = "Could not create the files input contract fixture: " + path;
            return false;
        }
        files.paths.push_back(targets[index]);
    }
    return true;
}

int report_failure(std::string_view message)
{
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

bool contains_marked_one(std::string_view transcript)
{
    std::size_t search_from = 0;
    while (true)
    {
        const std::size_t marker = transcript.find("Marked:", search_from);
        if (marker == std::string_view::npos)
            return false;
        const std::size_t end = std::min(transcript.size(), marker + 256);
        std::string rendered;
        rendered.reserve(end - marker);
        for (std::size_t index = marker; index < end; ++index)
        {
            if (transcript[index] == '\x1b' && index + 1 < end && transcript[index + 1] == '[')
            {
                index += 2;
                while (index < end && (transcript[index] < '@' || transcript[index] > '~'))
                    ++index;
                continue;
            }
            if (transcript[index] >= ' ')
                rendered.push_back(transcript[index]);
        }
        if (rendered.find("Marked:1") != std::string::npos || rendered.find("Marked: 1") != std::string::npos)
            return true;
        search_from = marker + 7;
    }
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    Invocation invocation;
    if (!parse_invocation(argc, argv, invocation))
        return report_failure("Usage: ck_vision_terminal_smoke [--files-input-contract] --term TERM --config-root PATH -- EXECUTABLE [ARGS...]");

    ContractFiles contract_files;
    std::string contract_failure;
    if (!prepare_contract_files(invocation, contract_files, contract_failure))
        return report_failure(contract_failure);

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

    contract_files.owns_cleanup = false;

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
        contract_files.owns_cleanup = false;
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
    bool interaction_sent = false;
    bool interaction_observed = false;
    std::chrono::steady_clock::time_point mark_observed_at{};
    int child_status = 0;
    bool exited = false;
    std::string transcript;
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

        if (invocation.files_input_contract && !interaction_sent && elapsed >= std::chrono::milliseconds(450) &&
            transcript.find("a-mouse-target.txt") != std::string::npos &&
            transcript.find("b-mouse-target.txt") != std::string::npos)
        {
            constexpr std::string_view input = "\x1b[<0;10;7M"
                                               "\x1b[<0;10;7m"
                                               "\x1b[2~";
            const ssize_t written = ::write(master, input.data(), input.size());
            if (written != static_cast<ssize_t>(input.size()))
            {
                reap_after_failure(child);
                close_fd(master);
                close_fd(mode_probe);
                return report_failure("Could not send the files row-selection contract.");
            }
            interaction_sent = true;
        }

        if (invocation.files_input_contract && interaction_sent && !interaction_observed &&
            contains_marked_one(transcript))
        {
            interaction_observed = true;
            mark_observed_at = std::chrono::steady_clock::now();
        }

        if (invocation.files_input_contract && interaction_observed && !quit_sent &&
            std::chrono::steady_clock::now() - mark_observed_at >= std::chrono::milliseconds(250))
        {
            constexpr std::string_view quit = "\x1bx";
            const ssize_t written = ::write(master, quit.data(), quit.size());
            if (written != static_cast<ssize_t>(quit.size()))
            {
                reap_after_failure(child);
                close_fd(master);
                close_fd(mode_probe);
                return report_failure("Could not send the files contract quit chord.");
            }
            quit_sent = true;
        }

        if (!invocation.files_input_contract && !quit_sent && elapsed >= std::chrono::milliseconds(450))
        {
            // Exercise the real terminal input path with an unbound modified
            // key and an SGR mouse press/release on the footer.  The footer
            // cell is outside the file rows, so this cannot select or invoke
            // a file operation; the following Alt+X still proves clean exit.
            constexpr std::string_view input = "\x1b[1;2A"
                                               "\x1b[999~"
                                               "\x1b[<0;99;23M"
                                               "\x1b[<0;99;23m"
                                               "\x1bx";
            const ssize_t written = ::write(master, input.data(), input.size());
            if (written != static_cast<ssize_t>(input.size()))
            {
                reap_after_failure(child);
                close_fd(master);
                close_fd(mode_probe);
                return report_failure("Could not send the terminal input contract.");
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
            std::array<char, 16 * 1024> output{};
            ssize_t read_count = 0;
            while ((read_count = ::read(master, output.data(), output.size())) > 0)
            {
                transcript.append(output.data(), static_cast<std::size_t>(read_count));
                if (transcript.size() > 1U << 20)
                    transcript.erase(0, transcript.size() - (1U << 19));
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
            if (invocation.files_input_contract)
            {
                const std::size_t tail_start = transcript.size() > 12'000 ? transcript.size() - 12'000 : 0;
                std::cerr << "Files input contract timeout; interaction_sent=" << interaction_sent
                          << "; interaction_observed=" << interaction_observed << "; transcript_tail:\n"
                          << transcript.substr(tail_start) << '\n';
            }
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
    if (invocation.files_input_contract && (!interaction_sent || !interaction_observed))
        return report_failure("The files input contract did not observe a functional row selection.");
    if (!restored)
        return report_failure("The terminal-profile child did not restore the pseudo-terminal mode. Before: " +
                              terminal_mode_summary(original_mode) + "; after: " + terminal_mode_summary(restored_mode));
    return EXIT_SUCCESS;
}
