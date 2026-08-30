#include "find_custom_command.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include "ck/find/cli_buffer_utils.hpp"

#if defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ck::vision
{
namespace
{

constexpr std::size_t kMaximumOutputBytes = 16 * 1024;
constexpr auto kCommandTimeout = std::chrono::seconds(5);
constexpr std::string_view kSandboxExecutable = "/usr/bin/sandbox-exec";

struct CommandTemplate
{
    std::string_view identifier;
    std::string_view executable;
    std::array<std::string_view, 3> arguments;
    std::size_t argument_count;
};

constexpr std::array kCommandTemplates{
    CommandTemplate{"ckvision.file-info", "/usr/bin/stat", {"-f", "%N\\t%z\\t%Sp", "<matched-path>"}, 3},
    CommandTemplate{"ckvision.sha256", "/usr/bin/shasum", {"-a", "256", "<matched-path>"}, 3},
};

const CommandTemplate *find_template(std::string_view identifier)
{
    const auto found = std::find_if(kCommandTemplates.begin(), kCommandTemplates.end(), [identifier](const CommandTemplate &candidate) {
        return candidate.identifier == identifier;
    });
    return found == kCommandTemplates.end() ? nullptr : &*found;
}

bool contains_profile_unsafe_character(const std::string &value)
{
    return std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20 || character == '\\' || character == '"';
    });
}

std::string sandbox_profile(const FindCustomCommandPlan &plan)
{
    const std::string root = plan.working_directory.string();
    std::error_code canonical_error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(plan.working_directory, canonical_error);
    const std::string canonical_root_string = canonical_error ? root : canonical_root.string();
    const std::string executable = plan.executable_argv.front();
    return "(version 1)\n"
           // Apple's system profile supplies the minimal dyld, descriptor,
           // and path-resolution allowances a Unix utility needs. This adds
           // no user-data read scope; the explicit rules below deny network
           // and all writes, then permit only the selected search root and
           // fixed executable.
           "(import \"system.sb\")\n"
           "(deny network*)\n"
           "(deny file-write*)\n"
           "(deny process-exec)\n"
           "(allow process-exec (literal \"" + executable + "\"))\n"
           "(allow file-read* (subpath \"" + root + "\"))\n"
           // APFS firmlinks commonly resolve a selected `/var` root through
           // `/private/var`; allow both spellings of the same root, never a
           // parent directory or an unrelated read location.
           "(allow file-read* (subpath \"" + canonical_root_string + "\"))\n"
           "(allow file-read* (literal \"" + executable + "\"))\n";
}

FindCustomCommandCapability unavailable(std::string reason)
{
    return {.available = false, .reason = std::move(reason), .argv_preview = {}};
}

class UnsupportedFindCustomCommandExecutor final : public FindCustomCommandExecutor
{
public:
    FindCustomCommandCapability capability(const ck::find::SearchSpecification &) const override
    {
        return unavailable("Custom commands require the tested macOS sandbox executor and are unavailable on this platform.");
    }

    FindCustomCommandOutcome execute(const FindCustomCommandPlan &,
                                     const std::filesystem::path &,
                                     const std::atomic_bool &) override
    {
        return {.invoked = false,
                .cancelled = false,
                .timed_out = false,
                .output_truncated = false,
                .exit_code = -1,
                .output = {},
                .message = "Custom commands are unavailable on this platform."};
    }
};

#if defined(__APPLE__)

class MacSandboxedFindCustomCommandExecutor final : public FindCustomCommandExecutor
{
public:
    FindCustomCommandCapability capability(const ck::find::SearchSpecification &specification) const override
    {
        const FindCustomCommandPlan plan = planFindCustomCommand(specification);
        if (!plan.allowed)
            return unavailable(plan.reason);
        std::error_code error;
        const std::filesystem::file_status sandbox_status = std::filesystem::status(kSandboxExecutable, error);
        if (error || !std::filesystem::is_regular_file(sandbox_status))
            return unavailable("Custom commands remain disabled because the required macOS sandbox executor is unavailable.");
        const std::string root = plan.working_directory.string();
        std::error_code canonical_error;
        const std::string canonical_root = std::filesystem::weakly_canonical(plan.working_directory, canonical_error).string();
        if (contains_profile_unsafe_character(root) || (!canonical_error && contains_profile_unsafe_character(canonical_root)))
            return unavailable("The selected search root contains a character that cannot be safely represented in the sandbox profile.");

        std::vector<std::string> preview;
        preview.reserve(plan.executable_argv.size() + 4);
        preview.emplace_back(kSandboxExecutable);
        preview.emplace_back("-p");
        preview.push_back(sandbox_profile(plan));
        preview.emplace_back("--");
        preview.insert(preview.end(), plan.executable_argv.begin(), plan.executable_argv.end());
        return {.available = true, .reason = {}, .argv_preview = std::move(preview)};
    }

