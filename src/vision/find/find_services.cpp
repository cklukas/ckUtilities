// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "find_services.hpp"

#include "ck/file_io.hpp"
#include "host_about.hpp"
#include "native_host.hpp"

#include <chrono>
#include <filesystem>
#include <utility>

namespace ck::vision
{

namespace
{

constexpr std::size_t kMaximumCustomCommandInvocations = 64;
constexpr std::size_t kMaximumAggregatedCommandOutputBytes = 64 * 1024;
constexpr std::size_t kMaximumCapturedMatches = 10000;

bool remove_non_directory_entry(const std::filesystem::path &path)
{
#if defined(_WIN32)
    if (!ck::io::is_regular_file(path))
        return false;
    return ck::io::remove_regular_file(path);
#else
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error)
        return false;
    if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status))
        return false;
    return std::filesystem::remove(path, error) && !error;
#endif
}

} // namespace

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

bool CoreFindSpecificationStore::remove(const std::string &name)
{
    return ck::find::removeSpecification(name);
}

ThreadedFindExecutionService::ThreadedFindExecutionService()
    : ThreadedFindExecutionService(makePlatformFindCustomCommandExecutor())
{
}

ThreadedFindExecutionService::ThreadedFindExecutionService(std::unique_ptr<FindCustomCommandExecutor> custom_command_executor)
    : custom_command_executor_(std::move(custom_command_executor))
{
}

ThreadedFindExecutionService::~ThreadedFindExecutionService()
{
    cancel();
    std::scoped_lock lock(mutex_);
    if (worker_.joinable())
        worker_.join();
}

FindCustomCommandCapability ThreadedFindExecutionService::custom_command_capability(
    const ck::find::SearchSpecification &specification) const
{
    if (custom_command_executor_ == nullptr)
    {
        return {.available = false,
                .reason = "No sandboxed custom-command executor is configured.",
                .argv_preview = {}};
    }
    return custom_command_executor_->capability(specification);
}

bool ThreadedFindExecutionService::delete_single_file(const std::string &path)
{
    return remove_non_directory_entry(ck::host::utf8_path(path));
}

void ThreadedFindExecutionService::start(ck::find::SearchSpecification specification, bool delete_matched_files,
                                         CompletionHandler on_complete)
{
    start_with_progress(std::move(specification), delete_matched_files, {}, std::move(on_complete));
}

