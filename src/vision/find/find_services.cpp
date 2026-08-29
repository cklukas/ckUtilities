#include "find_services.hpp"

#include <filesystem>
#include <utility>

namespace ck::vision
{

std::vector<ck::find::SavedSpecification> CoreFindSpecificationStore::list() const
{
    return ck::find::listSavedSpecifications();
}

std::optional<ck::find::SearchSpecification> CoreFindSpecificationStore::load(const std::string &name) const
{
    return ck::find::loadSpecification(name);
}

bool CoreFindSpecificationStore::save(const ck::find::SearchSpecification &specification, const std::string &name)
{
    return ck::find::saveSpecification(specification, name);
}

ThreadedFindExecutionService::~ThreadedFindExecutionService()
{
    cancel();
    std::scoped_lock lock(mutex_);
    if (worker_.joinable())
        worker_.join();
}

void ThreadedFindExecutionService::start(ck::find::SearchSpecification specification, bool delete_matched_files,
                                         CompletionHandler on_complete)
{
    cancel();

    std::jthread previous;
    {
        std::scoped_lock lock(mutex_);
        previous = std::move(worker_);
    }
    if (previous.joinable())
        previous.join();

    auto cancellation = std::make_shared<std::atomic_bool>(false);
    running_.store(true, std::memory_order_release);
    std::jthread worker([this, cancellation, specification = std::move(specification), delete_matched_files,
                         on_complete = std::move(on_complete)]() mutable {
        ck::find::SearchExecutionOptions options;
        // The search core remains non-destructive. This application adapter is
        // the only place that may apply a confirmed, deliberately narrow file
        // action while a match is being discovered.
        options.includeActions = false;
        options.captureMatches = true;
        options.maxCapturedMatches = 200;
        options.filterContent = true;
        options.cancellation_requested = [cancellation] {
            return cancellation->load(std::memory_order_acquire);
        };

        std::size_t deleted_count = 0;
        std::size_t failed_deletion_count = 0;
        if (delete_matched_files)
        {
            options.on_match = [cancellation, &deleted_count, &failed_deletion_count](const std::filesystem::path &path) {
                if (cancellation->load(std::memory_order_acquire))
                    return;
                std::error_code error;
                const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
                if (error)
                {
                    ++failed_deletion_count;
                    return;
                }
                if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status))
                    return;
                if (std::filesystem::remove(path, error) && !error)
                    ++deleted_count;
                else
                    ++failed_deletion_count;
            };
        }

        auto result = ck::find::executeSpecification(specification, options);
        result.deletedCount = deleted_count;
        result.failedDeletionCount = failed_deletion_count;
        running_.store(false, std::memory_order_release);
        if (on_complete)
            on_complete(std::move(result));
    });

    {
        std::scoped_lock lock(mutex_);
        cancellation_ = std::move(cancellation);
        worker_ = std::move(worker);
    }
}

void ThreadedFindExecutionService::cancel() noexcept
{
    std::shared_ptr<std::atomic_bool> cancellation;
    {
        std::scoped_lock lock(mutex_);
        cancellation = cancellation_;
    }
    if (cancellation)
        cancellation->store(true, std::memory_order_release);
}

bool ThreadedFindExecutionService::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

} // namespace ck::vision