    FindCustomCommandOutcome execute(const FindCustomCommandPlan &plan,
                                     const std::filesystem::path &matched_path,
                                     const std::atomic_bool &cancellation) override
    {
        if (!plan.allowed)
            return {.message = plan.reason};
        if (cancellation.load(std::memory_order_acquire))
            return {.cancelled = true, .message = "Custom command cancelled before it started."};
        const std::string root = plan.working_directory.string();
        std::error_code canonical_error;
        const std::string canonical_root = std::filesystem::weakly_canonical(plan.working_directory, canonical_error).string();
        if (contains_profile_unsafe_character(root) || (!canonical_error && contains_profile_unsafe_character(canonical_root)))
            return {.message = "The selected search root cannot be safely represented in the sandbox profile."};

        std::vector<std::string> command;
        command.reserve(plan.executable_argv.size() + 4);
        command.emplace_back(kSandboxExecutable);
        command.emplace_back("-p");
        command.push_back(sandbox_profile(plan));
        command.emplace_back("--");
        command.insert(command.end(), plan.executable_argv.begin(), plan.executable_argv.end());
        command.back() = matched_path.string();

        int pipe_fds[2]{};
        if (pipe(pipe_fds) != 0)
            return {.message = "Could not create a bounded custom-command output pipe."};
        const auto close_fds = [&pipe_fds] {
            if (pipe_fds[0] >= 0)
                close(pipe_fds[0]);
            if (pipe_fds[1] >= 0)
                close(pipe_fds[1]);
            pipe_fds[0] = -1;
            pipe_fds[1] = -1;
        };

        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0)
        {
            close_fds();
            return {.message = "Could not initialize the custom-command sandbox process."};
        }
        const auto destroy_actions = [&actions] { posix_spawn_file_actions_destroy(&actions); };
        const int change_directory_error = add_working_directory_action(&actions, plan.working_directory);
        if (posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO) != 0 ||
            posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO) != 0 ||
            posix_spawn_file_actions_addclose(&actions, pipe_fds[0]) != 0 ||
            posix_spawn_file_actions_addclose(&actions, pipe_fds[1]) != 0 ||
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0 ||
            change_directory_error != 0)
        {
            destroy_actions();
            close_fds();
            return {.message = "Could not configure the custom-command sandbox process."};
        }

        std::vector<char *> argv;
        argv.reserve(command.size() + 1);
        for (std::string &argument : command)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        char path[] = "PATH=/usr/bin:/bin";
        char lang[] = "LANG=C";
        char locale[] = "LC_ALL=C";
        char home[] = "HOME=/var/empty";
        char *const environment[] = {path, lang, locale, home, nullptr};
        pid_t process_id = 0;
        const int spawn_error = posix_spawn(&process_id, command.front().c_str(), &actions, nullptr, argv.data(), environment);
        destroy_actions();
        close(pipe_fds[1]);
        pipe_fds[1] = -1;
        if (spawn_error != 0)
        {
            close_fds();
            return {.message = "Could not start the sandboxed custom command: " + std::string(std::strerror(spawn_error))};
        }

        const int existing_flags = fcntl(pipe_fds[0], F_GETFL, 0);
        if (existing_flags >= 0)
            (void)fcntl(pipe_fds[0], F_SETFL, existing_flags | O_NONBLOCK);

        FindCustomCommandOutcome outcome;
        outcome.invoked = true;
        const auto started = std::chrono::steady_clock::now();
        bool terminated = false;
        bool force_terminated = false;
        std::optional<std::chrono::steady_clock::time_point> termination_requested_at;
        int wait_status = 0;
        for (;;)
        {
            std::array<char, 1024> buffer{};
            for (;;)
            {
                const ssize_t bytes_read = read(pipe_fds[0], buffer.data(), buffer.size());
                if (bytes_read <= 0)
                    break;
                const std::size_t remaining = kMaximumOutputBytes > outcome.output.size()
                                                  ? kMaximumOutputBytes - outcome.output.size()
                                                  : 0;
                const std::size_t accepted = std::min(remaining, static_cast<std::size_t>(bytes_read));
                outcome.output.append(buffer.data(), accepted);
                if (accepted != static_cast<std::size_t>(bytes_read))
                    outcome.output_truncated = true;
            }

            const pid_t waited = waitpid(process_id, &wait_status, WNOHANG);
            if (waited == process_id)
            {
                // The final bytes may arrive between the nonblocking read at
                // the top of this iteration and child termination.
                for (;;)
                {
                    const ssize_t bytes_read = read(pipe_fds[0], buffer.data(), buffer.size());
                    if (bytes_read <= 0)
                        break;
                    const std::size_t remaining = kMaximumOutputBytes > outcome.output.size()
                                                      ? kMaximumOutputBytes - outcome.output.size()
                                                      : 0;
                    const std::size_t accepted = std::min(remaining, static_cast<std::size_t>(bytes_read));
                    outcome.output.append(buffer.data(), accepted);
                    if (accepted != static_cast<std::size_t>(bytes_read))
                        outcome.output_truncated = true;
                }
                break;
            }
            if (waited < 0)
            {
                close_fds();
                return {.invoked = true, .message = "Could not collect the sandboxed custom-command result."};
            }

            const auto now = std::chrono::steady_clock::now();
            const bool cancellation_requested = cancellation.load(std::memory_order_acquire);
            const bool timed_out = now - started >= kCommandTimeout;
            if (!terminated && (cancellation_requested || timed_out || outcome.output_truncated))
            {
                outcome.cancelled = cancellation_requested;
                outcome.timed_out = timed_out;
                (void)kill(process_id, SIGTERM);
                terminated = true;
                termination_requested_at = now;
            }
            else if (terminated && !force_terminated && termination_requested_at.has_value() &&
                     now - *termination_requested_at >= std::chrono::milliseconds(250))
            {
                // Cancellation and time limits must eventually release the
                // joinable worker even if a future allowlisted utility ignores
                // SIGTERM.
                (void)kill(process_id, SIGKILL);
                force_terminated = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        close_fds();

        if (WIFEXITED(wait_status))
            outcome.exit_code = WEXITSTATUS(wait_status);
        else if (WIFSIGNALED(wait_status))
            outcome.exit_code = 128 + WTERMSIG(wait_status);
        if (outcome.cancelled)
            outcome.message = "Custom command cancelled.";
        else if (outcome.timed_out)
            outcome.message = "Custom command exceeded its five-second limit.";
        else if (outcome.output_truncated)
            outcome.message = "Custom command exceeded its 16 KiB output limit.";
        else if (outcome.exit_code == 0)
            outcome.message = "Custom command completed in the macOS sandbox.";
        else
            outcome.message = "Custom command exited with status " + std::to_string(outcome.exit_code) + ".";
        return outcome;
    }

private:
    static int add_working_directory_action(posix_spawn_file_actions_t *actions, const std::filesystem::path &working_directory)
    {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (__builtin_available(macOS 26.0, *))
            return posix_spawn_file_actions_addchdir(actions, working_directory.c_str());
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const int result = posix_spawn_file_actions_addchdir_np(actions, working_directory.c_str());
#pragma clang diagnostic pop
        return result;
    }
};

