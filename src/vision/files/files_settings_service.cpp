// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_settings_service.hpp"
#include <cvision/core/utf8.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace ck::vision {
namespace {
struct SnapshotSource {
    std::array<const ck::files::PaneView *, 2> views{};
    std::array<std::span<const FilesPaneLocation>, 2> owned{};
    std::array<std::span<const std::shared_ptr<const FilesPaneLocation>>, 2> shared{};
    bool borrowed = false;
    explicit SnapshotSource(const FilesPersistentSettings &s) {
        for (std::size_t i=0;i<2;++i) { views[i]=&s.views[i]; owned[i]=s.bookmarks[i]; }
    }
    explicit SnapshotSource(const FilesSettingsSnapshotView &s):views(s.views),shared(s.bookmarks),borrowed(true) {}
    std::size_t count(std::size_t i) const noexcept {return borrowed?shared[i].size():owned[i].size();}
    const FilesPaneLocation *location(std::size_t i,std::size_t j) const noexcept {
        return borrowed?shared[i][j].get():&owned[i][j];
    }
};
FilesSettingsAdmission validate(const SnapshotSource &s,std::size_t append_pane=2,
                                 const FilesPaneLocation *append=nullptr) noexcept {
    using A=FilesSettingsAdmission;
    if(append && append_pane>=2)return A::Invalid;
    std::size_t bytes=std::string_view("CKFILES_SETTINGS 1\n").size()+std::string_view("END\n").size();
    const auto add=[&](std::size_t amount) {
        if(amount>FilesSettingsBounds::bytes-bytes)return false;
        bytes+=amount;return true;
    };
    const auto field=[&](std::size_t units,std::size_t encoded_width) {
        return units<=FilesSettingsBounds::native_units_per_field &&
            units<=(FilesSettingsBounds::bytes-bytes)/encoded_width && add(units*encoded_width);
    };
    const auto bookmark=[&](const FilesPaneLocation *v) {
        if(!v || !v->root_address().is_valid())return A::Invalid;
        if(v->native_bytes()>ck::files::NativePathLimits{}.max_native_bytes ||
            v->components().size()>FilesPaneLocation::hard_components ||
            !add(std::string_view("BOOKMARK 0 P ").size()))return A::Limit;
        const auto &root=v->root_address();
        const bool posix=root.posix_bytes()!=nullptr;
        if(!posix && !root.windows_units())return A::Invalid;
        if(!field(posix?root.posix_bytes()->size():root.windows_units()->size(),posix?2U:4U))return A::Limit;
        for(const auto &name:v->components()) {
            if(!name.is_valid() || name.encoding()!=root.encoding())return A::Invalid;
            if(!add(1) || !field(posix?name.posix_bytes()->size():name.windows_units()->size(),posix?2U:4U))return A::Limit;
        }
        return add(1)?A::Accepted:A::Limit;
    };
    for(std::size_t i=0;i<2;++i) {
        const auto *v=s.views[i];
        if(!v)return A::Invalid;
        if(v->display_substring.size()>FilesSettingsBounds::filter_bytes ||
            s.count(i)>FilesSettingsBounds::bookmarks_per_pane ||
            (append && append_pane==i && s.count(i)==FilesSettingsBounds::bookmarks_per_pane))return A::Limit;
        if(!ck::files::valid_pane_order({v->sort,v->descending,v->folders_first}) ||
            !ckv::utf8::is_valid(v->display_substring))return A::Invalid;
        if(!add(std::string_view("VIEW 0 0 0 0 0 0 ").size()) ||
            !field(v->display_substring.size(),2) || !add(1))return A::Limit;
        for(std::size_t j=0;j<s.count(i);++j) {
            const auto checked=bookmark(s.location(i,j));if(checked!=A::Accepted)return checked;
        }
        if(append && append_pane==i) {
            const auto checked=bookmark(append);if(checked!=A::Accepted)return checked;
        }
    }
    return A::Accepted;
}
struct Request { std::uint64_t revision=0; FilesPersistentSettings value; };
} // namespace

