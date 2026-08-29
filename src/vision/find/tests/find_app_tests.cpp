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

void verify_late_execution_delivery_is_lifetime_safe()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    MemoryStore specifications;
    ManualExecution execution;
    {
        ck::vision::FindApp find(application, specifications, execution);
        require(application.execute_command(find.execute_command()) && find.execution_running(),
                "The test requires an active native find execution.");
    }
    require(execution.cancelled(), "Destroying Find must request execution cancellation.");
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
    require(!fs::exists(deletable) && !fs::exists(directory / "match.txt") && fs::exists(directory) &&
                deletion_result->deletedCount == 2 && deletion_result->failedDeletionCount == 0,
            "Confirmed deletion must remove matching regular files without deleting the containing directory.");
    fs::remove_all(directory);
}
