#include "find_services.hpp"

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

void ThreadedFindExecutionService::start(ck::find::SearchSpecification specification, CompletionHandler on_complete)
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
    std::jthread worker([this, cancellation, specification = std::move(specification), on_complete = std::move(on_complete)]() mutable {
        ck::find::SearchExecutionOptions options;
        // Interactive execution is intentionally a non-destructive search:
        // action mutations retain their command-preview role until their own
        // confirmation workflow is migrated.
        options.includeActions = false;
        options.captureMatches = true;
        options.maxCapturedMatches = 200;
        options.filterContent = true;
        options.cancellation_requested = [cancellation] {
            return cancellation->load(std::memory_order_acquire);
        };

        auto result = ck::find::executeSpecification(specification, options);
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