std::string_view files_settings_admission_message(FilesSettingsAdmission state) noexcept {
    switch(state) {
    case FilesSettingsAdmission::Accepted:return {};
    case FilesSettingsAdmission::Closed:return "Settings are closing; change not queued for saving";
    case FilesSettingsAdmission::Limit:return "Settings limit exceeded; change not saved (1 MiB, 256 bookmarks per pane, 4096 filter bytes)";
    case FilesSettingsAdmission::Invalid:return "Invalid settings snapshot; change not saved";
    case FilesSettingsAdmission::NoMemory:return "Not enough memory to queue settings; change not saved";
    case FilesSettingsAdmission::WorkerFault:return "Settings worker unavailable; change not saved";
    case FilesSettingsAdmission::RevisionExhausted:return "Settings revision exhausted; change not saved";
    }
    return "Settings change not saved";
}
std::string_view files_settings_write_message(FilesSettingsWriteState state) noexcept {
    switch(state) {
    case FilesSettingsWriteState::Saved:return {};
    case FilesSettingsWriteState::StoreError:return "Settings save did not confirm success";
    case FilesSettingsWriteState::NoMemory:return "Settings save ran out of memory; saved state is not confirmed";
    case FilesSettingsWriteState::WorkerFault:return "Settings worker failed; saved state is not confirmed";
    }
    return "Settings save not confirmed";
}
FilesSettingsAdmission validate_files_settings_snapshot(const FilesSettingsSnapshotView &s,
    std::size_t pane,const FilesPaneLocation *append) noexcept {return validate(SnapshotSource(s),pane,append);}

struct FilesSettingsService::State {
    FilesSettingsStore &store;
    std::function<void()> wake;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<Request> pending;
    std::optional<FilesSettingsReply> reply;
    bool reply_pending=false;
    std::atomic<std::uint64_t> accepted{0},finished{0},saved{0};
    std::atomic<bool> closing{false},done{false},wake_failed{false},fatal{false},fatal_taken{false};
    std::thread worker; // Last: every field is initialized before run can start.
    State(FilesSettingsStore &s,std::function<void()> w):store(s),wake(std::move(w)),worker([this]{run();}) {}
    void notify() noexcept {
        try {if(wake)wake();} catch(...) {wake_failed.store(true,std::memory_order_release);}
    }
    FilesSettingsSubmitted admit(const SnapshotSource &s) noexcept {
        try {
            if(closing.load(std::memory_order_acquire))return {FilesSettingsAdmission::Closed,0};
            if(fatal.load(std::memory_order_acquire))return {FilesSettingsAdmission::WorkerFault,0};
            const auto checked=validate(s);if(checked!=FilesSettingsAdmission::Accepted)return {checked,0};
            auto candidate=std::make_unique<Request>();
            for(std::size_t i=0;i<2;++i) {
                candidate->value.views[i]=*s.views[i];
                candidate->value.bookmarks[i].reserve(s.count(i));
                for(std::size_t j=0;j<s.count(i);++j)candidate->value.bookmarks[i].push_back(*s.location(i,j));
            }
            std::uint64_t revision=0;
            {
                std::lock_guard lock(mutex); // Only POD/pointer moves; never I/O or wake.
                if(closing.load(std::memory_order_relaxed))return {FilesSettingsAdmission::Closed,0};
                if(fatal.load(std::memory_order_relaxed))return {FilesSettingsAdmission::WorkerFault,0};
                const auto previous=accepted.load(std::memory_order_relaxed);
                if(previous==std::numeric_limits<std::uint64_t>::max())return {FilesSettingsAdmission::RevisionExhausted,0};
                revision=previous+1;candidate->revision=revision;
                candidate.swap(pending); // Destroy the superseded payload OUTSIDE the lock.
                accepted.store(revision,std::memory_order_release);
            }
            ready.notify_one();return {FilesSettingsAdmission::Accepted,revision};
        } catch(const std::bad_alloc &) {return {FilesSettingsAdmission::NoMemory,0};}
          catch(...) {return {FilesSettingsAdmission::WorkerFault,0};}
    }
    void close() noexcept {
        {std::lock_guard lock(mutex);closing.store(true,std::memory_order_release);}
        ready.notify_one(); // Predicate and notification cannot race past wait's unlock.
    }
    void run() noexcept {
        try {
            for(;;) {
                std::unique_ptr<Request> request;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock,[this]{return pending || closing.load(std::memory_order_acquire);});
                    if(!pending)break;
                    request=std::move(pending);
                }
                FilesSettingsReply result;result.revision=request->revision;
                try {
                    const auto diagnostic=store.save(request->value); // SOLE persistence call, on worker.
                    result.state=diagnostic.empty()?FilesSettingsWriteState::Saved:FilesSettingsWriteState::StoreError;
                    // Store's legacy string result cannot prove whether an I/O
                    // error occurred before or after publication. Never infer no effect.
                    result.outcome_unknown=!diagnostic.empty();
                    result.diagnostic_size=std::min(diagnostic.size(),result.diagnostic.size());
                    std::copy_n(diagnostic.data(),result.diagnostic_size,result.diagnostic.data());
                    result.diagnostic_truncated=diagnostic.size()>result.diagnostic.size();
                } catch(const std::bad_alloc &) {
                    result.state=FilesSettingsWriteState::NoMemory;result.outcome_unknown=true;
                } catch(...) {result.state=FilesSettingsWriteState::WorkerFault;result.outcome_unknown=true;}
                request.reset(); // Retire payload before publishing completion/drain.
                {
                    std::lock_guard lock(mutex);
                    reply=result;reply_pending=true; // Fixed POD slot; publish even under persistent OOM.
                    if(result.state==FilesSettingsWriteState::Saved)saved.store(result.revision,std::memory_order_release);
                    finished.store(result.revision,std::memory_order_release);
                }
                notify();
            }
        } catch(...) {fatal.store(true,std::memory_order_release);closing.store(true,std::memory_order_release);}
        notify();done.store(true,std::memory_order_release); // No subsequent I/O or wake.
    }
};
FilesSettingsService::FilesSettingsService(FilesSettingsStore &store,std::function<void()> wake)
    :state_(std::make_unique<State>(store,std::move(wake))) {}
