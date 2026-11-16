// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ck/find/search_backend.hpp"
#include "find_custom_command.hpp"

namespace ck::vision
{

// Platform policy for saved searches.  Presentation code only sees this
// narrow capability; it does not select a directory or perform filesystem IO.
class FindSpecificationStore
{
public:
    virtual ~FindSpecificationStore() = default;

    virtual std::vector<ck::find::SavedSpecification> list() const = 0;
    virtual std::optional<ck::find::SearchSpecification> load(const std::string &name) const = 0;
    virtual bool save(const ck::find::SearchSpecification &specification, const std::string &name) = 0;
    virtual bool remove(const std::string &name)
    {
        (void)name;
        return false;
    }
};

// The production adapter retains the established, JSON-backed storage format.
// It belongs at the composition root, never in a view or dialog handler.
class CoreFindSpecificationStore final : public FindSpecificationStore
{
public:
    std::vector<ck::find::SavedSpecification> list() const override;
    std::optional<ck::find::SearchSpecification> load(const std::string &name) const override;
    bool save(const ck::find::SearchSpecification &specification, const std::string &name) override;
    bool remove(const std::string &name) override;
};

// Execution is an application service rather than a widget behavior.  It may
// invoke its callbacks from a worker thread, so a presentation adapter must
// marshal them with Application::post() before touching ckVision views.
class FindExecutionService
{
public:
    using CompletionHandler = std::function<void(ck::find::SearchExecutionResult)>;
    using ProgressHandler = std::function<void(ck::find::SearchProgressSnapshot)>;

    virtual ~FindExecutionService() = default;

    // Deletion is an explicit execution capability granted only after the UI
    // has confirmed the specification. Sandboxed custom commands use the
    // separately queried capability below; all other actions remain disabled.
    virtual void start(ck::find::SearchSpecification specification, bool delete_matched_files,
                       CompletionHandler on_complete) = 0;
    virtual void start_with_progress(ck::find::SearchSpecification specification,
                                     bool delete_matched_files,
                                     ProgressHandler on_progress,
                                     CompletionHandler on_complete)
    {
        (void)on_progress;
        start(std::move(specification), delete_matched_files, std::move(on_complete));
    }
    virtual FindCustomCommandCapability custom_command_capability(const ck::find::SearchSpecification &specification) const = 0;
    virtual bool delete_single_file(const std::string &path)
    {
        (void)path;
        return false;
    }
    virtual void cancel() noexcept = 0;
    virtual bool running() const noexcept = 0;
};

// A deliberately small native host adapter.  The core receives a cancellation
// probe and contains no thread or terminal dependency.  Destruction cancels
// and joins the worker before the owning Application can be destroyed.
class ThreadedFindExecutionService final : public FindExecutionService
{
public:
    ThreadedFindExecutionService();
    explicit ThreadedFindExecutionService(std::unique_ptr<FindCustomCommandExecutor> custom_command_executor);
    ~ThreadedFindExecutionService() override;

    void start(ck::find::SearchSpecification specification, bool delete_matched_files,
               CompletionHandler on_complete) override;
    void start_with_progress(ck::find::SearchSpecification specification,
                             bool delete_matched_files,
                             ProgressHandler on_progress,
                             CompletionHandler on_complete) override;
    FindCustomCommandCapability custom_command_capability(const ck::find::SearchSpecification &specification) const override;
    bool delete_single_file(const std::string &path) override;
    void cancel() noexcept override;
    bool running() const noexcept override;

private:
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::atomic_bool running_{false};
    std::unique_ptr<FindCustomCommandExecutor> custom_command_executor_;
};

} // namespace ck::vision