#endif

} // namespace

FindCustomCommandPlan planFindCustomCommand(const ck::find::SearchSpecification &specification)
{
    if (!specification.enableActionOptions || !specification.actionOptions.execEnabled)
        return {.allowed = false, .reason = "The saved search does not enable a custom command."};
    if (specification.actionOptions.execUsePlus ||
        specification.actionOptions.execVariant != ck::find::ActionOptions::ExecVariant::Exec)
    {
        return {.allowed = false,
                .reason = "Custom commands support only the fixed non-interactive ckvision templates; legacy exec variants are refused."};
    }

    const std::string identifier = ck::find::bufferToString(specification.actionOptions.execCommand);
    const CommandTemplate *const command_template = find_template(identifier);
    if (command_template == nullptr)
    {
        return {.allowed = false,
                .reason = "The saved custom command is not an allowlisted ckvision template. Use ckvision.file-info or ckvision.sha256."};
    }

    std::error_code error;
    const std::filesystem::path requested_root = ck::find::bufferToString(specification.startLocation);
    // Preserve the selected root's absolute spelling. Search results are
    // reported relative to that spelling, and passing a canonicalized root to
    // the sandbox could otherwise deny an equivalent `/var` result path on
    // macOS. No shell expansion or substitution is involved.
    const std::filesystem::path root = std::filesystem::absolute(requested_root, error).lexically_normal();
    if (error || root.empty())
        return {.allowed = false, .reason = "The selected search root cannot be resolved for sandboxed execution."};
    const std::filesystem::file_status root_status = std::filesystem::symlink_status(root, error);
    if (error || !std::filesystem::is_directory(root_status) || std::filesystem::is_symlink(root_status))
        return {.allowed = false, .reason = "Sandboxed custom commands require a concrete, non-symbolic-link search root directory."};

    FindCustomCommandPlan plan;
    plan.allowed = true;
    plan.working_directory = root;
    plan.executable_argv.emplace_back(command_template->executable);
    for (std::size_t index = 0; index < command_template->argument_count; ++index)
        plan.executable_argv.emplace_back(command_template->arguments[index]);
    return plan;
}

std::unique_ptr<FindCustomCommandExecutor> makePlatformFindCustomCommandExecutor()
{
#if defined(__APPLE__)
    return std::make_unique<MacSandboxedFindCustomCommandExecutor>();
#else
    return std::make_unique<UnsupportedFindCustomCommandExecutor>();
#endif
}

} // namespace ck::vision