FilesSettingsService::~FilesSettingsService(){request_close();join();}
FilesSettingsSubmitted FilesSettingsService::submit(const FilesPersistentSettings &s) noexcept {return state_->admit(SnapshotSource(s));}
FilesSettingsSubmitted FilesSettingsService::submit(const FilesSettingsSnapshotView &s) noexcept {return state_->admit(SnapshotSource(s));}
std::optional<FilesSettingsReply> FilesSettingsService::take() noexcept {
    if(state_->fatal.load(std::memory_order_acquire)) {
        if(state_->fatal_taken.exchange(true,std::memory_order_acq_rel))return {};
        FilesSettingsReply result;result.revision=state_->accepted.load(std::memory_order_acquire);
        result.state=FilesSettingsWriteState::WorkerFault;result.outcome_unknown=true;return result;
    }
    try {
        std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock())return {};
        if(!state_->reply_pending)return {};
        auto result=state_->reply;state_->reply_pending=false;return result;
    }catch(...) {return {};}
}
std::optional<FilesSettingsReply> FilesSettingsService::latest_result() const noexcept {
    if(state_->fatal.load(std::memory_order_acquire)) {
        FilesSettingsReply result;result.revision=state_->accepted.load(std::memory_order_acquire);
        result.state=FilesSettingsWriteState::WorkerFault;result.outcome_unknown=true;return result;
    }
    try {
        std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock())return {};
        return state_->reply;
    }catch(...) {return {};}
}
FilesSettingsProgress FilesSettingsService::progress() const noexcept {
    return {state_->accepted.load(std::memory_order_acquire),state_->finished.load(std::memory_order_acquire),
        state_->saved.load(std::memory_order_acquire),state_->closing.load(std::memory_order_acquire),
        state_->done.load(std::memory_order_acquire),state_->wake_failed.load(std::memory_order_acquire),
        state_->fatal.load(std::memory_order_acquire)};
}
void FilesSettingsService::request_close() noexcept {state_->close();}
bool FilesSettingsService::drained() const noexcept {return state_->done.load(std::memory_order_acquire);}
void FilesSettingsService::join(){if(state_->worker.joinable())state_->worker.join();}
} // namespace ck::vision
