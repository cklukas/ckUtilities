#include "find_app.hpp"

#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <utility>
#include <vector>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>

#include "ck/find/cli_buffer_utils.hpp"

namespace
{
void require(bool value, const char *message)
{
    if (value)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

class MemoryStore final : public ck::vision::FindSpecificationStore
{
public:
    std::vector<ck::find::SavedSpecification> list() const override { return {}; }
    std::optional<ck::find::SearchSpecification> load(const std::string &) const override { return std::nullopt; }
    bool save(const ck::find::SearchSpecification &, const std::string &) override { return true; }
};

class ManualExecution final : public ck::vision::FindExecutionService
{
public:
    void start(ck::find::SearchSpecification, bool delete_matched_files, CompletionHandler on_complete) override
    {
        running_ = true;
        cancelled_ = false;
        delete_matched_files_ = delete_matched_files;
        on_complete_ = std::move(on_complete);
    }

    void cancel() noexcept override { cancelled_ = true; }
    bool running() const noexcept override { return running_; }
    ck::vision::FindCustomCommandCapability custom_command_capability(const ck::find::SearchSpecification &specification) const override
    {
        const ck::vision::FindCustomCommandPlan plan = ck::vision::planFindCustomCommand(specification);
        if (!plan.allowed)
            return {.available = false, .reason = plan.reason, .argv_preview = {}};
        return {.available = true,
                .reason = {},
                .argv_preview = {"/usr/bin/sandbox-exec", "-p", "<tested profile>", "--",
                                 plan.executable_argv[0], plan.executable_argv[1], plan.executable_argv[2], plan.executable_argv[3]}};
    }

    void finish(ck::find::SearchExecutionResult result)
    {
        running_ = false;
        on_complete_(std::move(result));
    }

    bool cancelled() const noexcept { return cancelled_; }
    bool delete_matched_files() const noexcept { return delete_matched_files_; }

private:
    bool running_ = false;
    bool cancelled_ = false;
    bool delete_matched_files_ = false;
    CompletionHandler on_complete_;
};

class RecordingCustomCommandExecutor final : public ck::vision::FindCustomCommandExecutor
{
public:
    ck::vision::FindCustomCommandCapability capability(const ck::find::SearchSpecification &specification) const override
    {
        const ck::vision::FindCustomCommandPlan plan = ck::vision::planFindCustomCommand(specification);
        if (!plan.allowed)
            return {.available = false, .reason = plan.reason, .argv_preview = {}};
        return {.available = true,
                .reason = {},
                .argv_preview = {"/usr/bin/sandbox-exec", "-p", "<tested profile>", "--",
                                 plan.executable_argv[0], plan.executable_argv[1], plan.executable_argv[2], plan.executable_argv[3]}};
    }

    ck::vision::FindCustomCommandOutcome execute(const ck::vision::FindCustomCommandPlan &plan,
                                                  const std::filesystem::path &matched_path,
                                                  const std::atomic_bool &) override
    {
        ++invocations_;
        plan_ = plan;
        matched_path_ = matched_path;
        return outcome_;
    }

    std::size_t invocations() const noexcept { return invocations_; }
    const ck::vision::FindCustomCommandPlan &plan() const noexcept { return plan_; }
    const std::filesystem::path &matched_path() const noexcept { return matched_path_; }
    void set_outcome(ck::vision::FindCustomCommandOutcome outcome) { outcome_ = std::move(outcome); }

private:
    std::size_t invocations_ = 0;
    ck::vision::FindCustomCommandPlan plan_;
    std::filesystem::path matched_path_;
    ck::vision::FindCustomCommandOutcome outcome_{.invoked = true,
                                                  .cancelled = false,
                                                  .timed_out = false,
                                                  .output_truncated = false,
                                                  .exit_code = 0,
                                                  .output = "read-only metadata",
                                                  .message = "Custom command completed in the test sandbox."};
};

void verify_late_execution_delivery_is_lifetime_safe()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    MemoryStore specifications;
    ManualExecution execution;
    ckv::ui::CommandId execute_command = ckv::ui::kInvalidCommand;
    {
        ck::vision::FindApp find(application, specifications, execution);
        execute_command = find.execute_command();
        require(application.execute_command(execute_command) && find.execution_running(),
                "The test requires an active native find execution.");
    }
    require(execution.cancelled(), "Destroying Find must request execution cancellation.");
    require(!application.execute_command(execute_command),
            "Destroying Find must withdraw command callbacks that captured the controller.");
    execution.finish({.exitCode = 0, .cancelled = false, .matchCount = 1});
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30},
            "Late Find execution delivery after destruction must be safely ignored.");
}
}

