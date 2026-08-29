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
    void start(ck::find::SearchSpecification, CompletionHandler on_complete) override
    {
        running_ = true;
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

private:
    bool running_ = false;
    bool cancelled_ = false;
    CompletionHandler on_complete_;
};
}

int main()
{
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

    auto specification = ck::find::makeDefaultSpecification();
    ck::find::copyToArray(specification.startLocation, directory.string().c_str());
    ck::vision::ThreadedFindExecutionService threaded_execution;
    std::mutex completion_mutex;
    std::condition_variable completion_ready;
    std::optional<ck::find::SearchExecutionResult> threaded_result;
    threaded_execution.start(specification, [&](ck::find::SearchExecutionResult result) {
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
    fs::remove_all(directory);
}