void ThreadedFindExecutionService::start_with_progress(ck::find::SearchSpecification specification,
                                                       bool delete_matched_files,
                                                       ProgressHandler on_progress,
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
    const bool custom_command_enabled = specification.enableActionOptions && specification.actionOptions.execEnabled;
    const FindCustomCommandPlan custom_command_plan = custom_command_enabled ? planFindCustomCommand(specification) : FindCustomCommandPlan{};
    const FindCustomCommandCapability custom_command_capability = custom_command_enabled
                                                                       ? this->custom_command_capability(specification)
                                                                       : FindCustomCommandCapability{};
    running_.store(true, std::memory_order_release);
    std::jthread worker([this, cancellation, spec = std::move(specification), delete_matched_files,
                         custom_command_enabled, custom_command_plan, custom_command_capability,
                         progress_cb = std::move(on_progress),
                         complete_cb = std::move(on_complete)]() mutable {
        if (custom_command_enabled && delete_matched_files)
        {
            ck::find::SearchExecutionResult result;
            result.exitCode = 1;
            result.customCommandAudit = "Custom commands and deletion cannot be combined in one execution.";
            running_.store(false, std::memory_order_release);
            if (complete_cb)
                complete_cb(std::move(result));
            return;
        }
        if (custom_command_enabled && (!custom_command_plan.allowed || !custom_command_capability.available || custom_command_executor_ == nullptr))
        {
            ck::find::SearchExecutionResult result;
            result.exitCode = 1;
            result.customCommandAudit = !custom_command_capability.reason.empty() ? custom_command_capability.reason
                                                                                     : custom_command_plan.reason;
            running_.store(false, std::memory_order_release);
            if (complete_cb)
                complete_cb(std::move(result));
            return;
        }

        ck::find::SearchExecutionOptions options;
        // The search core remains non-destructive. This application adapter is
        // the only place that may apply a confirmed, deliberately narrow file
        // action while a match is being discovered.
        options.includeActions = false;
        options.captureMatches = true;
        options.maxCapturedMatches = kMaximumCapturedMatches;
        options.filterContent = true;
        options.cancellation_requested = [cancellation] {
            return cancellation->load(std::memory_order_acquire);
        };

        auto last_progress = std::chrono::steady_clock::time_point::min();
        if (progress_cb)
        {
            options.on_progress = [&progress_cb, &last_progress, &cancellation](const ck::find::SearchProgressSnapshot &snapshot) {
                if (cancellation->load(std::memory_order_acquire))
                    return;
                const auto now = std::chrono::steady_clock::now();
                if (last_progress != std::chrono::steady_clock::time_point::min() &&
                    now - last_progress < std::chrono::milliseconds(40))
                {
                    return;
                }
                last_progress = now;
                progress_cb(snapshot);
            };
        }

        std::size_t deleted_count = 0;
        std::size_t failed_deletion_count = 0;
        std::size_t custom_command_invocation_count = 0;
        std::size_t failed_custom_command_invocation_count = 0;
        bool custom_command_cancelled = false;
        bool custom_command_timed_out = false;
        bool custom_command_output_truncated = false;
        bool custom_command_limit_reached = false;
        std::string custom_command_audit;
        std::string custom_command_output;
        if (delete_matched_files)
        {
            options.on_match = [cancellation, &deleted_count, &failed_deletion_count](const std::filesystem::path &path) {
                if (cancellation->load(std::memory_order_acquire))
                    return;
#if defined(_WIN32)
                if (!ck::io::is_regular_file(path))
                    return;
                if (ck::io::remove_regular_file(path))
                    ++deleted_count;
                else
                    ++failed_deletion_count;
#else
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
#endif
            };
        }
        else if (custom_command_enabled)
        {
            options.on_match = [this,
                                cancellation,
                                custom_command_plan,
                                &custom_command_invocation_count,
                                &failed_custom_command_invocation_count,
                                &custom_command_cancelled,
                                &custom_command_timed_out,
                                &custom_command_output_truncated,
                                &custom_command_limit_reached,
                                &custom_command_audit,
                                &custom_command_output](const std::filesystem::path &path) {
                if (cancellation->load(std::memory_order_acquire))
                    return;
                if (custom_command_invocation_count >= kMaximumCustomCommandInvocations)
                {
                    custom_command_limit_reached = true;
                    return;
                }
                ++custom_command_invocation_count;
                FindCustomCommandOutcome outcome = custom_command_executor_->execute(custom_command_plan, path, *cancellation);
                custom_command_cancelled = custom_command_cancelled || outcome.cancelled;
                custom_command_timed_out = custom_command_timed_out || outcome.timed_out;
                custom_command_output_truncated = custom_command_output_truncated || outcome.output_truncated;
                if (!outcome.invoked || outcome.exit_code != 0 || outcome.cancelled || outcome.timed_out || outcome.output_truncated)
                    ++failed_custom_command_invocation_count;
                custom_command_audit = outcome.message;
                if (!outcome.output.empty() && custom_command_output.size() < kMaximumAggregatedCommandOutputBytes)
                {
                    std::string line = outcome.output;
                    if (line.back() != '\n')
                        line.push_back('\n');
                    const std::size_t remaining = kMaximumAggregatedCommandOutputBytes - custom_command_output.size();
                    if (line.size() <= remaining)
                        custom_command_output.append(line);
                    else
                    {
                        custom_command_output.append(line.data(), remaining);
                        custom_command_output_truncated = true;
                    }
                }
            };
        }

        auto result = ck::find::executeSpecification(spec, options);
        result.deletedCount = deleted_count;
        result.failedDeletionCount = failed_deletion_count;
        result.customCommandInvocationCount = custom_command_invocation_count;
        result.failedCustomCommandInvocationCount = failed_custom_command_invocation_count;
        result.customCommandCancelled = custom_command_cancelled;
        result.customCommandTimedOut = custom_command_timed_out;
        result.customCommandOutputTruncated = custom_command_output_truncated;
        result.customCommandOutput = std::move(custom_command_output);
        if (custom_command_enabled)
        {
            result.customCommandAudit = std::to_string(custom_command_invocation_count) + " sandboxed custom command invocation(s)";
            if (custom_command_limit_reached)
                result.customCommandAudit += "; stopped after the 64-invocation safety limit";
            if (!custom_command_audit.empty())
                result.customCommandAudit += "; last outcome: " + custom_command_audit;
            if (failed_custom_command_invocation_count != 0 && result.exitCode == 0)
                result.exitCode = 1;
        }
        running_.store(false, std::memory_order_release);
        if (complete_cb)
            complete_cb(std::move(result));
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