int main()
{
    verify_late_execution_delivery_is_lifetime_safe();

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    MemoryStore specifications;
    ManualExecution execution;
    ck::vision::FindApp find(application, specifications, execution);

    require(find.command_preview().find("find") == 0, "The preview must be built by the search backend.");
    require(application.execute_command(find.preview_command()), "Preview must be a registry command.");
    require(application.execute_command(find.new_search_command()), "New search must be a registry command.");
    require(application.execute_command(find.execute_command()), "Execution must be a registry command.");
    require(find.execution_running(), "Execution must be delegated to the injected service.");
    require(application.execute_command(find.cancel_command()), "Cancellation must be a registry command.");
    require(execution.cancelled(), "Cancellation must be delegated to the injected service.");
    execution.finish({.exitCode = 130, .cancelled = true, .matchCount = 2});
    application.step(0);
    require(find.last_execution_result().has_value(), "Completion must return to the presentation on the UI thread.");
    require(find.last_execution_result()->cancelled, "The cancelled result must be preserved for the UI.");
    require(application.current_frame().size() == ckv::Size{100, 30}, "Find must render headlessly.");

    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "ck-vision-find-execution-service-test";
    fs::remove_all(directory);
    fs::create_directories(directory);
    {
        std::ofstream file(directory / "match.txt");
        file << "match";
    }

    auto destructive_specification = ck::find::makeDefaultSpecification();
    ck::find::copyToArray(destructive_specification.startLocation, directory.string().c_str());
    destructive_specification.enableActionOptions = true;
    destructive_specification.actionOptions.deleteMatches = true;
    find.set_specification(destructive_specification);
    require(application.execute_command(find.execute_command()),
            "Destructive execution must dispatch through the command registry.");
    require(!execution.running(), "Destructive execution must wait for explicit confirmation.");
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    application.step(0);
    require(execution.running() && execution.delete_matched_files(),
            "Accepting the native confirmation must grant only the deletion capability to the execution service.");
    execution.finish({.exitCode = 0, .matchCount = 1, .deletedCount = 1});
    application.step(0);
    require(find.last_execution_result()->deletedCount == 1,
            "Confirmed deletion outcomes must return to the native presentation on the UI thread.");

    auto custom_command_specification = ck::find::makeDefaultSpecification();
    ck::find::copyToArray(custom_command_specification.startLocation, directory.string().c_str());
    custom_command_specification.enableActionOptions = true;
    custom_command_specification.actionOptions.deleteMatches = true;
    custom_command_specification.actionOptions.execEnabled = true;
    ck::find::copyToArray(custom_command_specification.actionOptions.execCommand, "echo must-not-run");
    find.set_specification(custom_command_specification);
    require(application.execute_command(find.execute_command()),
            "Custom-command execution must still dispatch through the command registry.");
    require(!execution.running(), "An unavailable custom command must not start the execution service.");
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    application.step(0);
    require(!execution.running(),
            "An unavailable custom command must take precedence over a requested deletion confirmation.");

    auto safe_custom_command_specification = ck::find::makeDefaultSpecification();
    ck::find::copyToArray(safe_custom_command_specification.startLocation, directory.string().c_str());
    safe_custom_command_specification.enableActionOptions = true;
    safe_custom_command_specification.actionOptions.execEnabled = true;
    ck::find::copyToArray(safe_custom_command_specification.actionOptions.execCommand, "ckvision.file-info");
    find.set_specification(safe_custom_command_specification);
    require(find.command_preview().find("/usr/bin/stat") != std::string::npos,
            "A permitted custom command must show its direct executable argv in the preview.");
    require(application.execute_command(find.execute_command()),
            "A permitted custom command must dispatch through the native command registry.");
    require(!execution.running(), "A custom command must show its exact sandboxed argv for explicit confirmation first.");
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    application.step(0);
    require(execution.running() && !execution.delete_matched_files(),
            "A confirmed custom command must not grant the file-deletion capability.");
    execution.finish({.exitCode = 0,
                      .matchCount = 1,
                      .customCommandInvocationCount = 1,
                      .customCommandAudit = "1 sandboxed custom command invocation(s)."});
    application.step(0);
    require(find.last_execution_result()->customCommandInvocationCount == 1,
            "A sandboxed custom-command audit result must return through the native presentation.");

    auto specification = ck::find::makeDefaultSpecification();
    ck::find::copyToArray(specification.startLocation, directory.string().c_str());
    ck::vision::ThreadedFindExecutionService threaded_execution;
    std::mutex completion_mutex;
    std::condition_variable completion_ready;
    std::optional<ck::find::SearchExecutionResult> threaded_result;
    threaded_execution.start(specification, false, [&](ck::find::SearchExecutionResult result) {
        {
            std::scoped_lock lock(completion_mutex);
            threaded_result = std::move(result);
        }
        completion_ready.notify_one();
    });
    {
        std::unique_lock lock(completion_mutex);
        require(completion_ready.wait_for(lock, std::chrono::seconds(2), [&] { return threaded_result.has_value(); }),
                "The threaded execution service must complete a filesystem search.");
    }
    require(threaded_result->exitCode == 0 && threaded_result->matchCount >= 1,
            "The threaded execution service must return the search-core result.");

    const fs::path spaced_match = directory / "match with spaces.txt";
    {
        std::ofstream file(spaced_match);
        file << "match";
    }
    auto custom_specification = ck::find::makeDefaultSpecification();
    ck::find::copyToArray(custom_specification.startLocation, directory.string().c_str());
    custom_specification.enableActionOptions = true;
    custom_specification.actionOptions.execEnabled = true;
    ck::find::copyToArray(custom_specification.actionOptions.execCommand, "ckvision.file-info");
    const ck::vision::FindCustomCommandPlan custom_plan = ck::vision::planFindCustomCommand(custom_specification);
    require(custom_plan.allowed && custom_plan.working_directory == fs::absolute(directory).lexically_normal() &&
                custom_plan.executable_argv == std::vector<std::string>{"/usr/bin/stat", "-f", "%N\\t%z\\t%Sp", "<matched-path>"},
            "The custom-command planner must accept only the fixed direct-argv template and selected search root.");
    auto unsafe_plan_specification = custom_specification;
    ck::find::copyToArray(unsafe_plan_specification.actionOptions.execCommand, "echo $(touch unsafe)");
    require(!ck::vision::planFindCustomCommand(unsafe_plan_specification).allowed,
            "The custom-command planner must refuse shell syntax and every non-allowlisted saved value.");
    unsafe_plan_specification = custom_specification;
    unsafe_plan_specification.actionOptions.execUsePlus = true;
    require(!ck::vision::planFindCustomCommand(unsafe_plan_specification).allowed,
            "The custom-command planner must refuse legacy batching variants that could alter argument scope.");

    auto platform_executor = ck::vision::makePlatformFindCustomCommandExecutor();
    const ck::vision::FindCustomCommandCapability platform_capability = platform_executor->capability(custom_specification);
    if (platform_capability.available)
    {
        require(platform_capability.argv_preview.size() >= 8 && platform_capability.argv_preview[0] == "/usr/bin/sandbox-exec" &&
                    platform_capability.argv_preview[1] == "-p" &&
                    platform_capability.argv_preview[2].find("(deny network*)") != std::string::npos &&
                    platform_capability.argv_preview[2].find("(deny file-write*)") != std::string::npos,
                "The production custom-command preview must expose the direct macOS sandbox argv and its no-network/no-write profile.");
        const std::atomic_bool not_cancelled{false};
        const ck::vision::FindCustomCommandOutcome platform_outcome =
            platform_executor->execute(custom_plan, spaced_match, not_cancelled);
        require(platform_outcome.invoked && !platform_outcome.cancelled && !platform_outcome.timed_out &&
                    !platform_outcome.output_truncated && platform_outcome.exit_code == 0,
                "An available production custom-command executor must run the allowlisted argv under the macOS sandbox.");
        const std::atomic_bool already_cancelled{true};
        const ck::vision::FindCustomCommandOutcome cancelled_outcome =
            platform_executor->execute(custom_plan, spaced_match, already_cancelled);
        require(!cancelled_outcome.invoked && cancelled_outcome.cancelled,
                "The production custom-command executor must honour cancellation before spawning a sandboxed child.");
    }
    else
    {
        require(!platform_capability.reason.empty(),
                "An unavailable production custom-command executor must explain why it remains disabled.");
    }

    auto recording_executor = std::make_unique<RecordingCustomCommandExecutor>();
    RecordingCustomCommandExecutor *const recording_executor_view = recording_executor.get();
    ck::vision::ThreadedFindExecutionService sandboxed_execution(std::move(recording_executor));
    std::optional<ck::find::SearchExecutionResult> custom_result;
    sandboxed_execution.start(custom_specification, false, [&](ck::find::SearchExecutionResult result) {
        {
            std::scoped_lock lock(completion_mutex);
            custom_result = std::move(result);
        }
        completion_ready.notify_one();
    });
    {
        std::unique_lock lock(completion_mutex);
        require(completion_ready.wait_for(lock, std::chrono::seconds(2), [&] { return custom_result.has_value(); }),
                "The sandboxed custom-command service must complete its bounded search workflow.");
    }
    require(custom_result->exitCode == 0 && custom_result->customCommandInvocationCount >= 1 &&
                custom_result->failedCustomCommandInvocationCount == 0 && recording_executor_view->invocations() >= 1 &&
                recording_executor_view->plan().executable_argv.front() == "/usr/bin/stat" &&
                recording_executor_view->matched_path().parent_path() == directory,
            "Custom command execution must pass a matched filesystem path as one literal direct argv value.");

    auto bounded_executor = std::make_unique<RecordingCustomCommandExecutor>();
    bounded_executor->set_outcome({.invoked = true,
                                   .cancelled = false,
                                   .timed_out = true,
                                   .output_truncated = true,
                                   .exit_code = 137,
                                   .output = {},
                                   .message = "Custom command exceeded its configured bounds."});
    ck::vision::ThreadedFindExecutionService bounded_execution(std::move(bounded_executor));
    std::optional<ck::find::SearchExecutionResult> bounded_result;
    bounded_execution.start(custom_specification, false, [&](ck::find::SearchExecutionResult result) {
        {
            std::scoped_lock lock(completion_mutex);
            bounded_result = std::move(result);
        }
        completion_ready.notify_one();
    });
    {
        std::unique_lock lock(completion_mutex);
        require(completion_ready.wait_for(lock, std::chrono::seconds(2), [&] { return bounded_result.has_value(); }),
                "The execution service must report bounded custom-command outcomes.");
    }
    require(bounded_result->exitCode != 0 && bounded_result->failedCustomCommandInvocationCount != 0 &&
                bounded_result->customCommandTimedOut && bounded_result->customCommandOutputTruncated &&
                !bounded_result->customCommandAudit.empty(),
            "Custom-command time and output limits must become a visible audit outcome instead of being silently ignored.");

    const fs::path deletable = directory / "delete-me.txt";
    {
        std::ofstream file(deletable);
        file << "delete";
    }
    std::optional<ck::find::SearchExecutionResult> deletion_result;
    threaded_execution.start(specification, true, [&](ck::find::SearchExecutionResult result) {
        {
            std::scoped_lock lock(completion_mutex);
            deletion_result = std::move(result);
        }
        completion_ready.notify_one();
    });
    {
        std::unique_lock lock(completion_mutex);
        require(completion_ready.wait_for(lock, std::chrono::seconds(2), [&] { return deletion_result.has_value(); }),
                "Confirmed deletion must complete through the injected execution service.");
    }
    require(!fs::exists(deletable) && !fs::exists(directory / "match.txt") && !fs::exists(spaced_match) && fs::exists(directory) &&
                deletion_result->deletedCount == 3 && deletion_result->failedDeletionCount == 0,
            "Confirmed deletion must remove matching regular files without deleting the containing directory.");
    fs::remove_all(directory);
}
